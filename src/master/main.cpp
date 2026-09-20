// Master firmware (headless dev build on AtomS3; Guition display added in
// Phase 3). Scans Victron BLE advertisements into an NVS-backed registry and
// serves a WiFi-AP web app:
//   /          mimic (energy-flow diagram), live
//   /devices   add / edit / delete devices + adopt discovered ones
//   /bindings  map panel signals -> device fields
//   /api/data  legacy JSON snapshot (slaves)
//   /api/panel resolved panel signals for the mimic / display
//
// Build/flash:  pio run -e atoms3 -t upload   (then tools/monitor.py)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <esp_wifi.h>

#include <cmath>
#include <cstring>
#include <string>

#include "DeviceConfig.h"
#include "Profiles.h"
#include "Registry.h"
#include "Signals.h"
#include "SlaveLink.h"
#include "SlaveReceiver.h"
#include "Stats.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "VictronTypes.h"
#include "app.h"
#include <rom/rtc.h>          // shared state/types + the board seam (VICMON_DISPLAY)
#include "web_assets.h"  // kStyle / kMimicPage / kStatsPage / kDiagPage (HTML/CSS/JS)

// AP SSID is made unique per device at boot (Vicmon-<last 3 MAC bytes>) so
// several masters in the same area don't collide — filled in setup() once the
// master id is known; the default is only a placeholder before then.
char kApSsid[24] = "Vicmon";         // default Vicmon-<mac3>; overridable via NVS (loadApCfg)
char kApPass[24] = "vicmon1234";     // >= 8 chars; overridable via NVS (loadApCfg)
const char* kFwVersion = "0.7.24";    // shown on the display Settings page + OTA version compare

DeviceConfig gConfig;
sig::SignalMap gSignals;
ProfileManager gProfiles;
stats::Stats gStats;
NimBLEScan* gScan = nullptr;
extern NimBLEAdvertisedDeviceCallbacks& gIngestCb;  // ble_ingest.cpp
AsyncWebServer gServer(80);
DNSServer gDns;
SemaphoreHandle_t gRegMux = nullptr;  // registry mutex (see app.h RegLock)


// Victron devices seen but not configured (no matching key). (struct in app.h)
Discovered gDisc[12];
size_t gDiscN = 0;

// ESP-NOW master identity + pairing window (used by the broadcaster below and by
// the diagnostics view). Declared here because ingest() and the debug-capture
// path reference gDebugCapture before the ESP-NOW section.
uint32_t gMasterId = 0;              // stable per-chip id (low 32b of efuse MAC)
volatile uint32_t gPairUntilMs = 0;  // pairing window closes at this millis (0 = closed)
volatile bool gDebugCapture = false; // capture raw bytes of unknown adverts for troubleshooting
bool gEspNowOk = false;              // ESP-NOW radio up (broadcaster in master, receiver in slave)
volatile uint16_t gSnapSeq = 0;      // broadcast sequence counter (esp_timer task)

// Open a 60 s pairing window: while it's up the broadcast sets F_PAIRING so an
// adopting slave will accept this master's id (see lib/slavelink/SlaveLink.h).
void startPairing() { gPairUntilMs = millis() + 60000; }
bool pairingActive() {
    uint32_t u = gPairUntilMs;
    return u != 0 && (int32_t)(u - millis()) > 0;  // wrap-safe: window is only 60 s
}
int pairSecsLeft() {
    return pairingActive() ? (int)((gPairUntilMs - millis()) / 1000) : 0;
}
// Close the window early (a second press of the pair button = "never mind").
void stopPairing() { gPairUntilMs = 0; }

// Role-aware pairing, for the callers that just want "the pair button" and don't
// care which end of the link this board is. Pairing is two-sided: a MASTER opens
// an advertising window, a SLAVE arms adoption of whichever master it hears
// advertising. The web Pair button already branches this way (web.cpp); these two
// wrap it so the physical button and the LED agree with it.
bool pairingModeActive() {
    return gRole == ROLE_SLAVE ? gRx.isAdopting() : pairingActive();
}
void togglePairingMode() {
    if (gRole == ROLE_SLAVE) {
        if (gRx.isAdopting()) gRx.stopAdopt(); else gRx.startAdopt();
    } else {
        if (pairingActive()) stopPairing(); else startPairing();
    }
}

// ---- display hardware detection (Guition vs LilyGo) ------------------------
// The universal image compiles both display backends; pick the right one at boot
// so one image + the OTA firmware-clone serve every board. The two boards are
// told apart by the Guition's AXS15231B capacitive touch controller (I2C 0x3B on
// SDA=4/SCL=8): it ACKs on the Guition and is absent on the LilyGo (buttons
// variant). The probe runs before ANY panel init, so GPIO8 — the LilyGo's parallel
// bus WR — is free to borrow as I2C SCL here; we release it (Wire.end + gpio
// reset) so the LilyGo bus can claim it. An NVS override (ns "vicboard" key "hw":
// 1=Guition 2=LilyGo 3=headless) wins if set, for a misdetect or bench forcing.
uint8_t gHwBoard = HW_HEADLESS;
void detectBoard() {
    // NVS override (ns "vicboard" key "hw") wins for a misdetect or bench forcing:
    // 1=Guition 2=LilyGo 3=headless 4=M5Capsule. Checked before any probing.
    {
        Preferences p;
        p.begin("vicboard", true);
        uint8_t f = p.getUChar("hw", 0);
        p.end();
        if (f) {
            gHwBoard = (f == 1) ? HW_GUITION
                     : (f == 2) ? HW_LILYGO
                     : (f == 4) ? HW_M5CAPSULE
                                : HW_HEADLESS;
            Serial.printf("[board] forced hw=%u\n", gHwBoard);
#ifdef VICMON_HAS_M5CAPSULE
            if (gHwBoard == HW_M5CAPSULE) capsuleProbe();  // start the RTC bus for bringUp
#endif
            return;
        }
    }
#if defined(VICMON_HAS_GUITION) && defined(VICMON_HAS_LILYGO)
    // Sample GPIO4 BEFORE any I2C probing. The M5Capsule probe below clocks GPIO8,
    // which is the Guition's touch I2C SCL; a partial transaction there can leave the
    // touch controller holding SDA (= GPIO4) low, so a read taken afterwards sees ~0 mV
    // and misdetects a Guition as a LilyGo. Taken first, the level is the one the
    // board's own pull-up/divider sets:
    //   • Guition: GPIO4 = touch I2C SDA, pulled up to ~3.3V   -> ~3100 mV
    //   • LilyGo:  GPIO4 = VBAT/2 battery divider              -> ~1800..2300 mV
    // Average a few ADC reads; > threshold => the pulled-up Guition line.
    pinMode(4, INPUT);
    uint32_t gpio4mv = 0;
    for (int i = 0; i < 8; ++i) gpio4mv += analogReadMilliVolts(4);
    gpio4mv /= 8;
    // Release GPIO4 from the ADC so the Guition touch's Wire.begin(4,8) can claim it
    // cleanly — without this the pin stays attached to ADC1 and I2C touch init fails.
    gpio_reset_pin((gpio_num_t)4);
#endif
#ifdef VICMON_HAS_M5CAPSULE
    // Positive probe first: the M5Capsule's BM8563 RTC ACKs at 0x51 on the internal
    // I2C bus (SDA=8/SCL=10); nothing on the Guition or LilyGo answers there. On a
    // miss, release GPIO8/10 so the Guition touch (Wire on 4/8) or the LilyGo bus
    // (WR on 8) can claim them cleanly, then fall through to the panel detection.
    if (capsuleProbe()) {
        gHwBoard = HW_M5CAPSULE;
        Serial.println("[board] auto-detect: M5CAPSULE (BM8563 @0x51)");
        return;
    }
    Wire.end();
    gpio_reset_pin((gpio_num_t)8);
    gpio_reset_pin((gpio_num_t)10);
#endif
#if defined(VICMON_HAS_GUITION) && defined(VICMON_HAS_LILYGO)
    // Distinguish by the DC level on GPIO4, sampled above before any I2C probing —
    // a genuine hardware difference between the two boards, unlike an I2C probe
    // (GPIO4 is the LilyGo's battery-ADC pin, which fakes ACKs and reads back as
    // 0x00 just like an idle Guition touch).
    bool guition = (gpio4mv > 2800);
    gHwBoard = guition ? HW_GUITION : HW_LILYGO;
    Serial.printf("[board] auto-detect: %s (GPIO4 = %lu mV)\n",
                  guition ? "GUITION" : "LILYGO", (unsigned long)gpio4mv);
#elif defined(VICMON_HAS_GUITION)
    gHwBoard = HW_GUITION;  Serial.println("[board] compile-time: GUITION");
#elif defined(VICMON_HAS_LILYGO)
    gHwBoard = HW_LILYGO;   Serial.println("[board] compile-time: LILYGO");
#else
    gHwBoard = HW_HEADLESS;
#endif
}

// ---- device role (Master / Slave) ------------------------------------------
// The same firmware runs either role, chosen at boot from an NVS flag and
// toggled from the Diagnostics tab (reboots to re-lay-out the radio + tasks).
// Slave role skips BLE/AP/web and instead receives another master's broadcast
// via gRx and renders it on the same display.
uint8_t gRole = ROLE_MASTER;             // ROLE_MASTER / ROLE_SLAVE (enum in app.h)
volatile bool gRoleReq = false;          // display/loop request: toggle role + reboot
volatile bool gRebootReq = false;        // request: reboot (e.g. after an AP-name change)
slavelink::Receiver gRx;                 // ESP-NOW receiver, used only in slave role

static void loadRole() {
    Preferences p;
    p.begin("vicrole", true);
    gRole = p.getUChar("role", ROLE_MASTER);
    p.end();
    if (gRole > ROLE_SLAVE) gRole = ROLE_MASTER;
}
static void applyRoleToggle() {
    uint8_t nr = (gRole == ROLE_MASTER) ? ROLE_SLAVE : ROLE_MASTER;
    Preferences p;
    p.begin("vicrole", false);
    p.putUChar("role", nr);
    p.end();
    Serial.printf("[role] switching to %s\n", nr == ROLE_SLAVE ? "SLAVE" : "MASTER");
    cleanRestart("role switch");
}
// ---- boot record + heap supervisor ------------------------------------------
// Both of today's lockups had the same shape: every task still running, free
// heap frozen at one value, the largest block collapsed, ESP-NOW returning
// ESP_ERR_ESPNOW_NO_MEM for minutes, TCP refused while the AP kept beaconing —
// and nothing recovered it. That is not a state a task watchdog can see (nothing
// is stuck), so the loop watches for it directly and restarts CLEANLY: stats
// and history flushed first, the cause recorded, and the counters below kept in
// NVS so the event is visible on the next boot instead of being lost.
uint32_t gBootCount = 0;
uint32_t gHeapRestarts = 0;
const char* gResetReason = "unknown";
uint32_t gEspNowLastOkMs = 0;

static const char* resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_EXT:      return "external";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT-WDT";
        case ESP_RST_TASK_WDT: return "TASK-WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_DEEPSLEEP:return "deep-sleep";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

// Runs once early in setup(): read + bump the boot record, log the reset cause.
static void recordBoot() {
    static char rr[24];
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_UNKNOWN) {  // the SDK does not name every RTC code (a USB-CDC flash reset is 0x15)
        snprintf(rr, sizeof(rr), "unknown(rtc 0x%x)", (unsigned)rtc_get_reset_reason(0));
        gResetReason = rr;
    } else {
        gResetReason = resetReasonStr(r);
    }
    Preferences p;
    p.begin("vicboot", false);
    gBootCount = p.getUInt("boots", 0) + 1;
    gHeapRestarts = p.getUInt("heaprb", 0);
    const char* lastWhy = "";
    static char whyBuf[32];
    p.getString("why", whyBuf, sizeof(whyBuf)); lastWhy = whyBuf;
    p.putUInt("boots", gBootCount);
    p.putString("why", "");  // consumed
    p.end();
    Serial.printf("[boot] #%lu reset=%s heap-restarts=%lu%s%s\n", (unsigned long)gBootCount,
                  gResetReason, (unsigned long)gHeapRestarts,
                  lastWhy[0] ? " last-restart=" : "", lastWhy);
}

