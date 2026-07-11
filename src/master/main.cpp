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
#include <esp_now.h>
#include <esp_timer.h>
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
#include "app.h"          // shared state/types + the board seam (VICMON_DISPLAY)
#include "web_assets.h"  // kStyle / kMimicPage / kStatsPage / kDiagPage (HTML/CSS/JS)

// AP SSID is made unique per device at boot (Vicmon-<last 3 MAC bytes>) so
// several masters in the same area don't collide — filled in setup() once the
// master id is known; the default is only a placeholder before then.
char kApSsid[24] = "Vicmon";         // default Vicmon-<mac3>; overridable via NVS (loadApCfg)
char kApPass[24] = "vicmon1234";     // >= 8 chars; overridable via NVS (loadApCfg)
const char* kFwVersion = "0.3.0";    // shown on the display Settings page

DeviceConfig gConfig;
sig::SignalMap gSignals;
ProfileManager gProfiles;
stats::Stats gStats;
NimBLEScan* gScan = nullptr;
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
    Serial.printf("[role] switching to %s, rebooting...\n", nr == ROLE_SLAVE ? "SLAVE" : "MASTER");
    delay(300);
    ESP.restart();
}
// Consume a role-toggle request (raised by the Diag tab). Called from both loops.
void serviceRole() {
    if (gRoleReq) { gRoleReq = false; applyRoleToggle(); }
    if (gRebootReq) { Serial.println("[cfg] rebooting to apply..."); delay(300); ESP.restart(); }
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

static float median3(float a, float b, float c) {
    return a < b ? (b < c ? b : (a < c ? c : a)) : (a < c ? a : (b < c ? c : b));
}

// Median-of-3 smoother over recent BLE polls, applied to the derived
// energy-balance signals so they don't flicker for a second or two when one
// contributing device advertises before another (e.g. the DC-DC drops to 0 a
// beat before the BMV current refreshes). Measured device readings stay raw.
struct Smoothed {
    float ring[3] = {0, 0, 0};
    int n = 0, pos = 0;
    float value = 0;
    bool valid = false;
    void push(bool v, float x) {
        valid = v;
        if (!v) return;
        ring[pos] = x;
        pos = (pos + 1) % 3;
        if (n < 3) ++n;
        value = (n < 3) ? x : median3(ring[0], ring[1], ring[2]);  // settle before smoothing
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
HistRing gFine, gCoarse;

static int16_t encA(bool v, float a) {
    return v ? static_cast<int16_t>(lroundf(a * 10.0f)) : -32768;
}
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

// Runs continuously from loop() regardless of any connected client. Feeds both
// rings from a single read.
static void sampleHistory() {
    uint32_t now = millis();
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
    if (!dueFine && !dueCoarse) return;
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
}

// ---- history persistence (LittleFS) ----------------------------------------
// Both ring buffers are written to a per-profile file every few minutes (and on
// a profile switch) so the 1 h / 24 h charts survive a reboot. There are no
// timestamps in the data, so the gap during downtime simply isn't represented —
// reloaded samples continue seamlessly at the "now" edge.

bool gFsOk = false;
static const uint8_t kHistVer = 2;  // bumped when HistSample gained `soc`
// How often the history is flushed to flash. At ~26 KB/save (full buffers) this
// is ~7.5 MB/day; LittleFS wear-levels it across the ~1.5 MB FS partition, so at
// 100k erase cycles/block the flash lasts decades. Raise it to lose less to
// wear (at the cost of losing a little more recent history on an unclean reboot).
static const uint32_t kHistSaveMs = 5 * 60 * 1000;

static String histPath(int profile) { return "/hist" + String(profile) + ".bin"; }

// Write a ring's samples oldest-first.
static void writeRing(File& f, const HistRing& r) {
    size_t start = (r.head + r.cap - r.count) % r.cap;
    for (size_t k = 0; k < r.count; ++k) {
        size_t idx = (start + k) % r.cap;
        f.write(reinterpret_cast<const uint8_t*>(&r.buf[idx]), sizeof(HistSample));
    }
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

void saveHistFile(int profile) {
    if (!gFsOk) return;
    File f = LittleFS.open(histPath(profile), "w");
    if (!f) return;
    uint8_t hdr[4] = {'V', 'H', kHistVer, 0};
    uint16_t fc = gFine.count, cc = gCoarse.count;
    f.write(hdr, 4);
    f.write(reinterpret_cast<uint8_t*>(&fc), 2);
    f.write(reinterpret_cast<uint8_t*>(&cc), 2);
    writeRing(f, gFine);
    writeRing(f, gCoarse);
    f.close();
}

static void loadHistFile(int profile) {
    if (!gFsOk) return;
    File f = LittleFS.open(histPath(profile), "r");
    if (!f) return;
    uint8_t hdr[4];
    if (f.read(hdr, 4) != 4 || hdr[0] != 'V' || hdr[1] != 'H' || hdr[2] != kHistVer) {
        f.close();
        return;
    }
    uint16_t fc = 0, cc = 0;
    f.read(reinterpret_cast<uint8_t*>(&fc), 2);
    f.read(reinterpret_cast<uint8_t*>(&cc), 2);
    readRing(f, gFine, fc);
    readRing(f, gCoarse, cc);
    f.close();
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

String buildHistoryJson(int mins) {
    if (mins < 1) mins = 1;
    if (mins > 1440) mins = 1440;
    // Windows over 60 min read the coarse (60 s) buffer; shorter ones the fine (5 s).
    const HistRing& r = mins > 60 ? gCoarse : gFine;

    int want = mins * 60 * 1000 / static_cast<int>(r.intervalMs);
    if (want > static_cast<int>(r.count)) want = r.count;
    if (want < 0) want = 0;
    size_t start = (r.head + r.cap - want) % r.cap;
    const char* names[6] = {"battery", "solar", "charger", "dcdc", "load", "soc"};
    String j = "{\"interval\":" + String(r.intervalMs / 1000) +
               ",\"mins\":" + String(mins) + ",\"series\":{";
    for (int f = 0; f < 6; ++f) {
        j += "\"" + String(names[f]) + "\":[";
        for (int k = 0; k < want; ++k) {
            size_t idx = (start + k) % r.cap;
            int16_t v = sampleField(r.buf[idx], f);
            if (k) j += ",";
            j += (v == -32768) ? "null" : String(v / 10.0f, 1);
        }
        j += "]";
        if (f < 5) j += ",";
    }
    j += "}}";
    return j;
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
    p.end();
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

// Local (TZ-adjusted) unix seconds, or 0 if no clock at all. Prefers a real NTP
// fix, else the manually-set clock; 0 tells Stats to fall back to run-time days.
uint32_t currentLocalEpoch() {
    time_t t = time(nullptr);
    if (t >= 1700000000) return static_cast<uint32_t>(t) + gTzOffsetMin * 60;  // NTP
    if (gManualEpoch) return gManualEpoch + (millis() - gManualMillis) / 1000 + gTzOffsetMin * 60;
    return 0;  // no clock -> run-time day boundaries
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
    WiFi.softAP(kApSsid, kApPass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/4);
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
#ifdef VICMON_DISPLAY
    bringUpDisplay();
#endif
}

// Load a completed history pull into the trend rings (chronological, native
// resolution) so the Graph page is fully populated; live frames extend it after.
static void applyPulledHistory() {
    // Only replace a ring the master actually sent samples for — otherwise keep
    // the slave's own live-accumulated ring (don't wipe 12h/24h when the master
    // has no coarse history yet).
    if (gRx.fineCount() > 0) {
        gFine.clear();
        for (uint16_t i = 0; i < gRx.fineCount(); ++i) {
            const slavelink::HistPointW& p = gRx.finePoint(i);
            HistSample s{p.battery, p.solar, p.charger, p.dcdc, p.load, p.soc};
            gFine.push(s, millis());
        }
    }
    if (gRx.coarseCount() > 0) {
        gCoarse.clear();
        for (uint16_t i = 0; i < gRx.coarseCount(); ++i) {
            const slavelink::HistPointW& p = gRx.coarsePoint(i);
            HistSample s{p.battery, p.solar, p.charger, p.dcdc, p.load, p.soc};
            gCoarse.push(s, millis());
        }
    }
}

static void slaveLoop() {
    gDns.processNextRequest();
    gRx.poll();
    serviceRole();  // "Switch to Master" (reboots)

    // Live trend ALWAYS runs, so the Graph populates from now immediately and is
    // never blocked on the backlog pull (a slow/stuck pull can't stall it). The
    // pull fetches the older history in the background and splices it in when it
    // completes.
    sampleSlaveHistory();

    static bool gHistApplied = false;
    static uint32_t gLiveSince = 0;
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

    // Stable per-chip id (low 32 bits of the factory MAC): identifies this master
    // in every ESP-NOW frame so slaves can filter/pair to it. Reads from efuse,
    // needs no init, so it's available before WiFi/ESP-NOW come up.
    gMasterId = (uint32_t)ESP.getEfuseMac();
    snprintf(kApSsid, sizeof(kApSsid), "Vicmon-%06X", (unsigned)(gMasterId & 0xFFFFFF));
    loadApCfg();  // apply a custom AP name/password if one was saved
    Serial.printf("[boot] master id %08X, AP '%s'\n", gMasterId, kApSsid);

    // Registry mutex: created before the server/tasks so RegLock is live the
    // moment concurrent access becomes possible (it no-ops while null above).
    gRegMux = xSemaphoreCreateRecursiveMutex();

    // Shared init (history rings, LittleFS, profiles/config/signals) — needed by
    // the web app in BOTH roles, so it runs before the role branch.
    gFine.init(gFineBuf, HIST_CAP, HIST_INTERVAL);
    gCoarse.init(gCoarseBuf, HIST2_CAP, HIST2_INTERVAL);

    gFsOk = LittleFS.begin(/*formatOnFail=*/true);
    Serial.printf("LittleFS: %s\n", gFsOk ? "mounted" : "unavailable (history not persisted)");

    gProfiles.begin();
    applyProfile(gProfiles.active());
    Serial.printf("Profile '%s': %u device(s)\n", gProfiles.name(gProfiles.active()),
                  (unsigned)gConfig.count());

    // Dual-purpose: a slave-role device skips the master-only bring-up (BLE scan,
    // broadcaster) but still serves the SAME web app (sourced from received data).
    loadRole();
    Serial.printf("[boot] role: %s\n", gRole == ROLE_SLAVE ? "SLAVE" : "MASTER");
    if (gRole == ROLE_SLAVE) { setupSlave(); return; }

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
    WiFi.softAP(kApSsid, kApPass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/4);
    IPAddress ip = WiFi.softAPIP();
    { uint8_t pc = 0; wifi_second_chan_t sc; esp_wifi_get_channel(&pc, &sc);
      Serial.printf("AP '%s' up at http://%s/  (pass: %s) channel %u\n", kApSsid,
                    ip.toString().c_str(), kApPass, pc); }
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

    // mDNS: reachable as http://vicmon.local/ on a joined network.
    if (MDNS.begin("vicmon")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: http://vicmon.local/");
    }

    setupEspNow();  // live data broadcast to slaves
#endif

#ifdef VICMON_SIM
    simSetup();
#else
    NimBLEDevice::init("");
    gScan = NimBLEDevice::getScan();
    // Active scan so we also receive scan responses, which carry the device's
    // friendly name (Victron puts it there, not in the advertisement).
    gScan->setActiveScan(true);
    // BLE/WiFi radio split: window/interval = 50% duty. Higher BLE duty catches
    // adverts faster but leaves the WiFi AP less airtime (near-100% duty made the
    // AP unjoinable; 30% was very safe). 50:50 is a deliberate middle ground.
    gScan->setInterval(160);
    gScan->setWindow(80);
#endif

#ifdef VICMON_DISPLAY
    bringUpDisplay();  // panel + touch + display task (core 1)
#endif
}

void loop() {
    if (gRole == ROLE_SLAVE) { slaveLoop(); return; }
    serviceMasterSerial();  // `pair` / `role` console commands
    serviceRole();          // consume a serial/web role-toggle on headless masters

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
    sampleHistory();  // continuous logging, regardless of any connected client
    sampleStats();    // integrate energy counters / trip stats

    uint32_t now = millis();
    int worst = buildAlerts(now);
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
    static uint32_t lastHistSave = 0;
    if (gFsOk && now - lastHistSave >= kHistSaveMs) {
        lastHistSave = now;
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

    Serial.printf("[state] victron_adverts=%d decoded=%d |", gScanVictron, gScanDecoded);
    for (size_t i = 0; i < gConfig.count(); ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
    }
    Serial.println();
}