void cleanRestart(const char* why) {
    Serial.printf("[restart] %s — flushing and rebooting\n", why);
    gStats.maybePersist(millis(), true);
#ifndef VICMON_SIM
    if (gRole != ROLE_SLAVE) saveHistFile(gProfiles.active());
#endif
    Preferences p;
    p.begin("vicboot", false);
    p.putString("why", why);
    p.end();
    delay(300);
    ESP.restart();
}

// Called from both loops. Three signals, each must persist for kSupervisorMs
// before acting so a transient dip (a big page being served) never trips it:
//   - largest free block below the floor          (fragmentation wedge)
//   - free heap below the floor                    (outright exhaustion)
//   - master only: no successful ESP-NOW send      (the symptom both lockups
//     showed first; a healthy master succeeds every 250ms)
void serviceSupervisor(uint32_t now) {
    static const uint32_t kSupervisorMs = 60000;
    static const uint32_t kMinBlock = 12 * 1024, kMinFree = 16 * 1024;
    static uint32_t badSince = 0, lastCheck = 0;
    if (now - lastCheck < 5000) return;
    lastCheck = now;

    const char* why = nullptr;
    if (ESP.getMaxAllocHeap() < kMinBlock) why = "largest block below floor";
    else if (ESP.getFreeHeap() < kMinFree) why = "free heap below floor";
    else if (gRole != ROLE_SLAVE && gEspNowLastOkMs && now - gEspNowLastOkMs > kSupervisorMs)
        why = "ESP-NOW sends failing";

    if (!why) { badSince = 0; return; }
    if (!badSince) { badSince = now; Serial.printf("[supervisor] %s (watching)\n", why); return; }
    if (now - badSince < kSupervisorMs) return;

    Preferences p;
    p.begin("vicboot", false);
    p.putUInt("heaprb", gHeapRestarts + 1);
    p.end();
    cleanRestart(why);
}

// Consume a role-toggle request (raised by the Diag tab). Called from both loops.
void serviceRole() {
    if (gRoleReq) { gRoleReq = false; applyRoleToggle(); }
    if (gRebootReq) cleanRestart("config apply");
}

// Custom SoftAP name/password (device-wide, NVS ns "vicap"). Empty = use the
// auto-generated Vicmon-<mac3> / default password. Loaded at boot after the
// default SSID is computed, so a saved value overrides it.
void loadApCfg() {
    Preferences p;
    p.begin("vicap", true);
    String s = p.getString("ssid", ""), pw = p.getString("pass", "");
    p.end();
    if (s.length()) { strncpy(kApSsid, s.c_str(), sizeof(kApSsid) - 1); kApSsid[sizeof(kApSsid) - 1] = 0; }
    if (pw.length() >= 8) { strncpy(kApPass, pw.c_str(), sizeof(kApPass) - 1); kApPass[sizeof(kApPass) - 1] = 0; }
}
void saveApCfg(const String& ssid, const String& pass) {
    Preferences p;
    p.begin("vicap", false);
    p.putString("ssid", ssid);            // "" clears -> reverts to the default at next boot
    if (pass.length() >= 8) p.putString("pass", pass);
    else if (pass.length() == 0) p.putString("pass", "");
    p.end();
}

// Serial console commands (handy on a headless board / for testing): `pair`
// Summarise the graph's own history rings: how many samples of each series are
// valid, their range, and the amp-hours they integrate to. The Week page reads
// the Stats integrator while the Graph page reads these rings, so when the two
// disagree this says which one is empty.
static uint16_t gHistLoadedFine = 0, gHistLoadedCoarse = 0, gHistLoadedEnv = 0, gHistLoadedEnvF = 0;
static uint32_t gHistGapSec = 0;   // downtime bridged with n/a samples at the last load
static uint32_t gLastHistSaveMs = 0;  // 0 = not saved since boot
static bool gLastHistSaveOk = false;
static const uint32_t kHistSaveMs = 2 * 60 * 1000;
static void dumpHist() {
    static const char* kName[5] = {"batt", "solar", "chg", "dcdc", "load"};
    Serial.printf("[hist] restored at boot: fine=%u coarse=%u env=%u envfine=%u (gap bridged %lus) | last save %s%s | next in %lus\n",
                  (unsigned)gHistLoadedFine, (unsigned)gHistLoadedCoarse, (unsigned)gHistLoadedEnv, (unsigned)gHistLoadedEnvF, (unsigned long)gHistGapSec,
                  gLastHistSaveMs ? (String((millis() - gLastHistSaveMs) / 1000) + "s ago").c_str() : "none since boot",
                  gLastHistSaveMs ? (gLastHistSaveOk ? " (ok)" : " (FAILED)") : "",
                  (unsigned long)((kHistSaveMs - (millis() - gLastHistSaveMs) % kHistSaveMs) / 1000));
    Serial.printf("[hist] env-fine ring: count=%u cap=%u interval=%lums\n", (unsigned)gEnvFine.count, (unsigned)gEnvFine.cap, (unsigned long)gEnvFine.intervalMs);
    Serial.printf("[hist] env ring: count=%u cap=%u interval=%lums (%lu h span)\n", (unsigned)gEnv.count,
                  (unsigned)gEnv.cap, (unsigned long)gEnv.intervalMs, (unsigned long)(gEnv.count * gEnv.intervalMs / 3600000));
    for (int which = 0; which < 2; ++which) {
        const HistRing& r = which ? gCoarse : gFine;
        Serial.printf("[hist] %s ring: count=%u cap=%u interval=%lums (%lu min span)\n",
                      which ? "coarse" : "fine", (unsigned)r.count, (unsigned)r.cap,
                      (unsigned long)r.intervalMs,
                      (unsigned long)(r.count * r.intervalMs / 60000));
        if (!r.count) continue;
        size_t start = (r.head + r.cap - r.count) % r.cap;
        float hrs = r.intervalMs / 3600000.0f;
        for (int f = 0; f < 5; ++f) {
            int n = 0, nz = 0;
            float mn = 0, mx = 0, pos = 0, neg = 0;
            for (size_t i = 0; i < r.count; ++i) {
                const HistSample& hs = r.buf[(start + i) % r.cap];
                const int16_t v[5] = {hs.battery, hs.solar, hs.charger, hs.dcdc, hs.load};
                if (v[f] == -32768) continue;
                float a = v[f] / 10.0f;
                if (!n) { mn = mx = a; }
                else { if (a < mn) mn = a; if (a > mx) mx = a; }
                ++n;
                if (a != 0) ++nz;
                if (a > 0) pos += a * hrs; else neg += -a * hrs;
            }
            Serial.printf("[hist]   %-5s valid=%4d nonzero=%4d min=%7.1f max=%7.1f "
                          "+%.2fAh -%.2fAh\n", kName[f], n, nz, mn, mx, pos, neg);
        }
    }
}

// Dump the Week page's source data: the archived day records plus the live TODAY
// bucket. On a slave these arrive in the master's StatsFrame, so this prints
// exactly what the page is handed — the only way to tell an empty slot (no such
// day) from a zero-energy one (a day that archived with nothing in it).
static void dumpWeek() {
    if (gRole == ROLE_SLAVE) {
        if (!gRx.everStats()) { Serial.println("[week] no stats frame from master yet"); return; }
        const slavelink::StatsFrame& f = gRx.stats();
        Serial.printf("[week] slave: fresh=%u clockOk=%u dayNow=%lu dayCount=%u\n",
                      gRx.hasStats() ? 1u : 0u, (unsigned)f.clockOk,
                      (unsigned long)f.dayNow, (unsigned)f.dayCount);
        for (int i = 0; i < (int)f.dayCount && i < 7; ++i)
            Serial.printf("[week]   day[%d] stamp=%lu sol=%u dcdc=%u chg=%u load=%u | net in=%u out=%u\n", i,
                          (unsigned long)f.dayStamp[i], (unsigned)f.daySolarAh[i],
                          (unsigned)f.dayDcdcAh[i], (unsigned)f.dayChargerAh[i],
                          (unsigned)f.dayLoadAh[i], (unsigned)f.dayChargedAh[i], (unsigned)f.dayDischargedAh[i]);
        Serial.printf("[week]   today in=%lu out=%lu sol=%lu dcdc=%lu chg=%lu load=%lu dur=%lus\n",
                      (unsigned long)f.today.inAh, (unsigned long)f.today.outAh,
                      (unsigned long)f.today.solarAh, (unsigned long)f.today.dcdcAh,
                      (unsigned long)f.today.chargerAh, (unsigned long)f.today.loadAh,
                      (unsigned long)f.today.durSecs);
        // The live snapshot's validity bits say whether the master can SEE each
        // source at all — an empty Week bar with a charging battery means the
        // per-source signals are not resolving, not that the chart is broken.
        const slavelink::Snapshot& sn = gRx.snapshot();
        Serial.printf("[week]   live valid=0x%04x batt=%c%.1fA solar=%c%.1fA chg=%c%.1fA "
                      "dcdc=%c%.1fA load=%c%.1fA\n",
                      (unsigned)sn.valid,
                      (sn.valid & slavelink::V_BATTA) ? '+' : '-', sn.battA_da / 10.0f,
                      (sn.valid & slavelink::V_SOLAR) ? '+' : '-', sn.solarA_da / 10.0f,
                      (sn.valid & slavelink::V_CHARGER) ? '+' : '-', sn.chargerA_da / 10.0f,
                      (sn.valid & slavelink::V_DCDC) ? '+' : '-', sn.dcdcA_da / 10.0f,
                      (sn.valid & slavelink::V_LOAD) ? '+' : '-', sn.loadA_da / 10.0f);
        Serial.printf("[week]   master uptime=%lus seq=%u soc=%.1f%% battV=%.2f consumed=%.1fAh "
                      "mode=%u\n",
                      (unsigned long)sn.uptime_s, (unsigned)sn.seq, sn.soc_d / 10.0f,
                      sn.battV_cv / 100.0f, sn.consumedAh_da / 10.0f, (unsigned)sn.mode);
        return;
    }
    const stats::Bucket& b = gStats.bucket(stats::TODAY);
    Serial.printf("[week] master: localEpoch=%lu runSecs=%lu runDay=%lu dayCount=%u todayStamp=%lu\n",
                  (unsigned long)currentLocalEpoch(), (unsigned long)gStats.runSecs(),
                  (unsigned long)gStats.runDay(), (unsigned)gStats.dayCount(),
                  (unsigned long)b.dayStamp);
    for (size_t i = 0; i < gStats.dayCount(); ++i) {
        const stats::DayRecord& r = gStats.day(i);
        Serial.printf("[week]   day[%u] stamp=%lu sol=%.1f dcdc=%.1f chg=%.1f load=%.1f\n",
                      (unsigned)i, (unsigned long)r.dayStamp, r.solarAh, r.dcdcAh,
                      r.chargerAh, r.loadAh);
    }
    Serial.printf("[week]   today sol=%.1f dcdc=%.1f chg=%.1f load=%.1f dur=%lus\n",
                  (float)b.solarAh, (float)b.dcdcAh, (float)b.chargerAh, (float)b.loadAh,
                  (unsigned long)b.durationSecs);
}

// The slave loop never reaches serviceMasterSerial(), so it gets its own tiny
// console with just the read-only dumps.
static void serviceSlaveSerial() {
    static char line[16];
    static uint8_t n = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line[n] = '\0';
            if (n) {
                if (!strcmp(line, "week")) dumpWeek();
                else if (!strcmp(line, "hist")) dumpHist();
                else if (!strcmp(line, "restart")) cleanRestart("console");
                else if (!strcmp(line, "mem"))
                    Serial.printf("[mem] free=%u largest=%u minfree-ever=%u\n",
                                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                                  (unsigned)ESP.getMinFreeHeap());
            }
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
}

// opens the master pairing window; `role` toggles Master<->Slave (reboots).
static void serviceMasterSerial() {
    static char line[16];
    static uint8_t n = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line[n] = '\0';
            if (n) {
                if (!strcmp(line, "pair")) {
                    startPairing();
                    Serial.printf("[master] pairing window open %ds\n", pairSecsLeft());
                } else if (!strcmp(line, "role")) {
                    gRoleReq = true;
                } else if (!strcmp(line, "mem")) {
                    Serial.printf("[mem] heap pool total=%u free=%u largest=%u minfree-ever=%u\n",
                                  (unsigned)ESP.getHeapSize(), (unsigned)ESP.getFreeHeap(),
                                  (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
                    Serial.printf("[mem] internal-RAM total=%u free=%u | flash=%u\n",
                                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                  (unsigned)ESP.getFlashChipSize());
                } else if (!strcmp(line, "restart")) {
                    cleanRestart("console");  // flushes stats + history first; recorded in the boot log
                } else if (!strcmp(line, "hist")) {
                    dumpHist();  // graph history rings: validity + integrated Ah
                } else if (!strcmp(line, "week")) {
                    dumpWeek();  // Week-page source data: archived days + TODAY
                } else if (!strcmp(line, "webtest")) {
                    webSelfTest();  // build the big pages + report heap (no HTTP client needed)
                } else if (!strcmp(line, "tasks")) {
                    // Min-ever free stack (BYTES) for the tasks worth trimming, so a
                    // stack can be sized to just above its real peak. Load a few web
                    // pages first, then run this, to catch the AsyncTCP peak.
                    const char* names[] = {"async_tcp", "loopTask", "wifi", "tiT"};
                    for (auto nm : names) {
                        TaskHandle_t h = xTaskGetHandle(nm);
                        if (h)
                            Serial.printf("[task] %-10s minFreeStack=%uB\n", nm,
                                          (unsigned)(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t)));
                        else
                            Serial.printf("[task] %-10s (not found)\n", nm);
                    }
#ifdef VICMON_HAS_M5CAPSULE
                } else if (!strcmp(line, "cap")) {
                    // Headless M5Capsule status probe (no screen to show it on).
                    Serial.printf("[cap] board=%s rtc=%s(utc %lu) sd=%s buzzer=%s clock=%lu\n",
                                  capsulePresent() ? "M5CAPSULE" : "other",
                                  capsuleRtcOk() ? "ok" : "unset", (unsigned long)capsuleRtcUtc(),
                                  capsuleSdOk() ? "mounted" : "none",
                                  gBuzzerEnable ? "on" : "muted",
                                  (unsigned long)currentUtcEpoch());
                } else if (!strcmp(line, "beep")) {
                    capsuleServiceBuzzer(true, millis());  // fire one test chirp
                    Serial.println("[cap] test beep");
                } else if (!strcmp(line, "sd")) {
                    capsuleDumpSd();  // list the microSD log files
                } else if (!strcmp(line, "led")) {
                    capsuleLedTest();  // is the WS2812 alive on GPIO21 at all?
#endif
#ifdef VICMON_HAS_ENVPRO
                } else if (!strcmp(line, "env")) {
                    envDump();  // scan Grove Port A + print the live ENV Pro reading
#endif
                }
            }
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
}

// Format bytes as lowercase hex into a fixed buffer (for the Diag raw-capture
// line; hexBytes() returns a String and is defined much later in the file).
void hexInto(char* out, size_t n, const uint8_t* p, size_t len) {
    static const char* hx = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < n; ++i) {
        out[o++] = hx[p[i] >> 4];
        out[o++] = hx[p[i] & 0xF];
    }
    out[o < n ? o : n - 1] = '\0';
}

// One-line live summary of a monitored device for the Diagnostics list.
void summarizeDevice(const DeviceSlot& s, char* out, size_t n) {
    switch (s.type) {
        case victron::Record::BatteryMonitor:
            snprintf(out, n, "%.2fV %.0f%%", s.battery.voltage, s.battery.soc); break;
        case victron::Record::OrionXs:
            snprintf(out, n, "%.1fA %.1fV", s.dcdc.outputCurrent, s.dcdc.outputVoltage); break;
        case victron::Record::SolarCharger:
            snprintf(out, n, "%.0fW %.1fA", s.solar.pvPower, s.solar.batteryCurrent); break;
        case victron::Record::AcCharger:
            snprintf(out, n, "%.1fA %.1fV", s.charger.batteryCurrent, s.charger.batteryVoltage); break;
        default: snprintf(out, n, "--"); break;
    }
}

// ---- type <-> string helpers ----------------------------------------------

victron::Record parseType(const String& t) {
    if (t == "dcdc") return victron::Record::OrionXs;
    if (t == "solar") return victron::Record::SolarCharger;
    if (t == "charger") return victron::Record::AcCharger;
    return victron::Record::BatteryMonitor;
}
const char* typeName(victron::Record r) {
    switch (r) {
        case victron::Record::OrionXs: return "dcdc";
        case victron::Record::BatteryMonitor: return "battery";
        case victron::Record::SolarCharger: return "solar";
        case victron::Record::AcCharger: return "charger";
        default: return "?";
    }
}

// ---- signal resolution -----------------------------------------------------

sig::Resolved R(sig::Role role, uint32_t now) {
    const sig::Binding& b = gSignals.binding(role);
    return sig::resolveField(gConfig.slots(), gConfig.count(), b.device, b.field, now);
}


// Sum of the measured charge sources (solar + DC-DC output + charger).
static float measuredSources(uint32_t now) {
    sig::Resolved sa = R(sig::Role::SolarA, now);
    sig::Resolved doa = R(sig::Role::DcDcOutA, now);
    sig::Resolved cg = R(sig::Role::ChargerA, now);
    return (sa.valid ? sa.value : 0) + (doa.valid ? doa.value : 0) + (cg.valid ? cg.value : 0);
}

// "Assume zero until stable" smoother for the derived energy-balance signals.
// They can spike for a poll or two when contributing devices advertise out of
// step (e.g. the DC-DC reads 0 a beat before the BMV current refreshes). Report
// the MINIMUM over the last few samples: a value only shows once every recent
// sample agrees it's genuinely present, and a transient spike is pulled to zero.
// Measured device readings stay raw.
struct Smoothed {
    static const int N = 4;  // recent samples the derived value must agree over
    float ring[N] = {0};
    int n = 0, pos = 0;
    float value = 0;
    bool valid = false;
    void push(bool v, float x) {
        valid = v;
        if (!v) { value = 0; return; }
        ring[pos] = x;
        pos = (pos + 1) % N;
        if (n < N) ++n;
        float mn = ring[0];
        for (int i = 1; i < n; ++i) if (ring[i] < mn) mn = ring[i];
        value = mn;
    }
};
static Smoothed gSmCharge, gSmLoad;

// Raw (unsmoothed) energy-balance halves (let bat = net battery current
// (+charge/-discharge), src = measured sources):
//   charge = max(0, bat - src)   charge not explained by sources
//   load   = max(0, src - bat)   consumption (battery flow offset by sources)
static void rawDerived(uint32_t now, sig::Resolved& charge, sig::Resolved& load) {
    sig::Resolved ba = R(sig::Role::BatteryA, now);
    charge.valid = load.valid = ba.valid;
    if (ba.valid) {
        float src = measuredSources(now);
        float cv = ba.value - src;
        float lv = src - ba.value;
        charge.value = cv > 0 ? cv : 0;
        load.value = lv > 0 ? lv : 0;
    }
}

// Push one fresh derived sample into the smoothers. Call once per loop, after
// the registry has been refreshed and before anything consumes the signals.
static void updateDerivedSmoothing() {
    sig::Resolved c, l;
    rawDerived(millis(), c, l);
    gSmCharge.push(c.valid, c.value);
    gSmLoad.push(l.valid, l.value);
}

// Resolve a role honouring derived sentinels; derived roles return the
// median-smoothed value. Otherwise resolves the bound device field directly.
sig::Resolved resolveSignal(sig::Role role, uint32_t now) {
    const sig::Binding& b = gSignals.binding(role);
    bool chargeOnly = strcmp(b.device, sig::kChargeOnly) == 0;
    bool load = strcmp(b.device, sig::kLoadOnly) == 0 || strcmp(b.device, sig::kDerived) == 0;
    if (chargeOnly) return {gSmCharge.value, gSmCharge.valid};
    if (load) return {gSmLoad.value, gSmLoad.valid};
    return R(role, now);
}

static bool roleIsDerived(sig::Role role) {
    const char* d = gSignals.binding(role).device;
    return strcmp(d, sig::kDerived) == 0 || strcmp(d, sig::kChargeOnly) == 0 ||
           strcmp(d, sig::kLoadOnly) == 0;
}

// The five trended currents (shared by the panel API and the history sampler).
struct Currents {
    float battery = 0, solar = 0, charger = 0, dcdc = 0, load = 0;
    bool bV = false, sV = false, cV = false, dV = false, lV = false;
};
static Currents computeCurrents(uint32_t now) {
    Currents c;
    sig::Resolved ba = resolveSignal(sig::Role::BatteryA, now);
    sig::Resolved sa = resolveSignal(sig::Role::SolarA, now);
    sig::Resolved cg = resolveSignal(sig::Role::ChargerA, now);
    sig::Resolved doa = resolveSignal(sig::Role::DcDcOutA, now);
    sig::Resolved la = resolveSignal(sig::Role::LoadA, now);
    c.battery = ba.value; c.bV = ba.valid;
    c.solar = sa.value; c.sV = sa.valid;
    c.charger = cg.value; c.cV = cg.valid;
    c.dcdc = doa.value; c.dV = doa.valid;
    c.load = la.value; c.lV = la.valid;
    return c;
}

// ---- continuous history (server-side ring buffer) --------------------------

static const size_t HIST_CAP = 720;          // 60 min @ 5 s (fine, short windows)
static const uint32_t HIST_INTERVAL = 5000;  // ms
static const size_t HIST2_CAP = 1440;          // 24 h @ 60 s (coarse, long windows)
static const uint32_t HIST2_INTERVAL = 60000;  // ms
// HistSample + HistRing moved to app.h (collectHistory in display.cpp reads the
// rings; buildHistoryJson in web/main serializes them).
static HistSample gFineBuf[HIST_CAP];
static HistSample gCoarseBuf[HIST2_CAP];
static const size_t ENV_CAP = 288;             // 24 h @ 5 min
static const uint32_t ENV_INTERVAL = 300000;   // ms
static EnvSample gEnvBuf[ENV_CAP];
EnvRing gEnv;
// A 5-minute cadence is right for 24h but gave the 1m/10m/1h windows one or two
// points and a blank chart for five minutes after every boot. Windows up to an
// hour read this 60s ring instead (480 B; live only, it refills within the hour).
static const size_t ENVF_CAP = 60;
static const uint32_t ENVF_INTERVAL = 60000;
static EnvSample gEnvFineBuf[ENVF_CAP];
EnvRing gEnvFine;
uint8_t gLinkMismatch = 0;
HistRing gFine, gCoarse;

static int16_t encA(bool v, float a) {
    return v ? static_cast<int16_t>(lroundf(a * 10.0f)) : -32768;
}
// Number of series buildHistoryJson emits, and which one is the gas channel (the
// only one not held as *10 fixed-point).
static const int kHistSeries = 10;
static const int kHistGasIdx = 9;

static int16_t sampleField(const HistSample& s, int idx) {
    switch (idx) {
        case 0: return s.battery;
        case 1: return s.solar;
        case 2: return s.charger;
        case 3: return s.dcdc;
        case 4: return s.load;
        default: return s.soc;
    }
}
static int16_t envField(const EnvSample& e, int idx) {   // 6..9 of the series order
    switch (idx) { case 6: return e.t; case 7: return e.h; case 8: return e.p; default: return e.g; }
}

// Encode the live Unit ENV Pro reading into a history sample. Temperature,
// humidity and pressure share the *10 fixed-point convention of the electrical
// channels; gas is recorded in whole kilo-ohms because its useful range spans
// three orders of magnitude and a tenth of an ohm is noise. Gas carries its own
// validity — the heater needs a few cycles from cold before its number means
// anything, while T/H/P are good from the first reading.
static void encodeEnv(EnvSample& e) {
    const EnvReading& r = envReading();
    e.t = r.valid ? static_cast<int16_t>(lroundf(r.tempC * 10.0f)) : -32768;
    e.h = r.valid ? static_cast<int16_t>(lroundf(r.humidity * 10.0f)) : -32768;
    e.p = r.valid ? static_cast<int16_t>(lroundf(r.pressureHpa * 10.0f)) : -32768;
    if (r.gasValid) {
        float k = r.gasOhm / 1000.0f;       // ohms -> kilo-ohms
        if (k > 32000.0f) k = 32000.0f;     // very clean air can run away; clamp under INT16_MAX
        if (k < 0.0f) k = 0.0f;
        e.g = static_cast<int16_t>(lroundf(k));
    } else {
        e.g = -32768;
    }
}

// Runs continuously from loop() regardless of any connected client. Feeds both
// rings from a single read.
static void sampleHistory() {
    uint32_t now = millis();
    // First env sample waits for a valid reading: at boot the BME688 has not
    // produced one yet, and an n/a sample would blank the first five minutes.
    // Only ever store a VALID reading. The BME688 reading flips invalid between
    // measurement cycles; at 5s cadence a stray n/a sample was harmless, at 60s
    // it is a hole, and as a window's seed it blanks the whole window. A ring
    // that is due simply stays due until the next valid reading.
    if (envReading().valid) {
        if (gEnv.due(now))     { EnvSample e; encodeEnv(e); gEnv.push(e, now); }
        if (gEnvFine.due(now)) { EnvSample e; encodeEnv(e); gEnvFine.push(e, now); }
    }
    bool dueFine = gFine.due(now);
    bool dueCoarse = gCoarse.due(now);
    if (!dueFine && !dueCoarse) return;
    Currents c = computeCurrents(now);
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    HistSample s;
    s.battery = encA(c.bV, c.battery);
    s.solar = encA(c.sV, c.solar);
    s.charger = encA(c.cV, c.charger);
    s.dcdc = encA(c.dV, c.dcdc);
    s.load = encA(c.lV, c.load);
    s.soc = encA(soc.valid, soc.value);  // deci-percent (same *10 encoding)
    if (dueFine) gFine.push(s, now);
    if (dueCoarse) gCoarse.push(s, now);
}

// Slave role: record the received ESP-NOW frames into the same history rings, so
// the web Trend chart (and a display-slave's Graph) show the live data we've been
// getting. The Snapshot fields are already deci-encoded with -32768 = n/a, so they
// map straight onto HistSample.
static void sampleSlaveHistory() {
    uint32_t now = millis();
    bool dueFine = gFine.due(now), dueCoarse = gCoarse.due(now);
    if (!dueFine && !dueCoarse && !gEnv.due(now) && !gEnvFine.due(now)) return;
    if (!gRx.live()) return;  // only log while actually receiving
    using namespace slavelink;
    const Snapshot& sn = gRx.snapshot();
    auto f = [&](uint16_t bit, int16_t v) -> int16_t { return (sn.valid & bit) ? v : (int16_t)-32768; };
    HistSample s;
    s.battery = f(V_BATTA, sn.battA_da);
    s.solar   = f(V_SOLAR, sn.solarA_da);
    s.charger = f(V_CHARGER, sn.chargerA_da);
    s.dcdc    = f(V_DCDC, sn.dcdcA_da);
    s.load    = f(V_LOAD, sn.loadA_da);
    s.soc     = f(V_SOC, sn.soc_d);
    if (dueFine) gFine.push(s, now);
    if (dueCoarse) gCoarse.push(s, now);
    // Environment mirrors the master's sensor on its own 5-min ring: T/H/P share
    // one validity bit, gas has its own (see slavelink::V_ENV / V_ENVGAS).
    if (sn.valid & V_ENV) {   // never store an n/a env sample (see sampleHistory)
        EnvSample e;
        e.t = f(V_ENV, sn.envTemp_dc); e.h = f(V_ENV, sn.envHum_dp);
        e.p = f(V_ENV, (int16_t)sn.envPress_dhpa); e.g = f(V_ENVGAS, (int16_t)sn.envGas_kohm);
        if (gEnv.due(now)) gEnv.push(e, now);
        if (gEnvFine.due(now)) gEnvFine.push(e, now);
    }
}

// ---- history persistence (LittleFS) ----------------------------------------
// Both ring buffers are written to a per-profile file every few minutes (and on
// a profile switch) so the 1 h / 24 h charts survive a reboot. There are no
// timestamps in the data, so the gap during downtime simply isn't represented —
// reloaded samples continue seamlessly at the "now" edge.

bool gFsOk = false;
static const uint8_t kHistVer = 5;  // 2: +soc; 3: +env in HistSample; 4: env on its own 5-min ring; 5: +60s env ring, +save time (downtime gap on load)
// How often the history is flushed to flash. At ~26 KB/save (full buffers) this
// is ~7.5 MB/day; LittleFS wear-levels it across the ~1.5 MB FS partition, so at
// 100k erase cycles/block the flash lasts decades. Raise it to lose less to
// wear (at the cost of losing a little more recent history on an unclean reboot).
// Anything newer than the last save is lost on a brownout or a USB reset (only
// cleanRestart() flushes first). 2 minutes bounds that loss; the file is ~28KB,
// so ~20MB/day on a wear-levelled LittleFS partition — decades, not years.

static String histPath(int profile) { return "/hist" + String(profile) + ".bin"; }

// Write a ring's samples oldest-first.
static void writeRing(File& f, const HistRing& r) {
    size_t start = (r.head + r.cap - r.count) % r.cap;
    for (size_t k = 0; k < r.count; ++k) {
        size_t idx = (start + k) % r.cap;
        f.write(reinterpret_cast<const uint8_t*>(&r.buf[idx]), sizeof(HistSample));
    }
}
static void writeEnvRing(File& f, const EnvRing& r) {
    size_t start = (r.head + r.cap - r.count) % r.cap;
    for (size_t k = 0; k < r.count; ++k)
        f.write(reinterpret_cast<const uint8_t*>(&r.buf[(start + k) % r.cap]), sizeof(EnvSample));
}
static void readEnvRing(File& f, EnvRing& r, uint16_t n) {
    if (n > r.cap) n = r.cap;
    size_t k = 0;
    for (; k < n; ++k)
        if (f.read(reinterpret_cast<uint8_t*>(&r.buf[k]), sizeof(EnvSample)) != sizeof(EnvSample)) break;
    r.count = k; r.head = k % r.cap;
}
// Read up to n samples back into a (chronological) ring.
static void readRing(File& f, HistRing& r, uint16_t n) {
    if (n > r.cap) n = r.cap;
    size_t k = 0;
    for (; k < n; ++k)
        if (f.read(reinterpret_cast<uint8_t*>(&r.buf[k]), sizeof(HistSample)) != sizeof(HistSample))
            break;
    r.count = k;
    r.head = k % r.cap;
}

// Sample counts the file held when it was loaded. A ring starts EMPTY on every
// boot, so an unconditional periodic save would overwrite a day of stored
// history with a few seconds of it — which is exactly what repeated reflashing
// did to the master's trend today. Refuse to shrink the file.


void saveHistFile(int profile) {
    if (!gFsOk) return;
    if (gFine.count < gHistLoadedFine || gCoarse.count < gHistLoadedCoarse || gEnv.count < gHistLoadedEnv || gEnvFine.count < gHistLoadedEnvF) return;
    // Write to a temp file and rename over the old one. LittleFS rename is
    // atomic, so a reset or reflash landing mid-write (every 5 min, so not rare
    // across a day of flashing) leaves the previous complete file in place
    // instead of a torn one that loads as a near-empty ring.
    String path = histPath(profile), tmp = path + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) return;
    uint8_t hdr[4] = {'V', 'H', kHistVer, 0};
    uint16_t fc = gFine.count, cc = gCoarse.count, ec = gEnv.count, efc = gEnvFine.count;
    uint32_t savedUtc = currentUtcEpoch();   // 0 = no clock; the load then cannot size the gap
    bool ok = f.write(hdr, 4) == 4 &&
              f.write(reinterpret_cast<uint8_t*>(&fc), 2) == 2 &&
              f.write(reinterpret_cast<uint8_t*>(&cc), 2) == 2 &&
              f.write(reinterpret_cast<uint8_t*>(&ec), 2) == 2 &&
              f.write(reinterpret_cast<uint8_t*>(&efc), 2) == 2 &&
              f.write(reinterpret_cast<uint8_t*>(&savedUtc), 4) == 4;
    if (ok) { writeRing(f, gFine); writeRing(f, gCoarse); writeEnvRing(f, gEnv); writeEnvRing(f, gEnvFine); }
    f.close();
    // Verify the length on a fresh handle. Checking size() on the still-open
    // handle read the pre-flush length, judged every save short, and silently
    // discarded it — so the file on flash stayed the previous version and every
    // boot "started fresh". A failed save is now loud, never silent.
    const size_t expect = 16 + (size_t)(fc + cc) * sizeof(HistSample) + (size_t)(ec + efc) * sizeof(EnvSample);
    size_t got = 0;
    { File v = LittleFS.open(tmp, "r"); if (v) { got = v.size(); v.close(); } }
    if (!ok || got != expect) {
        Serial.printf("[hist] SAVE FAILED: wrote %u of %u bytes\n", (unsigned)got, (unsigned)expect);
        LittleFS.remove(tmp); gLastHistSaveOk = false; return;
    }
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path)) { Serial.println("[hist] SAVE FAILED: rename"); gLastHistSaveOk = false; return; }
    gLastHistSaveOk = true;
}

static void loadHistFile(int profile) {
    if (!gFsOk) return;
    File f = LittleFS.open(histPath(profile), "r");
    if (!f) return;
    uint8_t hdr[4];
    if (f.read(hdr, 4) != 4 || hdr[0] != 'V' || hdr[1] != 'H' || hdr[2] != kHistVer) {
        if (f.size() >= 3 && hdr[0] == 'V' && hdr[1] == 'H') Serial.printf("[hist] file is v%u, this firmware v%u — starting fresh\n", hdr[2], kHistVer);
        f.close();
        return;
    }
    uint16_t fc = 0, cc = 0, ec = 0, efc = 0; uint32_t savedUtc = 0;
    f.read(reinterpret_cast<uint8_t*>(&fc), 2);
    f.read(reinterpret_cast<uint8_t*>(&cc), 2);
    f.read(reinterpret_cast<uint8_t*>(&ec), 2);
    f.read(reinterpret_cast<uint8_t*>(&efc), 2);
    f.read(reinterpret_cast<uint8_t*>(&savedUtc), 4);
    size_t expect = 16 + (size_t)(fc + cc) * sizeof(HistSample) + (size_t)(ec + efc) * sizeof(EnvSample);
    if (fc > gFine.cap || cc > gCoarse.cap || ec > gEnv.cap || efc > gEnvFine.cap || f.size() != expect) {
        Serial.printf("[hist] %s: bad length (%u, expected %u) — ignoring\n",
                      histPath(profile).c_str(), (unsigned)f.size(), (unsigned)expect);
        f.close();
        return;
    }
    readRing(f, gFine, fc);
    readRing(f, gCoarse, cc);
    readEnvRing(f, gEnv, ec);
    readEnvRing(f, gEnvFine, efc);
    f.close();
    // The rings carry no timestamps: a sample is assumed one interval before the
    // next, ending now. Restored as-is, the saved history would butt straight up
    // against the new samples and the downtime would vanish — a night off would
    // show yesterday evening glued to this morning, and every reboot compressed
    // the timeline a little more. Bridge the gap with n/a samples instead.
    uint32_t nowUtc = currentUtcEpoch();
    gHistGapSec = (savedUtc && nowUtc > savedUtc) ? nowUtc - savedUtc : 0;
    auto bridge = [&](HistRing& r) { HistSample na; na.battery = na.solar = na.charger = na.dcdc = na.load = na.soc = -32768;
        uint32_t k = gHistGapSec * 1000 / r.intervalMs; if (k > r.cap) k = r.cap; for (uint32_t i = 0; i < k; ++i) r.push(na, 0); };
    auto bridgeE = [&](EnvRing& r) { EnvSample na; na.t = na.h = na.p = na.g = -32768;
        uint32_t k = gHistGapSec * 1000 / r.intervalMs; if (k > r.cap) k = r.cap; for (uint32_t i = 0; i < k; ++i) r.push(na, 0); };
    if (gHistGapSec) { bridge(gFine); bridge(gCoarse); bridgeE(gEnv); bridgeE(gEnvFine); }
    gHistLoadedFine = gFine.count;    // what the file now represents (incl. the bridged gap)
    gHistLoadedCoarse = gCoarse.count;
    gHistLoadedEnv = gEnv.count;
    gHistLoadedEnvF = gEnvFine.count;
    Serial.printf("[hist] restored fine=%u coarse=%u env=%u envfine=%u; %s\n", (unsigned)fc, (unsigned)cc, (unsigned)ec, (unsigned)efc,
                  gHistGapSec ? (String("bridged ") + String(gHistGapSec) + "s of downtime").c_str()
                              : (savedUtc ? "no downtime gap" : "no clock at save or load — gap unknown, history butted"));
    gFine.lastMs = 0;  // take a fresh sample promptly after a reload
    gCoarse.lastMs = 0;
}

// ---- energy counters / trip stats ------------------------------------------


// Integrate the live readings into the energy counters. Runs every loop; the
// integrator handles the variable dt itself.
static void sampleStats() {
    uint32_t now = millis();
    Currents c = computeCurrents(now);
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved bv = R(sig::Role::BatteryV, now);
    sig::Resolved sw = R(sig::Role::SolarW, now);

    stats::Sample s;
    s.socValid = soc.valid; s.soc = soc.value;
    s.battVValid = bv.valid; s.battV = bv.value;
    s.battAValid = c.bV; s.battA = c.battery;
    s.solarValid = c.sV; s.solarA = c.solar;
    s.solarWValid = sw.valid; s.solarW = sw.value;
    s.dcdcValid = c.dV; s.dcdcA = c.dcdc;
    s.chargerValid = c.cV; s.chargerA = c.charger;
    s.loadValid = c.lV; s.loadA = c.load;

    gStats.update(s, now, currentLocalEpoch());
    gStats.maybePersist(now);
}

// Serialise the selected history window as JSON in ~1.4KB chunks.
//
// This used to build ONE String. A 60-minute window is 720 samples x 10 series
// (~35KB) and 24h is ~70KB, so the allocation simply failed and the endpoint
// answered 200 with an EMPTY BODY — the web chart was blank at every zoom above
// 10 minutes — while the repeated reallocation churned the heap on the way
// there. Chunking caps the largest single allocation at 1.4KB.
//
// The window is also downsampled to at most kWebHistPts columns: a 760px canvas
// cannot show 1440 of them. Buckets are peak-preserving (largest magnitude, sign
// kept) so current spikes survive, matching what the LCD does. `interval` is
// reported as the EFFECTIVE column spacing so the chart's right-aligned x-axis
// still lands each column in the right place.
static const int kWebHistPts = 100;  // 760px canvas at ~7px per column; the LCD uses 116

// Writes the JSON straight into the web arena with h.f(): no String, no vector,
// no per-request allocation of any size.
void buildHistoryInto(int mins, OutSink& h) {
    if (mins < 1) mins = 1;
    if (mins > 1440) mins = 1440;
    const HistRing& r = mins > 60 ? gCoarse : gFine;
    int want = mins * 60 * 1000 / static_cast<int>(r.intervalMs);
    if (want > static_cast<int>(r.count)) want = r.count;
    if (want < 0) want = 0;
    const int n = want < kWebHistPts ? want : kWebHistPts;
    int ivSec = static_cast<int>(r.intervalMs / 1000);
    if (n > 0) ivSec = static_cast<int>(static_cast<long>(want) * ivSec / n);
    if (ivSec < 1) ivSec = 1;
    const size_t start = (r.head + r.cap - static_cast<size_t>(want)) % r.cap;
    static const char* const names[kHistSeries] = {"battery", "solar", "charger", "dcdc", "load",
                                                   "soc", "temp", "humidity", "pressure", "gas"};
    h.f("{\"interval\":%d,\"mins\":%d,\"series\":{", ivSec, mins);
    // Environment lives on its own 5-min ring. Its columns are aligned to the
    // same depth in time as the electrical ones, averaged per column, and a
    // column with no env sample carries the previous value forward (a reading
    // that changes by a tenth of a degree an hour is a level, not a gap).
    const EnvRing& er = mins > 60 ? gEnv : gEnvFine;
    const int ivE = static_cast<int>(er.intervalMs / 1000);
    const int depthSec = want * static_cast<int>(r.intervalMs / 1000);
    int wantE = (depthSec + ivE - 1) / ivE;
    if (wantE > static_cast<int>(er.count)) wantE = er.count;
    const size_t startE = (er.head + er.cap - static_cast<size_t>(wantE)) % er.cap;
    // The sample just before the window seeds the level, so a window shorter than
    // the cadence draws the current reading across it instead of nothing.
    bool haveSeed = static_cast<int>(er.count) > wantE;
    const EnvRing* seedRing = &er;
    size_t seedIdx = (startE + er.cap - 1) % er.cap;
    if (!haveSeed && &er == &gEnvFine && gEnv.count) {   // 60s ring has nothing earlier: use the newest 5-min sample
        haveSeed = true; seedRing = &gEnv; seedIdx = (gEnv.head + gEnv.cap - 1) % gEnv.cap;
    }
    for (int f = 0; f < kHistSeries; ++f) {
        const bool whole = (f == kHistGasIdx);
        const bool env = (f >= 6);
        h.f("\"%s\":[", names[f]);
        int16_t hold = (env && haveSeed) ? envField(seedRing->buf[seedIdx], f) : (int16_t)-32768;
        for (int k = 0; k < n; ++k) {
            int16_t best = -32768;
            if (env) {
                int lo = static_cast<int>(static_cast<long>(k) * wantE / n);
                int hi = static_cast<int>(static_cast<long>(k + 1) * wantE / n);
                if (hi > wantE) hi = wantE;
                long sum = 0; int cnt = 0;
                for (int si = lo; si < hi; ++si) {
                    const int16_t v = envField(er.buf[(startE + static_cast<size_t>(si)) % er.cap], f);
                    if (v != -32768) { sum += v; ++cnt; }
                }
                if (cnt) hold = static_cast<int16_t>(sum / cnt);
                best = hold;
            } else {
                int lo = static_cast<int>(static_cast<long>(k) * want / n);
                int hi = static_cast<int>(static_cast<long>(k + 1) * want / n);
                if (hi <= lo) hi = lo + 1;
                if (hi > want) hi = want;
                for (int si = lo; si < hi; ++si) {
                    const int16_t v = sampleField(r.buf[(start + static_cast<size_t>(si)) % r.cap], f);
                    if (v == -32768) continue;
                    if (best == -32768) { best = v; continue; }
                    const int av = v < 0 ? -v : v, ab = best < 0 ? -best : best;
                    if (av > ab) best = v;
                }
            }
            if (best == -32768) h.f("%snull", k ? "," : "");
            else if (whole)     h.f("%s%d", k ? "," : "", (int)best);
            else                h.f("%s%s%d.%d", k ? "," : "", best < 0 ? "-" : "", (int)(best < 0 ? -best : best) / 10, (int)(best < 0 ? -best : best) % 10);
        }
        h.f(f < kHistSeries - 1 ? "]," : "]");
    }
    h.f("}}");
}

// ---- WiFi STA (join an existing network) -----------------------------------

float gBattCapacity = 0;     // Ah, 0 = unknown
float gDeadband = 0.2f;      // A; |current| below this reads as idle
int gTzOffsetMin = 600;      // local time offset from UTC, minutes (+10h AEST)
// Alert thresholds (0 disables that check).
float gSocWarn = 50;         // % — warn at/below
float gSocCrit = 30;         // % — critical at/below
float gVlow = 11.8f;         // V — critical at/below
float gVhigh = 15.0f;        // V — critical at/above
bool gBuzzerEnable = true;   // M5Capsule buzzer follows the SoC-critical alert (0 = mute)
String settingsNs(int profile) {
    return profile == 0 ? String("vicset") : "vicset" + String(profile);
}
static void loadSettings(int profile) {
    Preferences p;
    p.begin(settingsNs(profile).c_str(), true);
    gBattCapacity = p.getFloat("battcap", 0);
    gDeadband = p.getFloat("deadband", 0.2f);
    gTzOffsetMin = p.getInt("tzmin", 600);
    gSocWarn = p.getFloat("socwarn", 50);
    gSocCrit = p.getFloat("soccrit", 30);
    gVlow = p.getFloat("vlow", 11.8f);
    gVhigh = p.getFloat("vhigh", 15.0f);
    gBuzzerEnable = p.getUChar("buzzen", 1) != 0;
    p.end();
}
// M5Capsule buzzer mute flag (own tiny writer so the 4-arg saveAlertSettings
// signature and its callers stay unchanged).
void saveBuzzerEnable(bool en) {
    Preferences p;
    p.begin(settingsNs(gProfiles.active()).c_str(), false);
    p.putUChar("buzzen", en ? 1 : 0);
    p.end();
    gBuzzerEnable = en;
}
void saveSettings(float capacity, float deadband, int tzMin) {
    Preferences p;
    p.begin(settingsNs(gProfiles.active()).c_str(), false);
    p.putFloat("battcap", capacity);
    p.putFloat("deadband", deadband);
    p.putInt("tzmin", tzMin);
    p.end();
    gBattCapacity = capacity;
    gDeadband = deadband;
    gTzOffsetMin = tzMin;
}
void saveAlertSettings(float socWarn, float socCrit, float vLow, float vHigh) {
    Preferences p;
    p.begin(settingsNs(gProfiles.active()).c_str(), false);
    p.putFloat("socwarn", socWarn);
    p.putFloat("soccrit", socCrit);
    p.putFloat("vlow", vLow);
    p.putFloat("vhigh", vHigh);
    p.end();
    gSocWarn = socWarn;
    gSocCrit = socCrit;
    gVlow = vLow;
    gVhigh = vHigh;
}

// Local unix seconds (TZ offset already applied) once NTP has synced, else 0.
// Used only for the daily-stats midnight rollover and "since" labels.
// Manually-set clock (from the AP "Set time"): the UTC epoch supplied and the
// millis() at which it was set. RAM-only, so it's lost on reboot (the run-time
// day odometer takes over until it's set again). 0 = not set.
uint32_t gManualEpoch = 0;
uint32_t gManualMillis = 0;

// Best-known UTC unix seconds, or 0 if no clock at all. Prefers a real NTP fix,
// else the manually-set / restored / master-supplied clock base (which is UTC).
uint32_t currentUtcEpoch() {
    time_t t = time(nullptr);
    if (t >= 1700000000) return static_cast<uint32_t>(t);                        // NTP
    if (gManualEpoch) return gManualEpoch + (millis() - gManualMillis) / 1000;   // manual base is UTC
    return 0;
}

// Local (TZ-adjusted) unix seconds, or 0 if no clock at all. 0 tells Stats to
// fall back to run-time day boundaries.
uint32_t currentLocalEpoch() {
    uint32_t u = currentUtcEpoch();
    return u ? u + gTzOffsetMin * 60 : 0;
}

// Rough clock persistence (NVS ns "vicclock"): the manual clock is RAM-only, so a
// reboot would lose it. We snapshot the current UTC epoch to flash periodically
// and restore it at boot, so the time survives resets to a rough order (it lags
// by up to the save interval + the powered-off duration — fine for the daily-
// stats midnight rollover; NTP corrects it exactly if it ever syncs).
//
// Flash wear is a non-issue: NVS appends each rewrite into a 4KB page (~126
// entries) and only *erases* a page on compaction, so ~1 erase per ~126 writes,
// wear-levelled across the partition. At the 60s cadence below that's ~1440
// writes/day ≈ 11 page-erases/day; against ~100k erase cycles/sector this is
// decades of life. It could go faster still, but 60s already bounds the worst-
// case backwards jump (zero-downtime reboot) to a minute, which is plenty.
static constexpr uint32_t kClockSaveMs = 60000;  // snapshot the clock every 1 min
static void loadClock() {
    Preferences p;
    p.begin("vicclock", true);
    uint32_t e = p.getUInt("utc", 0);
    p.end();
    if (e > 1700000000u) { gManualEpoch = e; gManualMillis = millis(); }
}
void saveClock() {
    if (gRole != ROLE_MASTER) return;  // master owns the clock; a slave takes it live
    uint32_t u = currentUtcEpoch();
    if (u <= 1700000000u) return;  // nothing worth saving yet
    Preferences p;
    p.begin("vicclock", false);
    p.putUInt("utc", u);
    p.end();
#ifdef VICMON_HAS_M5CAPSULE
    // Push the best-known time back into the RTC so it survives power loss and, next
    // boot, seeds the system clock without needing NTP. Cheap (~1 I2C write/min).
    if (capsulePresent()) capsuleRtcSet(u);
#endif
}
// Persist the clock on the kClockSaveMs cadence (see wear note above). Master-only
// — the slave takes its time live from the master and never writes it to NVS.
void serviceClockPersist() {
    static uint32_t lastSaveMs = 0;
    uint32_t now = millis();
    if (now - lastSaveMs >= kClockSaveMs) { lastSaveMs = now; saveClock(); }
}

// Set the clock from a manual entry (Settings page) or the browser ("Now"). Moves
// the ACTUAL system clock via settimeofday so time()/currentUtcEpoch() report it
// immediately — the old code only set gManualEpoch, which currentUtcEpoch ignores
// once time() is already valid (e.g. seeded from the M5Capsule RTC), so "Now" did
// nothing there. saveClock() then persists it (NVS + RTC write-through).
void setManualClock(uint32_t utc) {
    if (utc < 1700000000u) return;
    struct timeval tv = { (time_t)utc, 0 };
    settimeofday(&tv, nullptr);
    gManualEpoch = utc;
    gManualMillis = millis();
    saveClock();
}

// Convert a local wall-clock (Y/Mo/D h:m, using the configured TZ offset) to a UTC
// epoch. Uses Howard Hinnant's days-from-civil so it needs no libc calendar calls.
uint32_t localClockToUtc(int Y, int Mo, int D, int h, int m) {
    int yy = Y - (Mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153u * (Mo > 2 ? Mo - 3 : Mo + 9) + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468;
    int64_t local = days * 86400LL + (int64_t)h * 3600 + (int64_t)m * 60;
    return (uint32_t)(local - (int64_t)gTzOffsetMin * 60);
}

// Loads a profile's config/signals/settings and clears runtime caches so the
// mimic, history and discovery don't mix data across profiles.
void applyProfile(int pid) {
    RegLock lk;  // reloads the whole registry — exclude the loop's readers
    gConfig.begin(pid);
    gSignals.begin(gConfig.slots(), gConfig.count(), pid);
    loadSettings(pid);
    gStats.begin(pid);
    gDiscN = 0;
    gFine.clear();
    gCoarse.clear();
#ifndef VICMON_SIM
    loadHistFile(pid);  // restore persisted history for this profile (no-op under sim)
#endif
}

// Erases a profile's persisted data (devices / signals / settings).
void wipeProfile(int pid) {
    String dns = pid == 0 ? String("vicmon") : "vicmon" + String(pid);
    String sns = pid == 0 ? String("vicsig2") : "vicsig2_" + String(pid);
    String tns = settingsNs(pid);
    String stn = pid == 0 ? String("vicstat") : "vicstat" + String(pid);
    Preferences p;
    p.begin(dns.c_str(), false); p.clear(); p.end();
    p.begin(sns.c_str(), false); p.clear(); p.end();
    p.begin(tns.c_str(), false); p.clear(); p.end();
    p.begin(stn.c_str(), false); p.clear(); p.end();
    if (gFsOk) LittleFS.remove(histPath(pid));
}

String gStaSsid, gStaPass;
void loadWifi() {
    Preferences p;
    p.begin("vicwifi", true);
    gStaSsid = p.getString("ssid", "");
    gStaPass = p.getString("pass", "");
    p.end();
}
void saveWifiCreds(const String& s, const String& pw) {
    Preferences p;
    p.begin("vicwifi", false);
    p.putString("ssid", s);
    p.putString("pass", pw);
    p.end();
}

// Charge mode from the (possibly invalid) battery current, honouring the idle
// deadband. One definition shared by the panel/data JSON, the LED, and the
// ESP-NOW snapshot so the threshold can't drift between them.
// enum class ChargeMode moved to app.h
ChargeMode chargeMode(const sig::Resolved& ba) {
    if (!ba.valid) return ChargeMode::Unknown;
    if (ba.value > gDeadband) return ChargeMode::Charging;
    if (ba.value < -gDeadband) return ChargeMode::Discharging;
    return ChargeMode::Idle;
}
const char* chargeModeName(ChargeMode m) {
    switch (m) {
        case ChargeMode::Charging: return "charging";
        case ChargeMode::Discharging: return "discharging";
        case ChargeMode::Idle: return "idle";
        default: return "unknown";
    }
}

// Capitalised variant for the display banner (the JSON APIs keep the lowercase
// names above). The display's modeColor() matches these exact strings, so this
// also drives the banner colour (Charging green / Discharging red / Idle grey).
const char* chargeModeDisplayName(ChargeMode m) {
    switch (m) {
        case ChargeMode::Charging: return "Charging";
        case ChargeMode::Discharging: return "Discharging";
        case ChargeMode::Idle: return "Idle";
        default: return "--";
    }
}

// ---- alerts + status LED ---------------------------------------------------

// AtomS3 Lite onboard SK6812 RGB LED. Overridable per board.
#ifndef RGB_LED_PIN
#define RGB_LED_PIN 35
#endif

// Builds the alerts JSON array for `outArr` and returns the worst severity
// (0 = none, 1 = warning, 2 = critical). Thresholds of 0 disable that check.
// Returns the worst severity (0/1/2). If outArr is non-null, also serialises the
// alerts as a JSON array — severity-only callers (LED, ESP-NOW) pass nullptr to
// skip the string building on the hot path.
int buildAlerts(uint32_t now, String* outArr = nullptr) {
    if (outArr) *outArr = "[";
    int worst = 0;
    bool first = true;
    auto emit = [&](int sev, const String& msg) {
        if (sev > worst) worst = sev;
        if (!outArr) return;
        if (!first) *outArr += ",";
        first = false;
        *outArr += "{\"sev\":\"" + String(sev == 2 ? "crit" : "warn") + "\",\"msg\":\"" + msg + "\"}";
    };
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved bv = R(sig::Role::BatteryV, now);
    if (soc.valid) {
        if (gSocCrit > 0 && soc.value <= gSocCrit)
            emit(2, "Battery critically low (" + String(soc.value, 0) + "%)");
        else if (gSocWarn > 0 && soc.value <= gSocWarn)
            emit(1, "Battery low (" + String(soc.value, 0) + "%)");
    }
    if (bv.valid) {
        if (gVlow > 0 && bv.value <= gVlow)
            emit(2, "Battery voltage low (" + String(bv.value, 2) + "V)");
        if (gVhigh > 0 && bv.value >= gVhigh)
            emit(2, "Battery voltage high (" + String(bv.value, 2) + "V)");
    }
    // A configured device we've heard before that has now gone quiet. Use a
    // longer window than the display's 15 s staleness so a few missed adverts
    // (low BLE duty cycle vs the device's ~1/s rate) don't flap the warning.
    const uint32_t kOfflineMs = 60000;
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        if (s.everSeen && s.stale(now, kOfflineMs))
            emit(1, jsonEsc(String(s.name)) + " not responding");  // name is user input
    }
    if (outArr) *outArr += "]";
    return worst;
}

// Reflect the worst alert / charge mode on the onboard RGB LED (dim values to
// avoid a blinding indicator in a dark cab).
static void updateLed(int worst, ChargeMode mode) {
    uint8_t r = 0, g = 0, b = 0;
    if (worst >= 2) { r = 40; }
    else if (worst == 1) { r = 40; g = 22; }
    else if (mode == ChargeMode::Charging) { g = 30; }
    else if (mode == ChargeMode::Discharging) { g = 6; b = 22; }
    else { r = g = b = 2; }  // idle / unknown: faint white
#ifdef VICMON_HAS_M5CAPSULE
    // The Capsule's WS2812 (GPIO21) is owned by its button/LED task, which overlays
    // the pairing flash on top of this steady colour — publish, don't write, so the
    // two tasks never drive the RMT peripheral at the same time.
    if (capsulePresent()) { capsuleSetLed(r, g, b); return; }
#endif
    neopixelWrite(RGB_LED_PIN, r, g, b);
}

// Resolve a field from the first configured device of a given type. Used for
// secondary display values that don't have their own signal role.
static sig::Resolved fieldOfType(victron::Record type, sig::Field f, uint32_t now) {
    for (size_t i = 0; i < gConfig.count(); ++i)
        if (gConfig.slots()[i].type == type)
            return sig::resolveField(gConfig.slots(), gConfig.count(), gConfig.slots()[i].name, f, now);
    return {};
}

// PanelModel (the canonical once-per-consumer panel snapshot) moved to app.h so
// the web / espnow / display serializers can all read the same shape.
PanelModel collectPanel(uint32_t now) {
    PanelModel p;
    p.soc = R(sig::Role::BatterySOC, now);
    p.battV = R(sig::Role::BatteryV, now);
    p.battA = R(sig::Role::BatteryA, now);
    p.consumed = R(sig::Role::BatteryConsumed, now);
    p.starterV = R(sig::Role::BatteryStarterV, now);
    p.ttg = R(sig::Role::BatteryTTG, now);
    p.solarA = resolveSignal(sig::Role::SolarA, now);
    p.solarW = R(sig::Role::SolarW, now);
    p.solarV = fieldOfType(victron::Record::SolarCharger, sig::Field::SolarBattV, now);
    p.chargerA = resolveSignal(sig::Role::ChargerA, now);
    p.dcdcInA = resolveSignal(sig::Role::DcDcInA, now);
    p.dcdcOutA = resolveSignal(sig::Role::DcDcOutA, now);
    p.dcdcInV = fieldOfType(victron::Record::OrionXs, sig::Field::DcDcInV, now);
    p.dcdcOutV = fieldOfType(victron::Record::OrionXs, sig::Field::DcDcOutV, now);
    p.loadA = resolveSignal(sig::Role::LoadA, now);
    p.loadDerived = roleIsDerived(sig::Role::LoadA);
    p.mode = chargeMode(p.battA);
    p.alertWorst = buildAlerts(now);
    p.capacity = gBattCapacity;
    return p;
}




// ---- simulator (VICMON_SIM) ------------------------------------------------
// Synthesizes a battery + solar + DC-DC so the whole web UI / charts / stats /
// alerts can be exercised on the bench with no Victron device in range. Enabled
// by the `atoms3-sim` build env; compiled out otherwise.

#ifdef VICMON_SIM
static DeviceSlot* simFind(const char* name) {
    for (size_t i = 0; i < gConfig.count(); ++i)
        if (strcmp(gConfig.slots()[i].name, name) == 0) return &gConfig.slots()[i];
    return nullptr;
}

static void simSetup() {
    uint8_t zero[16] = {0};
    if (!simFind("SimBMV")) gConfig.add("SimBMV", victron::Record::BatteryMonitor, zero);
    if (!simFind("SimSolar")) gConfig.add("SimSolar", victron::Record::SolarCharger, zero);
    if (!simFind("SimDCDC")) gConfig.add("SimDCDC", victron::Record::OrionXs, zero);
    // Bind the panel signals to the synthetic devices (in-memory only — NVS is
    // left untouched so a real profile's bindings survive).
    gSignals.set(sig::Role::BatterySOC, "SimBMV", sig::Field::BattSOC);
    gSignals.set(sig::Role::BatteryV, "SimBMV", sig::Field::BattV);
    gSignals.set(sig::Role::BatteryA, "SimBMV", sig::Field::BattA);
    gSignals.set(sig::Role::BatteryConsumed, "SimBMV", sig::Field::BattConsumed);
    gSignals.set(sig::Role::BatteryStarterV, "SimBMV", sig::Field::BattAuxStarterV);
    gSignals.set(sig::Role::SolarA, "SimSolar", sig::Field::SolarBattA);
    gSignals.set(sig::Role::SolarW, "SimSolar", sig::Field::SolarPvW);
    gSignals.set(sig::Role::DcDcInA, "SimDCDC", sig::Field::DcDcInA);
    gSignals.set(sig::Role::DcDcOutA, "SimDCDC", sig::Field::DcDcOutA);
    gSignals.set(sig::Role::LoadA, sig::kLoadOnly, sig::Field::None);
    if (gBattCapacity <= 0) gBattCapacity = 100;  // so the mimic shows remaining Ah
    Serial.println("[SIM] synthetic BMV/Solar/DC-DC installed");
}

static float gSimSoc = 70.0f;
static float gSimYield = 0.0f;

static void simTick() {
    uint32_t now = millis();
    float phase = fmodf(now / 1000.0f, 120.0f) / 120.0f;  // one "day" every 120 s
    float sun = sinf(phase * 2.0f * PI);
    float solarA = sun > 0 ? sun * 18.0f : 0.0f;           // up to 18 A at noon
    bool engine = (phase > 0.60f && phase < 0.80f);        // a "drive" window
    float dcdcA = engine ? 28.0f : 0.0f;
    float loadA = 4.0f + 3.0f * (0.5f + 0.5f * sinf(phase * 6.0f * PI));  // 4–10 A
    float netA = solarA + dcdcA - loadA;

    static uint32_t last = 0;
    float dt = last ? (now - last) / 1000.0f : 0;
    last = now;
    gSimSoc += netA * (dt / 3600.0f) / 100.0f * 100.0f * 30.0f;  // sped up 30x
    if (gSimSoc > 100) gSimSoc = 100;
    if (gSimSoc < 5) gSimSoc = 5;
    gSimYield += (solarA * 13.0f) * (dt / 3600.0f) / 1000.0f;  // kWh

    float vbatt = 12.0f + gSimSoc / 100.0f * 1.0f + (netA > 0 ? 0.3f : 0.0f);

    DeviceSlot* bmv = simFind("SimBMV");
    if (bmv) {
        bmv->battery.soc = gSimSoc; bmv->battery.socValid = true;
        bmv->battery.voltage = vbatt; bmv->battery.voltageValid = true;
        bmv->battery.current = netA; bmv->battery.currentValid = true;
        bmv->battery.consumedAh = -(100.0f - gSimSoc); bmv->battery.consumedValid = true;
        bmv->battery.auxMode = victron::AuxMode::StarterVoltage;
        bmv->battery.auxValue = 12.5f + (engine ? 1.6f : 0.0f); bmv->battery.auxValid = true;
        bmv->everSeen = true; bmv->lastSeenMs = now;
    }
    DeviceSlot* sol = simFind("SimSolar");
    if (sol) {
        sol->solar.batteryVoltage = vbatt; sol->solar.battVValid = true;
        sol->solar.batteryCurrent = solarA; sol->solar.battIValid = true;
        sol->solar.pvPower = solarA * 13.0f; sol->solar.pvValid = true;
        sol->solar.yieldToday = gSimYield; sol->solar.yieldValid = true;
        sol->solar.deviceState = solarA > 0.1f ? 3 : 0;
        sol->everSeen = true; sol->lastSeenMs = now;
    }
    DeviceSlot* dc = simFind("SimDCDC");
    if (dc) {
        dc->dcdc.outputVoltage = vbatt; dc->dcdc.outputVValid = true;
        dc->dcdc.outputCurrent = dcdcA; dc->dcdc.outputIValid = true;
        dc->dcdc.inputVoltage = engine ? 14.2f : 12.4f; dc->dcdc.inputVValid = true;
        dc->dcdc.inputCurrent = dcdcA * 0.95f; dc->dcdc.inputIValid = true;
        dc->dcdc.deviceState = engine ? 3 : 0;
        dc->everSeen = true; dc->lastSeenMs = now;
    }
}
#endif  // VICMON_SIM

// ---- slave role (dual-purpose): receive another master's broadcast ---------
// When the NVS role flag is SLAVE, setup() calls this instead of the full master
// bring-up: no BLE scan, no broadcaster — just the ESP-NOW receiver (gRx) feeding
// the same display, plus a lightweight config SoftAP so a headless slave is still
// configurable (pair / unpair / switch role). A SoftAP pins the radio to one
// channel, so the receiver runs non-hopping on channel 1 (the master's default).

static void setupSlave() {
    Serial.println("[boot] SLAVE role — ESP-NOW receiver + config AP (no BLE)");
    // Drop any history loaded from our own flash — on a slave it's stale; the
    // authoritative trend comes live + from the master's history pull.
    gFine.clear();
    gCoarse.clear();
    // Config SoftAP on channel 1 (matches the master's default AP channel).
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/2);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[slave] config AP '%s' at http://%s/  (pass %s, ch1)\n", kApSsid,
                  ip.toString().c_str(), kApPass);
    gDns.start(53, "*", ip);
    setupServer();  // the SAME full web app as the master (role-gated + snapshot-fed)
    if (MDNS.begin("vicmon")) MDNS.addService("http", "tcp", 80);

    gRx.begin("vicslave", /*manageWifi=*/false);  // AP owns ch1; don't hop
    gEspNowOk = gRx.ok();
    Serial.println(gRx.ok() ? "ESP-NOW receiver ready (ch1)" : "ESP-NOW receiver init FAILED");
    if (gRx.isPaired()) Serial.printf("Paired to master %08X\n", gRx.pairedMaster());
    else Serial.println("Unpaired — Pair from the AP page / Diag tab / button");
    setupOta(ROLE_SLAVE);  // firmware clone: receive a push from (or push to) the master
#ifdef VICMON_DISPLAY
    bringUpDisplay();
#endif
}

// Load a completed history pull into the trend rings (chronological, native
// resolution) so the Graph page is fully populated; live frames extend it after.
static void applyPulledHistory() {
    // HistPointW and HistSample are deliberately the same sequence of int16
    // channels (asserted below, and relied on by sendHistChunk's memcpy), so copy
    // the whole point rather than listing fields.
    //
    // This used to brace-init the six electrical fields by name. When the four
    // environment channels were added, that init silently VALUE-INITIALISED them
    // to 0 — so every pulled backlog sample arrived as 0 degC / 0 %RH / 0 hPa
    // instead of "not available", and the Environment chart auto-scaled itself
    // from 0 to 1019 hPa, burying the real data in a flat line at the top. A
    // memcpy cannot silently drop a channel the next time one is added.
    static_assert(sizeof(HistSample) == sizeof(slavelink::HistPointW),
                  "HistSample and HistPointW must stay layout-compatible");
    auto take = [](const slavelink::HistPointW& p) {
        HistSample s;
        memcpy(&s, &p, sizeof(s));
        return s;
    };
    // Only replace a ring the master actually sent samples for — otherwise keep
    // the slave's own live-accumulated ring (don't wipe 12h/24h when the master
    // has no coarse history yet).
    if (gRx.fineCount() > 0) {
        gFine.clear();
        for (uint16_t i = 0; i < gRx.fineCount(); ++i) gFine.push(take(gRx.finePoint(i)), millis());
    }
    if (gRx.coarseCount() > 0) {
        gCoarse.clear();
        for (uint16_t i = 0; i < gRx.coarseCount(); ++i)
            gCoarse.push(take(gRx.coarsePoint(i)), millis());
    }
    if (gRx.envCount() > 0) {
        static_assert(sizeof(EnvSample) == sizeof(slavelink::EnvPointW), "EnvSample and EnvPointW must stay layout-compatible");
        gEnv.clear();
        for (uint16_t i = 0; i < gRx.envCount(); ++i) {
            EnvSample e; memcpy(&e, &gRx.envPoint(i), sizeof(e));
            gEnv.push(e, millis());
        }
    }
}

// A wire-version bump silences telemetry between a master and a slave on
// different firmware; before this, the slave just showed STALE until someone
// updated it by hand. The clone frames carry their own protocol version and
// the version beacon is version-independent, so the slave can tell that its
// paired master is on a different wire version AND whether the master's
// firmware is newer — and if so, pull it. Only a pull, never a push: updating
// yourself is safe, rebooting the master unasked is not. When this unit is the
// newer one it says so instead, and the Network page's push button does it.
static void serviceUpgradeWatch() {
    static uint32_t lastTryMs = 0; static uint8_t tries = 0;
    const bool mismatch = gRx.foreignMasterSeen() && gRx.pairedMaster() != 0 &&
                          gRx.foreignMasterId() == gRx.pairedMaster() &&
                          gRx.foreignMasterVersion() != slavelink::kVersion;
    const char* rel = gOta.peerKnown() ? gOta.peerRel() : "";
    if (!mismatch) {
        // Same wire, but the master's firmware is newer: the link still works, so
        // nothing is pulled unasked (a bench master gets flashed twenty times a
        // day) — but say so, so the Network page's "update from peer" is a click.
        gLinkMismatch = (strcmp(rel, "newer") == 0) ? 3 : 0;
        tries = 0; return;
    }
    if (strcmp(rel, "older") == 0) { gLinkMismatch = 2; return; }
    gLinkMismatch = 1;
    if (strcmp(rel, "newer") != 0 || gOta.busy()) return;   // wait for the beacon / a transfer in progress
    // The first pull reliably misses: the master starts offering only after the
    // slave's acceptance window has closed. 30s (was 90s) makes the retry that
    // engages come a minute sooner; a pull is one small broadcast.
    if (tries >= 8 || (lastTryMs && millis() - lastTryMs < 30000)) return;
    lastTryMs = millis(); ++tries;
    Serial.printf("[upgrade] master on wire v%u, this unit v%u, master firmware %s is newer — pulling (try %u)\n",
                  gRx.foreignMasterVersion(), (unsigned)slavelink::kVersion, gOta.peerVersion(), tries);
    gOta.startPull();
}

static void slaveLoop() {
    gDns.processNextRequest();
    gRx.poll();
    serviceSupervisor(millis());
    serviceSlaveSerial();  // `week` / `mem` dumps (the slave has no other console)
    serviceRole();  // "Switch to Master" (reboots)
    serviceOta();   // firmware clone push/receive state machine
    if (gOta.busy()) {  // dedicate the loop to the transfer
#ifdef VICMON_DISPLAY
        static uint32_t lastOtaPub = 0;  // still refresh the LCD so progress animates
        if (gDisplayOk && millis() - lastOtaPub > 200) { lastOtaPub = millis(); serviceDashRequests(); publishSlaveDash(); }
#endif
        return;
    }

    // Live trend ALWAYS runs, so the Graph populates from now immediately and is
    // never blocked on the backlog pull (a slow/stuck pull can't stall it). The
    // pull fetches the older history in the background and splices it in when it
    // completes.
    sampleSlaveHistory();

    static bool gHistApplied = false;
    static uint32_t gLiveSince = 0;
    // Re-homed to a different master (via Pair without unpair)? Re-pull its backlog
    // instead of keeping the old master's history.
    static uint32_t gLastPaired = 0;
    if (gRx.pairedMaster() != gLastPaired) {
        gLastPaired = gRx.pairedMaster();
        gHistApplied = false;
        gLiveSince = 0;
        gFine.clear();
        gCoarse.clear();
        gEnv.clear();
        gEnvFine.clear();
    }
    serviceUpgradeWatch();
    if (gRx.live() && gRx.haveMasterMac()) {
        if (gLiveSince == 0) gLiveSince = millis();
        // Wait until the link has been solidly live for a few seconds (first
        // status packets in + channel/peer settled) before asking for the backlog.
        if (!gHistApplied && !gRx.histActive() && !gRx.historyReady() && millis() - gLiveSince > 5000)
            gRx.requestHistory();
        if (gRx.historyReady() && !gHistApplied) {
            applyPulledHistory();  // splice in the older history in one step
            gHistApplied = true;
            Serial.printf("[slave] graph backlog loaded: %u fine + %u coarse pts\n",
                          gRx.fineCount(), gRx.coarseCount());
        }
    } else {
        gLiveSince = 0;
    }
#ifdef VICMON_DISPLAY
    if (gDisplayOk) { serviceDashRequests(); publishSlaveDash(); }  // apply tunable/brightness taps
#endif
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last >= 1000) {
        last = now;
        if (gRx.isPaired())
            Serial.printf("[slave] %08X %s ch%u drops=%lu%s\n", gRx.pairedMaster(),
                          gRx.live() ? "live" : "stale", gRx.channel(), (unsigned long)gRx.drops(),
                          gRx.histActive() ? " syncing-history" : "");
        else
            Serial.printf("[slave] unpaired ch%u %s\n", gRx.channel(),
                          gRx.isAdopting() ? "adopting" : "idle");
    }
    delay(20);
}

// ---- Arduino entry points --------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);

    // Safe-boot window: hold here briefly BEFORE any risky init (LittleFS, WiFi,
    // BLE, display). If a later stage crash-loops, the board still comes up alive
    // for this window every reset, so it can always be caught for reflashing
    // instead of the native-USB CDC re-enumerating too fast to grab. Cheap
    // insurance; kBootHoldMs can be trimmed for production.
    const uint32_t kBootHoldMs = 3000;
    for (int32_t left = kBootHoldMs; left > 0; left -= 1000) {
        Serial.printf("[boot] safe-boot hold, flash window %ld ms...\n", (long)left);
        delay(left < 1000 ? left : 1000);
    }
    Serial.println("\nVicmon Master: BLE + WiFi AP + display");
    recordBoot();

    // Stable per-chip id (low 32 bits of the factory MAC): identifies this master
    // in every ESP-NOW frame so slaves can filter/pair to it. Reads from efuse,
    // needs no init, so it's available before WiFi/ESP-NOW come up.
    gMasterId = (uint32_t)ESP.getEfuseMac();
    snprintf(kApSsid, sizeof(kApSsid), "Vicmon-%06X", (unsigned)(gMasterId & 0xFFFFFF));
    loadApCfg();  // apply a custom AP name/password if one was saved
    Serial.printf("[boot] master id %08X, AP '%s'\n", gMasterId, kApSsid);

    // Which display is wired to this board? Probe before any panel/role init so
    // both the master and slave bring-ups render on the right hardware.
    detectBoard();

#ifdef VICMON_HAS_M5CAPSULE
    // M5Capsule (headless): latch power, bring up RTC/buzzer/SD and seed the system
    // clock from the RTC. Runs in BOTH roles and before WiFi, so the clock is real
    // (no NTP needed) the moment the stats/logging code starts.
    if (gHwBoard == HW_M5CAPSULE) capsuleBringUp();
#endif

    // Registry mutex: created before the server/tasks so RegLock is live the
    // moment concurrent access becomes possible (it no-ops while null above).
    gRegMux = xSemaphoreCreateRecursiveMutex();

    // Shared init (history rings, LittleFS, profiles/config/signals) — needed by
    // the web app in BOTH roles, so it runs before the role branch.
    gFine.init(gFineBuf, HIST_CAP, HIST_INTERVAL);
    gEnv.init(gEnvBuf, ENV_CAP, ENV_INTERVAL);
    gEnvFine.init(gEnvFineBuf, ENVF_CAP, ENVF_INTERVAL);
    gCoarse.init(gCoarseBuf, HIST2_CAP, HIST2_INTERVAL);

    Serial.printf("[mem] boot: %u\n", (unsigned)ESP.getFreeHeap());
    gFsOk = LittleFS.begin(/*formatOnFail=*/true);
    Serial.printf("LittleFS: %s\n", gFsOk ? "mounted" : "unavailable (history not persisted)");
    Serial.printf("[mem] post-LittleFS: %u\n", (unsigned)ESP.getFreeHeap());

    gProfiles.begin();
    applyProfile(gProfiles.active());
    Serial.printf("Profile '%s': %u device(s)\n", gProfiles.name(gProfiles.active()),
                  (unsigned)gConfig.count());

    // Dual-purpose: a slave-role device skips the master-only bring-up (BLE scan,
    // broadcaster) but still serves the SAME web app (sourced from received data).
    loadRole();
    Serial.printf("[boot] role: %s\n", gRole == ROLE_SLAVE ? "SLAVE" : "MASTER");
    if (gRole == ROLE_SLAVE) { setupSlave(); return; }

#ifdef VICMON_HAS_ENVPRO
    // Unit ENV Pro on Grove Port A — MASTER role only. A slave's history and panel
    // are fed from the master's ESP-NOW snapshot (sampleSlaveHistory), so a locally
    // attached sensor there would be overwritten a moment later by the remote
    // reading; better to leave the bus alone than to fight over the same fields.
    // After capsuleBringUp(), which owns the separate internal I2C bus (Wire).
    if (gHwBoard == HW_M5CAPSULE) envBringUp();
#endif

    loadClock();  // master owns the clock: restore the rough time saved before reboot

#ifdef GUITION_NO_WIFI
    IPAddress ip;
    Serial.println("WiFi DISABLED (diag)");
#else
    loadWifi();
#ifdef GUITION_STA_ONLY
    // DIAG: STA mode, no SoftAP beacon — mimics an ESP-NOW display slave's light
    // WiFi footprint, to see if the LVGL+WiFi crash is SoftAP-specific.
    WiFi.mode(WIFI_STA);
    IPAddress ip = WiFi.softAPIP();
    Serial.println("WiFi STA mode (no AP)");
#else
    WiFi.mode(gStaSsid.length() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(kApSsid, kApPass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/2);
    IPAddress ip = WiFi.softAPIP();
    { uint8_t pc = 0; wifi_second_chan_t sc; esp_wifi_get_channel(&pc, &sc);
      Serial.printf("AP '%s' up at http://%s/  (pass: %s) channel %u\n", kApSsid,
                    ip.toString().c_str(), kApPass, pc); }
    Serial.printf("[mem] post-WiFiAP: %u\n", (unsigned)ESP.getFreeHeap());
#ifdef GUITION_SLOW_BEACON
    {  // DIAG: slow the AP beacon to reduce TX-vs-LVGL collisions.
        wifi_config_t c;
        if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {
            c.ap.beacon_interval = 1000;  // TU (~1.024ms); default 100
            esp_wifi_set_config(WIFI_IF_AP, &c);
            Serial.println("AP beacon_interval -> 1000");
        }
    }
#endif
#endif
    if (gStaSsid.length()) {
        WiFi.begin(gStaSsid.c_str(), gStaPass.c_str());
        Serial.printf("Joining WiFi '%s'...\n", gStaSsid.c_str());
        // NTP for the daily-stats rollover; offset is applied in currentLocalEpoch().
        configTime(0, 0, "pool.ntp.org");
    }
#endif  // GUITION_NO_WIFI

#ifndef GUITION_MINSYS
    gDns.start(53, "*", ip);
    setupServer();

    // mDNS (http://vicmon.local/) costs ~6KB heap and only helps when the device is
    // joined to a router — on its own SoftAP clients use 192.168.4.1 directly. So
    // start it ONLY in STA mode; AP-only (the offline 4WD norm) skips it and keeps
    // the 6KB. Re-provision WiFi and reboot to get vicmon.local back.
    if (gStaSsid.length() && MDNS.begin("vicmon")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: http://vicmon.local/");
    }

    Serial.printf("[mem] post-server: %u\n", (unsigned)ESP.getFreeHeap());
    setupEspNow();  // live data broadcast to slaves
    setupOta(ROLE_MASTER);  // firmware clone: push to (or receive from) a paired slave
    Serial.printf("[mem] post-espnow: %u\n", (unsigned)ESP.getFreeHeap());
#endif

#ifdef VICMON_SIM
    simSetup();
#else
    NimBLEDevice::init("");
    Serial.printf("[mem] post-NimBLE-init: %u\n", (unsigned)ESP.getFreeHeap());
    gScan = NimBLEDevice::getScan();
    // Active scan so we also receive scan responses, which carry the device's
    // friendly name (Victron puts it there, not in the advertisement).
    gScan->setActiveScan(true);
    // BLE/WiFi radio split: window/interval = 50% duty. Higher BLE duty catches
    // adverts faster but leaves the WiFi AP less airtime (near-100% duty made the
    // AP unjoinable; 30% was very safe). 50:50 is a deliberate middle ground.
    gScan->setInterval(160);
    gScan->setWindow(80);
    // Every device seen in a scan window is held as a heap object until the
    // results are cleared. Measured 13-18 devices / 2.4-8.7KB here; a car park or
    // marina could be several times that on a board with ~70KB free. Cap it.
    // Decode in the callback and store NOTHING: with maxResults 0 the library
    // never allocates a per-device object, so a scan window costs no heap at all
    // regardless of how many devices are advertising nearby.
    gScan->setAdvertisedDeviceCallbacks(&gIngestCb, /*wantDuplicates=*/false);
    gScan->setMaxResults(0);
#endif

#ifdef VICMON_DISPLAY
    bringUpDisplay();  // panel + touch + display task (core 1)
#endif
    webPreallocate();  // fixed build arena, taken LAST so the big early library blocks stay contiguous
    Serial.printf("[mem] setup done: %u\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
    if (gRole == ROLE_SLAVE) { slaveLoop(); return; }
    serviceSupervisor(millis());
    serviceMasterSerial();  // `pair` / `role` console commands
    serviceRole();          // consume a serial/web role-toggle on headless masters
    serviceOta();           // firmware clone push/receive state machine
    if (gOta.busy()) {      // dedicate the loop to the transfer (skip the ~2s BLE scan)
#ifdef VICMON_DISPLAY
        static uint32_t lastOtaPub = 0;  // still refresh the LCD so progress animates
        if (gDisplayOk && millis() - lastOtaPub > 200) { lastOtaPub = millis(); serviceDashRequests(); publishDash(); }
#endif
        return;
    }

#ifndef GUITION_MINSYS
    gDns.processNextRequest();
#endif
#ifdef VICMON_SIM
    simTick();
    delay(500);  // pace the sim loop (no blocking BLE scan to do it for us)
#else
    pollBle();  // blocks ~2s per scan
#endif
    // Everything below reads/iterates the registry; hold gRegMux so a web-task
    // config write (add/remove/import/profile switch) can't restructure the slot
    // array mid-iteration. Recursive: serviceDashRequests() may re-lock to apply
    // a deferred profile switch.
    RegLock regLk;
    updateDerivedSmoothing();  // refresh median-smoothed derived signals
#ifdef VICMON_HAS_ENVPRO
    envService(millis());  // non-blocking: kicks off / collects a BME688 measurement
#endif
    sampleHistory();  // continuous logging, regardless of any connected client
    sampleStats();    // integrate energy counters / trip stats

    uint32_t now = millis();
    serviceClockPersist();  // master-only: snapshot the rough clock to NVS every ~5 min
    int worst = buildAlerts(now);
#ifdef VICMON_HAS_M5CAPSULE
    if (gHwBoard == HW_M5CAPSULE) {
        // Audible alarm tracks the SoC-critical alert specifically (battery capacity),
        // not the whole worst-severity (which also covers voltage / stale devices).
        sig::Resolved soc = R(sig::Role::BatterySOC, now);
        bool socCrit = soc.valid && gSocCrit > 0 && soc.value <= gSocCrit;
        capsuleServiceBuzzer(socCrit, now);
        capsuleLogSample(now);  // append to the daily SD CSV (paced 60 s internally)
        // Drive the Capsule's WS2812 (GPIO21). The generic updateLed() call below is
        // compiled out in this display-capable universal image, so do it here.
        updateLed(worst, chargeMode(R(sig::Role::BatteryA, now)));
    }
#endif
#ifndef VICMON_DISPLAY
    // Status LED on GPIO 35 — but on the Guition (OPI PSRAM) GPIO 35 is a PSRAM
    // data pin, and driving it corrupts the framebuffer. The Guition has no user
    // RGB LED anyway, so skip it there.
    updateLed(worst, chargeMode(R(sig::Role::BatteryA, now)));
#endif

#ifdef VICMON_DISPLAY
    if (gDisplayOk) { serviceDashRequests(); publishDash(); }
#endif

#ifndef VICMON_SIM
    if (gFsOk && now - gLastHistSaveMs >= kHistSaveMs) {
        gLastHistSaveMs = now;
        saveHistFile(gProfiles.active());
    }
#endif

#ifndef GUITION_MINSYS
    static uint32_t lastBroadcast = 0;  // refresh the live snapshot ~1/s (timer TXes it)
    if (now - lastBroadcast >= 1000) {
        lastBroadcast = now;
        sendSlaveBroadcast();
    }
    static uint32_t lastStats = 0;  // 7-day energy frame for a slave's Week page
    if (now - lastStats >= 4000) {
        lastStats = now;
        sendStatsFrame();
    }
    // serviceHistSend() is pumped from broadcastTick (250ms) so the paced reply
    // isn't bottlenecked by this loop's ~2s BLE scan.
#endif

    Serial.printf("[state] heap=%u/%u scan=%d(%uB) victron_adverts=%d decoded=%d |",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                  gScanResults, (unsigned)gScanHeapCost, gScanVictron, gScanDecoded);
    for (size_t i = 0; i < gConfig.count(); ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
    }
    Serial.println();
}
