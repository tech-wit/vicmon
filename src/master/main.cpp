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
#include "web_assets.h"  // kStyle / kMimicPage / kStatsPage / kDiagPage (HTML/CSS/JS)

// Board selection: the display DRIVER is chosen at build time per board model,
// while the master/slave ROLE is chosen at runtime (NVS flag). This one app runs
// on every board — BOARD_GUITION selects the AXS15231B QSPI driver; BOARD_LILYGO
// is reserved for the T-Display-S3 (ST7789 + buttons, driver TBD, builds headless
// for now); no board flag = headless (no screen, e.g. the bare S3 / AtomS3). A
// selected display board defines VICMON_DISPLAY, which guards all rendering below.
#if defined(BOARD_GUITION)
  #define VICMON_DISPLAY 1
  #include <GuitionDisplay.h>
  #include <GuitionTouch.h>
  #include <GfxDashboard.h>
#elif defined(BOARD_LILYGO)
  #warning "BOARD_LILYGO: display driver not implemented yet — building headless on the LilyGo"
#endif

// AP SSID is made unique per device at boot (Vicmon-<last 3 MAC bytes>) so
// several masters in the same area don't collide — filled in setup() once the
// master id is known; the default is only a placeholder before then.
static char kApSsid[24] = "Vicmon";
static const char* kApPass = "vicmon1234";  // >= 8 chars; change before the field
static const char* kFwVersion = "0.3.0";    // shown on the display Settings page

static DeviceConfig gConfig;
static sig::SignalMap gSignals;
static ProfileManager gProfiles;
static stats::Stats gStats;
static NimBLEScan* gScan = nullptr;
static AsyncWebServer gServer(80);
static DNSServer gDns;

#ifdef VICMON_DISPLAY
// The Guition JC3248W535 panel. The dashboard is drawn directly with Arduino_GFX
// primitives into the canvas each refresh (LVGL's flush folds on this panel).
// Display + touch run on their own task so the loop's blocking BLE scan can't
// stall touch; the loop (registry owner) publishes gDash under gDashMux.
static guition::Display  gDisplay;
static guition::Touch    gTouch;
static bool              gDisplayOk = false;
static guition::DashData gDash;
static SemaphoreHandle_t gDashMux = nullptr;
// Requests from the display task (Settings page) that must run on the loop task
// (registry / NVS owner). -1 / 0 = idle.
static volatile int gProfileReq = -1;    // profile id to switch to
static volatile int gSetAdjWhich = -1;   // guition::Tunable index being adjusted
static volatile int gSetAdjSteps = 0;    // accumulated signed steps to apply
static volatile int gBindSetRole = -1;   // request: bind this role...
static volatile int gBindSetIdx = -1;    // ...to this shared source index
static volatile bool gPairReq = false;   // request: open the master pairing window
static int gSetSel = 0;                  // display-local: selected tunable row
static int gSetView = 0;                 // display-local: Settings sub-view (0 tune, 1 bind)
static int gBindMenuRole = -1;           // display-local: open source-picker role (-1 = list)
static int gBindPage = 0;                // display-local: bindings-list page
static int gMenuPage = 0;                // display-local: source-picker page
static int gDiagScreen = 0;              // display-local: Diag sub-screen (guition::DiagScreen)

static_assert(guition::ROLE_N == static_cast<int>(sig::kRoleCount),
              "display ROLE_N must match sig::kRoleCount");
// Graph-page zoom window (minutes), set by the display task (graphHitTest owns
// the pill list), read by collectHistory on the loop task.
static volatile int gGraphWinMin = 60;
static volatile uint8_t gGraphHidden = 0;  // Graph legend: series toggled off (display-owned)
#endif

// Victron devices seen but not configured (no matching key).
struct Discovered {
    char mac[20] = {0};
    char name[24] = {0};  // BLE advertised (friendly) name, if any
    uint16_t model = 0;
    int rssi = 0;
    uint32_t lastSeenMs = 0;
    uint8_t raw[16] = {0};  // encrypted advert payload, captured in debug mode
    uint8_t rawLen = 0;     // 0 = not captured
};
static Discovered gDisc[12];
static size_t gDiscN = 0;

// ESP-NOW master identity + pairing window (used by the broadcaster below and by
// the diagnostics view). Declared here because ingest() and the debug-capture
// path reference gDebugCapture before the ESP-NOW section.
static uint32_t gMasterId = 0;              // stable per-chip id (low 32b of efuse MAC)
static volatile uint32_t gPairUntilMs = 0;  // pairing window closes at this millis (0 = closed)
static volatile bool gDebugCapture = false; // capture raw bytes of unknown adverts for troubleshooting
static bool gEspNowOk = false;              // ESP-NOW radio up (broadcaster in master, receiver in slave)
static volatile uint16_t gSnapSeq = 0;      // broadcast sequence counter (esp_timer task)

// Open a 60 s pairing window: while it's up the broadcast sets F_PAIRING so an
// adopting slave will accept this master's id (see lib/slavelink/SlaveLink.h).
static void startPairing() { gPairUntilMs = millis() + 60000; }
static bool pairingActive() {
    uint32_t u = gPairUntilMs;
    return u != 0 && (int32_t)(u - millis()) > 0;  // wrap-safe: window is only 60 s
}
static int pairSecsLeft() {
    return pairingActive() ? (int)((gPairUntilMs - millis()) / 1000) : 0;
}

// ---- device role (Master / Slave) ------------------------------------------
// The same firmware runs either role, chosen at boot from an NVS flag and
// toggled from the Diagnostics tab (reboots to re-lay-out the radio + tasks).
// Slave role skips BLE/AP/web and instead receives another master's broadcast
// via gRx and renders it on the same display.
enum { ROLE_MASTER = 0, ROLE_SLAVE = 1 };
static uint8_t gRole = ROLE_MASTER;
static volatile bool gRoleReq = false;   // display/loop request: toggle role + reboot
static slavelink::Receiver gRx;          // ESP-NOW receiver, used only in slave role

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
static void serviceRole() {
    if (gRoleReq) { gRoleReq = false; applyRoleToggle(); }
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
static void hexInto(char* out, size_t n, const uint8_t* p, size_t len) {
    static const char* hx = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < n; ++i) {
        out[o++] = hx[p[i] >> 4];
        out[o++] = hx[p[i] & 0xF];
    }
    out[o < n ? o : n - 1] = '\0';
}

// One-line live summary of a monitored device for the Diagnostics list.
static void summarizeDevice(const DeviceSlot& s, char* out, size_t n) {
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

static victron::Record parseType(const String& t) {
    if (t == "dcdc") return victron::Record::OrionXs;
    if (t == "solar") return victron::Record::SolarCharger;
    if (t == "charger") return victron::Record::AcCharger;
    return victron::Record::BatteryMonitor;
}
static const char* typeName(victron::Record r) {
    switch (r) {
        case victron::Record::OrionXs: return "dcdc";
        case victron::Record::BatteryMonitor: return "battery";
        case victron::Record::SolarCharger: return "solar";
        case victron::Record::AcCharger: return "charger";
        default: return "?";
    }
}

// ---- BLE ingestion ---------------------------------------------------------

// In debug-capture mode, stash up to 16 bytes of the (encrypted) advert payload
// so the diagnostics view / serial can show raw bytes for an unknown device.
static void captureRaw(Discovered& d, const uint8_t* raw, size_t rawLen) {
    if (!gDebugCapture || !raw) return;
    uint8_t n = rawLen > sizeof(d.raw) ? sizeof(d.raw) : (uint8_t)rawLen;
    memcpy(d.raw, raw, n);
    d.rawLen = n;
}

static void noteDiscovered(const char* mac, const char* name, uint16_t model, int rssi,
                           const uint8_t* raw, size_t rawLen) {
    uint32_t now = millis();
    for (size_t i = 0; i < gDiscN; ++i) {
        if (strncmp(gDisc[i].mac, mac, sizeof(gDisc[i].mac)) == 0) {
            gDisc[i].rssi = rssi;
            gDisc[i].model = model;
            gDisc[i].lastSeenMs = now;
            if (name && name[0]) strncpy(gDisc[i].name, name, sizeof(gDisc[i].name) - 1);
            captureRaw(gDisc[i], raw, rawLen);
            return;
        }
    }
    if (gDiscN < (sizeof(gDisc) / sizeof(gDisc[0]))) {
        Discovered& d = gDisc[gDiscN++];
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        if (name) strncpy(d.name, name, sizeof(d.name) - 1);
        d.model = model;
        d.rssi = rssi;
        d.lastSeenMs = now;
        captureRaw(d, raw, rawLen);
    }
}

static int gScanVictron = 0, gScanDecoded = 0;  // per-scan diagnostics

static void ingest(NimBLEAdvertisedDevice* dev) {
    if (!dev->haveManufacturerData()) return;
    std::string md = dev->getManufacturerData();
    if (md.size() < 2 + 9) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(md.data());
    if (!(p[0] == 0xE1 && p[1] == 0x02)) return;  // Victron company id
    ++gScanVictron;

    const uint8_t* extra = p + 2;
    size_t extraLen = md.size() - 2;

    uint8_t out[32];
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        int n = victron::decrypt(extra, extraLen, s.key, out, sizeof(out));
        if (n < 0) continue;
        switch (s.type) {
            case victron::Record::BatteryMonitor:
                if (!victron::parseBatteryMonitor(out, n, s.battery)) return;
                break;
            case victron::Record::OrionXs:
                if (!victron::parseOrionXs(out, n, s.dcdc)) return;
                break;
            case victron::Record::SolarCharger:
                if (!victron::parseSolarCharger(out, n, s.solar)) return;
                break;
            case victron::Record::AcCharger:
                if (!victron::parseAcCharger(out, n, s.charger)) return;
                break;
            default:
                break;
        }
        s.everSeen = true;
        s.lastSeenMs = millis();
        ++gScanDecoded;
        s.rawLen = n > (int)sizeof(s.raw) ? sizeof(s.raw) : n;  // keep for /diag
        memcpy(s.raw, out, s.rawLen);
        s.modelId = victron::modelId(extra);
        strncpy(s.mac, dev->getAddress().toString().c_str(), sizeof(s.mac) - 1);
        if (dev->getName().size())
            strncpy(s.btname, dev->getName().c_str(), sizeof(s.btname) - 1);
        return;
    }
    // No configured key matched -> a device we could adopt (and, in debug mode,
    // capture the raw advert bytes of for troubleshooting).
    noteDiscovered(dev->getAddress().toString().c_str(), dev->getName().c_str(),
                   victron::modelId(extra), dev->getRSSI(), extra, extraLen);
}

static void pollBle() {
    gScanVictron = 0;
    gScanDecoded = 0;
    NimBLEScanResults results = gScan->start(2 /*seconds*/, false);
    for (int i = 0; i < results.getCount(); ++i) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        ingest(&d);
    }
    gScan->clearResults();
}

// ---- signal resolution -----------------------------------------------------

static sig::Resolved R(sig::Role role, uint32_t now) {
    const sig::Binding& b = gSignals.binding(role);
    return sig::resolveField(gConfig.slots(), gConfig.count(), b.device, b.field, now);
}

static String jbool(bool b) { return b ? "true" : "false"; }

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
static sig::Resolved resolveSignal(sig::Role role, uint32_t now) {
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
struct HistSample {
    int16_t battery, solar, charger, dcdc, load;  // deci-amps, -32768 = n/a
    int16_t soc;                                   // deci-percent (0..1000), -32768 = n/a
};

// One time-series ring buffer. Two instances are fed from a single read each
// loop: a fine (5 s / 1 h) and a coarse (60 s / 24 h) buffer.
struct HistRing {
    HistSample* buf = nullptr;
    size_t cap = 0, head = 0, count = 0;
    uint32_t intervalMs = 0, lastMs = 0;
    void init(HistSample* b, size_t c, uint32_t iv) { buf = b; cap = c; intervalMs = iv; }
    void clear() { head = count = 0; lastMs = 0; }
    bool due(uint32_t now) const { return count == 0 || now - lastMs >= intervalMs; }
    void push(const HistSample& s, uint32_t now) {
        lastMs = now;
        buf[head] = s;
        head = (head + 1) % cap;
        if (count < cap) ++count;
    }
};
static HistSample gFineBuf[HIST_CAP];
static HistSample gCoarseBuf[HIST2_CAP];
static HistRing gFine, gCoarse;

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

static bool gFsOk = false;
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

static void saveHistFile(int profile) {
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

static uint32_t currentLocalEpoch();  // fwd decl (defined with the settings)

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

static String buildHistoryJson(int mins) {
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

static float gBattCapacity = 0;     // Ah, 0 = unknown
static float gDeadband = 0.2f;      // A; |current| below this reads as idle
static int gTzOffsetMin = 600;      // local time offset from UTC, minutes (+10h AEST)
// Alert thresholds (0 disables that check).
static float gSocWarn = 50;         // % — warn at/below
static float gSocCrit = 30;         // % — critical at/below
static float gVlow = 11.8f;         // V — critical at/below
static float gVhigh = 15.0f;        // V — critical at/above
static String settingsNs(int profile) {
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
static void saveSettings(float capacity, float deadband, int tzMin) {
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
static void saveAlertSettings(float socWarn, float socCrit, float vLow, float vHigh) {
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
static uint32_t currentLocalEpoch() {
    time_t t = time(nullptr);
    if (t < 1700000000) return 0;  // ~2023-11; NTP not synced yet
    return static_cast<uint32_t>(t) + gTzOffsetMin * 60;
}

// Loads a profile's config/signals/settings and clears runtime caches so the
// mimic, history and discovery don't mix data across profiles.
static void applyProfile(int pid) {
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
static void wipeProfile(int pid) {
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

static String gStaSsid, gStaPass;
static void loadWifi() {
    Preferences p;
    p.begin("vicwifi", true);
    gStaSsid = p.getString("ssid", "");
    gStaPass = p.getString("pass", "");
    p.end();
}
static void saveWifiCreds(const String& s, const String& pw) {
    Preferences p;
    p.begin("vicwifi", false);
    p.putString("ssid", s);
    p.putString("pass", pw);
    p.end();
}

// Charge mode from the (possibly invalid) battery current, honouring the idle
// deadband. One definition shared by the panel/data JSON, the LED, and the
// ESP-NOW snapshot so the threshold can't drift between them.
enum class ChargeMode { Unknown, Charging, Discharging, Idle };
static ChargeMode chargeMode(const sig::Resolved& ba) {
    if (!ba.valid) return ChargeMode::Unknown;
    if (ba.value > gDeadband) return ChargeMode::Charging;
    if (ba.value < -gDeadband) return ChargeMode::Discharging;
    return ChargeMode::Idle;
}
static const char* chargeModeName(ChargeMode m) {
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
static const char* chargeModeDisplayName(ChargeMode m) {
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
static String jsonEsc(const String& s);  // defined with the web helpers below

static int buildAlerts(uint32_t now, String* outArr = nullptr) {
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

// One canonical snapshot of the live panel, resolved from the registry ONCE per
// consumer. The three serializers — the ESP-NOW Snapshot (buildSnapshot), the web
// panel JSON (buildPanelJson) and the LCD DashData (collectDash) — all read from
// this, so which signal/derived-sentinel/secondary-field a value comes from lives
// in exactly one place and can't drift between them.
struct PanelModel {
    sig::Resolved soc, battV, battA, consumed, starterV, ttg;
    sig::Resolved solarA, solarW, solarV;
    sig::Resolved chargerA;
    sig::Resolved dcdcInA, dcdcOutA, dcdcInV, dcdcOutV;
    sig::Resolved loadA;
    bool loadDerived = false;
    ChargeMode mode = ChargeMode::Unknown;
    int alertWorst = 0;
    float capacity = 0;  // battery Ah (0 = unknown)
};

static PanelModel collectPanel(uint32_t now) {
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

static String buildPanelJson() {
    // Slave role: the registry is empty (no BLE) — build the panel from the last
    // ESP-NOW frame so the mimic/dashboard show the master's live data.
    if (gRole == ROLE_SLAVE) {
        using namespace slavelink;
        const Snapshot& s = gRx.snapshot();
        bool live = gRx.live();
        auto has = [&](uint16_t f) { return live && (s.valid & f) != 0; };
        const char* mode = !live ? "unknown"
            : (s.mode == M_CHARGING ? "charging"
             : s.mode == M_DISCHARGING ? "discharging"
             : s.mode == M_IDLE ? "idle" : "unknown");
        String j = "{";
        j += "\"mode\":\"" + String(mode) + "\",";
        j += "\"battery\":{\"valid\":" + jbool(has(V_SOC) || has(V_BATTV) || has(V_BATTA)) +
             ",\"soc\":" + String(decDeci(s.soc_d), 1) +
             ",\"v\":" + String(decCenti(s.battV_cv), 2) +
             ",\"a\":" + String(decDeci(s.battA_da), 2) +
             ",\"consumed\":" + String(decDeci(s.consumedAh_da), 1) +
             ",\"consumed_valid\":" + jbool(has(V_CONSUMED)) +
             ",\"starter_v\":" + String(decCenti(s.starterV_cv), 2) +
             ",\"starter_valid\":" + jbool(has(V_STARTERV)) +
             ",\"ttg\":" + String(s.ttg_min == 0xFFFF ? 0 : s.ttg_min) +
             ",\"ttg_valid\":" + jbool(has(V_TTG)) + ",\"capacity\":0},";
        j += "\"solar\":{\"valid\":" + jbool(has(V_SOLAR)) + ",\"a\":" + String(decDeci(s.solarA_da), 1) +
             ",\"w\":" + String(decWhole(s.solarW_w), 0) +
             ",\"v\":" + String(decCenti(s.solarV_cv), 2) + ",\"v_valid\":" + jbool(has(V_SOLARV)) + "},";
        j += "\"charger\":{\"valid\":" + jbool(has(V_CHARGER)) + ",\"a\":" + String(decDeci(s.chargerA_da), 1) + "},";
        j += "\"dcdc\":{\"valid\":" + jbool(has(V_DCDC)) + ",\"out_a\":" + String(decDeci(s.dcdcA_da), 1) +
             ",\"in_a\":0,\"in_v\":" + String(decCenti(s.dcdcInV_cv), 2) +
             ",\"in_v_valid\":" + jbool(has(V_DCDCINV)) +
             ",\"out_v\":" + String(decCenti(s.dcdcOutV_cv), 2) +
             ",\"out_v_valid\":" + jbool(has(V_DCDCOUTV)) + "},";
        j += "\"load\":{\"valid\":" + jbool(has(V_LOAD)) + ",\"a\":" + String(decDeci(s.loadA_da), 1) +
             ",\"derived\":false},";
        j += "\"alerts\":[]}";
        return j;
    }
    uint32_t now = millis();
    PanelModel p = collectPanel(now);
    bool battValid = p.soc.valid || p.battV.valid || p.battA.valid;
    bool dcdcValid = p.dcdcOutA.valid || p.dcdcInA.valid;

    String j = "{";
    j += "\"mode\":\"" + String(chargeModeName(p.mode)) + "\",";
    j += "\"battery\":{\"valid\":" + jbool(battValid) +
         ",\"soc\":" + String(p.soc.value, 1) +
         ",\"v\":" + String(p.battV.value, 2) +
         ",\"a\":" + String(p.battA.value, 2) +
         ",\"consumed\":" + String(p.consumed.value, 1) + ",\"consumed_valid\":" + jbool(p.consumed.valid) +
         ",\"starter_v\":" + String(p.starterV.value, 2) + ",\"starter_valid\":" + jbool(p.starterV.valid) +
         ",\"ttg\":" + String(p.ttg.value, 0) + ",\"ttg_valid\":" + jbool(p.ttg.valid) +
         ",\"capacity\":" + String(p.capacity, 0) + "},";
    // NB: the SmartSolar advert has no PV-array voltage — only the charger's
    // battery-side voltage — so "solar V" is the battery-side reading.
    j += "\"solar\":{\"valid\":" + jbool(p.solarA.valid) +
         ",\"a\":" + String(p.solarA.value, 1) +
         ",\"w\":" + String(p.solarW.value, 0) +
         ",\"v\":" + String(p.solarV.value, 2) + ",\"v_valid\":" + jbool(p.solarV.valid) + "},";
    j += "\"charger\":{\"valid\":" + jbool(p.chargerA.valid) +
         ",\"a\":" + String(p.chargerA.value, 1) + "},";
    j += "\"dcdc\":{\"valid\":" + jbool(dcdcValid) +
         ",\"out_a\":" + String(p.dcdcOutA.value, 1) +
         ",\"in_a\":" + String(p.dcdcInA.value, 1) +
         ",\"in_v\":" + String(p.dcdcInV.value, 2) + ",\"in_v_valid\":" + jbool(p.dcdcInV.valid) +
         ",\"out_v\":" + String(p.dcdcOutV.value, 2) + ",\"out_v_valid\":" + jbool(p.dcdcOutV.valid) + "},";
    j += "\"load\":{\"valid\":" + jbool(p.loadA.valid) +
         ",\"a\":" + String(p.loadA.value, 1) +
         ",\"derived\":" + jbool(p.loadDerived) + "},";
    String alerts;
    buildAlerts(now, &alerts);
    j += "\"alerts\":" + alerts;
    j += "}";
    return j;
}

#ifdef VICMON_DISPLAY
// ---- Settings > Bindings (display) -----------------------------------------
// Compact field tag for the Bindings list, e.g. "SoC", "in A". Keeps the whole
// "<device> <tag>" label inside the row width.
static const char* shortField(sig::Field f) {
    switch (f) {
        case sig::Field::BattSOC:         return "SoC";
        case sig::Field::BattV:           return "V";
        case sig::Field::BattA:           return "A";
        case sig::Field::BattConsumed:    return "Ah";
        case sig::Field::BattAuxStarterV: return "start V";
        case sig::Field::BattTTG:         return "TTG";
        case sig::Field::DcDcInV:         return "in V";
        case sig::Field::DcDcOutV:        return "out V";
        case sig::Field::DcDcInA:         return "in A";
        case sig::Field::DcDcOutA:        return "out A";
        case sig::Field::DcDcState:       return "state";
        case sig::Field::SolarBattV:      return "bat V";
        case sig::Field::SolarBattA:      return "bat A";
        case sig::Field::SolarPvW:        return "PV W";
        case sig::Field::SolarYield:      return "yield";
        case sig::Field::SolarLoadA:      return "load A";
        case sig::Field::SolarState:      return "state";
        case sig::Field::ChgBattV:        return "bat V";
        case sig::Field::ChgBattA:        return "bat A";
        case sig::Field::ChgState:        return "state";
        default:                          return "";
    }
}

// Human label for a binding source (device+field, a derived sentinel, or none).
static void formatSource(const char* device, sig::Field f, char* buf, size_t n) {
    if (device[0] == '\0') { snprintf(buf, n, "-- none --"); return; }
    if (strcmp(device, sig::kChargeOnly) == 0) { snprintf(buf, n, "derived: charge"); return; }
    if (strcmp(device, sig::kLoadOnly) == 0 ||
        strcmp(device, sig::kDerived) == 0) { snprintf(buf, n, "derived: load"); return; }
    snprintf(buf, n, "%s %s", device, shortField(f));
}

// One selectable binding source.
struct BindOpt { const char* device; sig::Field field; };

// Build the SHARED source list the whole Bindings UI selects from: index 0 =
// none, 1 = derived charge, 2 = derived load, then every configured device x its
// offered fields. Same for all roles; the display hides the two derived entries
// for non-current-flow roles. Published every snapshot so the picker has no lag.
static int buildSrcList(BindOpt* out, int max) {
    int n = 0;
    auto add = [&](const char* dev, sig::Field f) { if (n < max) out[n++] = {dev, f}; };
    add("", sig::Field::None);
    add(sig::kChargeOnly, sig::Field::None);
    add(sig::kLoadOnly, sig::Field::None);
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        sig::Field fields[8];
        size_t nf = sig::fieldsForType(s.type, fields, 8);
        for (size_t k = 0; k < nf; ++k) add(s.name, fields[k]);
    }
    return n;
}

// Shared-list index a role is currently bound to (0 = none if not found).
static int sharedIndexOf(const BindOpt* list, int n, const sig::Binding& b) {
    const char* cd = b.device;
    if (strcmp(cd, sig::kDerived) == 0) cd = sig::kLoadOnly;  // legacy alias
    for (int i = 0; i < n; ++i)
        if (strcmp(list[i].device, cd) == 0 && list[i].field == b.field) return i;
    return 0;
}

// Bind a role to a shared-list source index and persist. Runs on the loop task.
static void applyBindSet(int roleIdx, int sharedIdx) {
    if (roleIdx < 0 || roleIdx >= (int)sig::kRoleCount) return;
    BindOpt list[guition::BIND_MAXSRC];
    int n = buildSrcList(list, guition::BIND_MAXSRC);
    if (sharedIdx < 0 || sharedIdx >= n) return;
    gSignals.set(static_cast<sig::Role>(roleIdx), list[sharedIdx].device, list[sharedIdx].field);
    gSignals.save();
}

// Fill a DashData from the resolved signals — the struct mirror of
// buildPanelJson(). Runs on the loop task (registry owner).
// Fill the Graph-page series for the selected zoom window, mirroring the web
// chart's buildHistoryJson(): windows over 60 min read the coarse (60 s) ring,
// shorter ones the fine (5 s) ring; take the most-recent time-based slice, then
// downsample to the plot columns. Runs on the loop task (registry owner).
static void collectHistory(guition::DashData& d) {
    int mins = gGraphWinMin;
    d.histWinMin = (uint16_t)mins;
    const HistRing& r = mins > 60 ? gCoarse : gFine;
    int want = mins * 60 * 1000 / (int)r.intervalMs;
    if (want > (int)r.count) want = r.count;
    if (want < 0) want = 0;
    if (want < 2) { d.histCount = 0; return; }
    int out = want < guition::HIST_POINTS ? want : guition::HIST_POINTS;
    size_t start = (r.head + r.cap - (size_t)want) % r.cap;
    for (int k = 0; k < out; ++k) {
        int src = (out == 1) ? (want - 1) : (int)((long)k * (want - 1) / (out - 1));
        size_t idx = (start + (size_t)src) % r.cap;
        const HistSample& s = r.buf[idx];
        d.histSoc[k]     = s.soc;
        d.histBatt[k]    = s.battery;
        d.histSolar[k]   = s.solar;
        d.histCharger[k] = s.charger;
        d.histDcdc[k]    = s.dcdc;
        d.histLoad[k]    = s.load;
    }
    d.histCount = out;
}

static void collectDash(guition::DashData& d) {
    uint32_t now = millis();
    PanelModel p = collectPanel(now);

    d.mode  = chargeModeDisplayName(p.mode);
    d.worst = p.alertWorst;

    d.battValid = p.soc.valid || p.battV.valid || p.battA.valid;
    d.soc = p.soc.value; d.v = p.battV.value; d.a = p.battA.value;
    d.ttgValid = p.ttg.valid; d.ttg = p.ttg.value;
    d.starterValid = p.starterV.valid; d.starterV = p.starterV.value;

    d.solarValid = p.solarA.valid; d.solarW = p.solarW.value; d.solarA = p.solarA.value;
    d.chargerValid = p.chargerA.valid; d.chargerA = p.chargerA.value;
    d.dcdcValid = p.dcdcOutA.valid || p.dcdcInA.valid; d.dcdcOutA = p.dcdcOutA.value;
    d.dcdcInVValid = p.dcdcInV.valid; d.dcdcInV = p.dcdcInV.value;
    d.loadValid = p.loadA.valid; d.loadA = p.loadA.value;
    d.loadDerived = p.loadDerived;

    // Graph page.
    collectHistory(d);

    // Week page: last-7-days energy + today's running totals.
    d.clockOk = (currentLocalEpoch() != 0);
    int dc = (int)gStats.dayCount();
    int start = dc > guition::DashData::DAYS_N ? dc - guition::DashData::DAYS_N : 0;
    int out = 0;
    for (int i = start; i < dc; ++i) {
        const stats::DayRecord& r = gStats.day(i);
        d.dayStamp[out]     = r.dayStamp;
        d.daySolarWh[out]   = r.solarWh;
        d.dayDcdcWh[out]    = r.dcdcWh;
        d.dayChargerWh[out] = r.chargerWh;
        d.dayLoadWh[out]    = r.loadWh;
        ++out;
    }
    d.dayCount = out;
    const stats::Bucket& tb = gStats.bucket(stats::TODAY);
    d.todaySolarWh = tb.solarWh; d.todayDcdcWh = tb.dcdcWh;
    d.todayChargerWh = tb.chargerWh; d.todayLoadWh = tb.loadWh;

    // Settings page: profiles, status, tunables (brightness + selected row are
    // filled by the display task).
    d.profileId = gProfiles.active();
    d.profileCount = gProfiles.usedCount();
    for (int i = 0; i < ProfileManager::kMax && i < 4; ++i) {
        d.profUsed[i] = gProfiles.used(i);
        strncpy(d.profNames[i], gProfiles.name(i), sizeof(d.profNames[i]) - 1);
        d.profNames[i][sizeof(d.profNames[i]) - 1] = '\0';
    }
    strncpy(d.apSsid, kApSsid, sizeof(d.apSsid) - 1);
    d.apSsid[sizeof(d.apSsid) - 1] = '\0';
    strncpy(d.apPass, kApPass, sizeof(d.apPass) - 1);
    d.apPass[sizeof(d.apPass) - 1] = '\0';
    WiFi.softAPIP().toString().toCharArray(d.ipStr, sizeof(d.ipStr));
    d.devPaired = (int)gConfig.count();
    d.devSeen = (int)gDiscN;
    d.uptimeSec = now / 1000;
    d.freeHeapKb = ESP.getFreeHeap() / 1024;
    strncpy(d.version, kFwVersion, sizeof(d.version) - 1);
    d.version[sizeof(d.version) - 1] = '\0';
    d.battCapAh = gBattCapacity;
    d.deadbandA = gDeadband;
    d.tzMin = gTzOffsetMin;
    d.socWarn = gSocWarn; d.socCrit = gSocCrit;
    d.vLow = gVlow; d.vHigh = gVhigh;

    // Settings > Bindings: publish the shared source list + each role's current
    // selection so the display can render the picker with no snapshot lag.
    BindOpt list[guition::BIND_MAXSRC];
    int nsrc = buildSrcList(list, guition::BIND_MAXSRC);
    d.srcCount = nsrc;
    for (int i = 0; i < nsrc; ++i)
        formatSource(list[i].device, list[i].field, d.srcLabels[i], sizeof(d.srcLabels[i]));
    for (size_t r = 0; r < sig::kRoleCount; ++r) {
        int idx = sharedIndexOf(list, nsrc, gSignals.binding(static_cast<sig::Role>(r)));
        d.bindIdx[r] = idx;
        strncpy(d.bindLabel[r], d.srcLabels[idx], sizeof(d.bindLabel[r]) - 1);
        d.bindLabel[r][sizeof(d.bindLabel[r]) - 1] = '\0';
    }

    // Settings > Diagnostics (master role): ESP-NOW/pairing state + device lists.
    d.role = ROLE_MASTER;
    d.espNowOk = gEspNowOk;
    d.masterId = gMasterId;
    d.snapSeq = gSnapSeq;
    d.pairing = pairingActive();
    d.pairSecLeft = pairSecsLeft();
    d.debugCapture = gDebugCapture;
    int mn = 0;
    for (size_t i = 0; i < gConfig.count() && mn < guition::DashData::MON_N; ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        strncpy(d.monName[mn], s.name[0] ? s.name : "(device)", sizeof(d.monName[mn]) - 1);
        d.monName[mn][sizeof(d.monName[mn]) - 1] = '\0';
        strncpy(d.monType[mn], typeName(s.type), sizeof(d.monType[mn]) - 1);
        d.monType[mn][sizeof(d.monType[mn]) - 1] = '\0';
        d.monLive[mn] = !s.stale(now);
        summarizeDevice(s, d.monVal[mn], sizeof(d.monVal[mn]));
        ++mn;
    }
    d.monCount = mn;
    int dn = 0;
    for (size_t i = 0; i < gDiscN && dn < guition::DashData::DISC_N; ++i) {
        if (now - gDisc[i].lastSeenMs > 30000) continue;  // only recently seen
        strncpy(d.discName[dn], gDisc[i].name, sizeof(d.discName[dn]) - 1);
        d.discName[dn][sizeof(d.discName[dn]) - 1] = '\0';
        strncpy(d.discMac[dn], gDisc[i].mac, sizeof(d.discMac[dn]) - 1);
        d.discMac[dn][sizeof(d.discMac[dn]) - 1] = '\0';
        d.discModel[dn] = gDisc[i].model;
        d.discRssi[dn] = gDisc[i].rssi;
        if (gDisc[i].rawLen) hexInto(d.discRaw[dn], sizeof(d.discRaw[dn]), gDisc[i].raw, gDisc[i].rawLen);
        else d.discRaw[dn][0] = '\0';
        ++dn;
    }
    d.discCount = dn;
}

// Redraw the dashboard from the latest signals a few times/sec. Called from
// loop(). Arduino_GFX direct draw into the canvas + push.
static guition::Page gPage = guition::PAGE_DASH;

// Publish the latest resolved signals for the display task. Runs on the loop
// task (registry owner) so registry access stays single-threaded.
static void publishDash() {
    if (!gDashMux) return;
    guition::DashData tmp;
    collectDash(tmp);
    if (xSemaphoreTake(gDashMux, 0) == pdTRUE) {
        gDash = tmp;
        xSemaphoreGive(gDashMux);
    }
}

// Slave role: fill the DashData from the received ESP-NOW frame (a subset of the
// master's data — enough for the Dash + Flow pages) plus the link/pairing status
// for the Diagnostics view. Graph/Week/Tune/Bind have no data and are gated off
// in the renderer. Runs on the loop task.
static void collectSlaveDash(guition::DashData& d) {
    using namespace slavelink;
    d.role = ROLE_SLAVE;
    d.espNowOk = gRx.ok();
    d.masterId = gRx.pairedMaster();
    d.pairing = gRx.isAdopting();
    d.pairSecLeft = (int)gRx.adoptSecsLeft();
    d.linkLive = gRx.live();
    d.linkDrops = gRx.drops();
    d.linkChannel = gRx.channel();
    d.heardInvite = gRx.heardInvite();

    if (!gRx.live()) { d.mode = "--"; d.battValid = false; return; }
    const Snapshot& s = gRx.snapshot();
    auto has = [&](uint16_t f) { return (s.valid & f) != 0; };
    switch (s.mode) {
        case M_CHARGING: d.mode = "Charging"; break;
        case M_DISCHARGING: d.mode = "Discharging"; break;
        case M_IDLE: d.mode = "Idle"; break;
        default: d.mode = "--"; break;
    }
    d.worst = s.alertWorst;
    d.battValid = has(V_SOC) || has(V_BATTV) || has(V_BATTA);
    d.soc = decDeci(s.soc_d);
    d.v = decCenti(s.battV_cv);
    d.a = decDeci(s.battA_da);
    d.ttgValid = has(V_TTG); d.ttg = (s.ttg_min == 0xFFFF) ? 0 : s.ttg_min;
    d.starterValid = has(V_STARTERV); d.starterV = decCenti(s.starterV_cv);
    d.solarValid = has(V_SOLAR); d.solarA = decDeci(s.solarA_da);
    d.solarW = decWhole(s.solarW_w);  // v3
    d.chargerValid = has(V_CHARGER); d.chargerA = decDeci(s.chargerA_da);
    d.dcdcValid = has(V_DCDC); d.dcdcOutA = decDeci(s.dcdcA_da);
    d.dcdcInVValid = has(V_DCDCINV); d.dcdcInV = decCenti(s.dcdcInV_cv);  // v3
    d.loadValid = has(V_LOAD); d.loadA = decDeci(s.loadA_da); d.loadDerived = false;
    d.battCapAh = s.capacityAh;  // v4: for the Dash/Flow remaining-Ah readout
    d.profileId = s.profile;
    collectHistory(d);  // Graph page: fill from the history built off received frames

    // Week page: from the low-rate stats frame (if we've received one).
    if (gRx.hasStats()) {
        const slavelink::StatsFrame& f = gRx.stats();
        d.clockOk = f.clockOk != 0;
        int n = f.dayCount > guition::DashData::DAYS_N ? guition::DashData::DAYS_N : f.dayCount;
        for (int i = 0; i < n; ++i) {
            d.dayStamp[i] = f.dayStamp[i];
            d.daySolarWh[i] = f.daySolarWh[i];
            d.dayDcdcWh[i] = f.dayDcdcWh[i];
            d.dayChargerWh[i] = f.dayChargerWh[i];
            d.dayLoadWh[i] = f.dayLoadWh[i];
        }
        d.dayCount = n;
        d.todaySolarWh = f.todaySolarWh; d.todayDcdcWh = f.todayDcdcWh;
        d.todayChargerWh = f.todayChargerWh; d.todayLoadWh = f.todayLoadWh;
    } else {
        d.dayCount = 0; d.clockOk = false;
    }

    // Settings (Tune) fields relevant to a slave: its own config AP + display prefs.
    strncpy(d.apSsid, kApSsid, sizeof(d.apSsid) - 1); d.apSsid[sizeof(d.apSsid) - 1] = '\0';
    strncpy(d.apPass, kApPass, sizeof(d.apPass) - 1); d.apPass[sizeof(d.apPass) - 1] = '\0';
    WiFi.softAPIP().toString().toCharArray(d.ipStr, sizeof(d.ipStr));
    d.uptimeSec = millis() / 1000;
    d.freeHeapKb = ESP.getFreeHeap() / 1024;
    d.tzMin = gTzOffsetMin;
    strncpy(d.version, kFwVersion, sizeof(d.version) - 1); d.version[sizeof(d.version) - 1] = '\0';
}

static void publishSlaveDash() {
    if (!gDashMux) return;
    guition::DashData tmp;
    collectSlaveDash(tmp);
    if (xSemaphoreTake(gDashMux, 0) == pdTRUE) {
        gDash = tmp;
        xSemaphoreGive(gDashMux);
    }
}

// Apply a Settings-page tunable adjustment (loop task: writes globals + NVS).
// `which` is a guition::Tunable; brightness (index 0) is handled by the display
// task and never reaches here. Each save*() call persists the whole group.
static void applyTunableAdjust(int which, int steps) {
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    switch (which) {
        case guition::TUN_BATTCAP:
            saveSettings(clampf(gBattCapacity + steps * 5.0f, 0, 2000), gDeadband, gTzOffsetMin);
            break;
        case guition::TUN_DEADBAND:
            saveSettings(gBattCapacity, clampf(gDeadband + steps * 0.05f, 0, 2), gTzOffsetMin);
            break;
        case guition::TUN_TZ: {
            int tz = gTzOffsetMin + steps * 30;
            if (tz < -720) tz = -720;
            if (tz > 840) tz = 840;
            saveSettings(gBattCapacity, gDeadband, tz);
            break;
        }
        case guition::TUN_SOCWARN:
            saveAlertSettings(clampf(gSocWarn + steps * 5, 0, 100), gSocCrit, gVlow, gVhigh);
            break;
        case guition::TUN_SOCCRIT:
            saveAlertSettings(gSocWarn, clampf(gSocCrit + steps * 5, 0, 100), gVlow, gVhigh);
            break;
        case guition::TUN_VLOW:
            saveAlertSettings(gSocWarn, gSocCrit, clampf(gVlow + steps * 0.1f, 5, 20), gVhigh);
            break;
        case guition::TUN_VHIGH:
            saveAlertSettings(gSocWarn, gSocCrit, gVlow, clampf(gVhigh + steps * 0.1f, 5, 20));
            break;
        default: break;
    }
}

// Handle deferred requests from the display task (Settings page) — profile
// switch and tunable adjust — on the loop task, which owns the registry / NVS.
static void serviceDashRequests() {
    if (gProfileReq >= 0) {
        int target = gProfileReq;
        gProfileReq = -1;
        if (target < ProfileManager::kMax && gProfiles.used(target) &&
            target != gProfiles.active()) {
            saveHistFile(gProfiles.active());   // flush the outgoing profile's history
            gProfiles.setActive(target);
            applyProfile(target);
            Serial.printf("[display] switched to profile '%s'\n", gProfiles.name(target));
        }
    }
    if (gSetAdjWhich >= 0 && gSetAdjSteps != 0) {
        int which = gSetAdjWhich, steps = gSetAdjSteps;
        gSetAdjSteps = 0;
        applyTunableAdjust(which, steps);
    }
    if (gBindSetRole >= 0) {
        int role = gBindSetRole, idx = gBindSetIdx;
        gBindSetRole = -1;
        applyBindSet(role, idx);
    }
    if (gPairReq) {
        gPairReq = false;
        startPairing();
        Serial.printf("[display] pairing window open %ds\n", pairSecsLeft());
    }
    serviceRole();  // role toggle (reboots) — no-op unless the Diag tab requested it
}

// Display + touch task: polls touch at ~30ms (responsive tab switching) and
// redraws the current page a couple of times/sec or on page change, from the
// mutex-protected snapshot. Never touches the registry, so it's independent of
// the loop's blocking BLE scan.
// Optimistic tunable display: the loop applies the real change + NVS on its ~2 s
// BLE-blocked cadence, so without this the on-screen number lags ~1-2 s. These
// step/clamp tables MIRROR applyTunableAdjust() (keep them in sync) so the display
// can show the new value the instant you tap. Index = guition::Tunable.
static const float kTunStep[guition::TUNABLE_N] = {10, 5, 0.05f, 30, 5, 5, 0.1f, 0.1f};
static const float kTunMin[guition::TUNABLE_N]  = {10, 0, 0, -720, 0, 0, 5, 5};
static const float kTunMax[guition::TUNABLE_N]  = {100, 2000, 2, 840, 100, 100, 20, 20};

static float tunVal(const guition::DashData& d, int i) {
    switch (i) {
        case guition::TUN_BRIGHT:   return d.brightness;
        case guition::TUN_BATTCAP:  return d.battCapAh;
        case guition::TUN_DEADBAND: return d.deadbandA;
        case guition::TUN_TZ:       return d.tzMin;
        case guition::TUN_SOCWARN:  return d.socWarn;
        case guition::TUN_SOCCRIT:  return d.socCrit;
        case guition::TUN_VLOW:     return d.vLow;
        case guition::TUN_VHIGH:    return d.vHigh;
    }
    return 0;
}
static void setTunVal(guition::DashData& d, int i, float v) {
    switch (i) {
        case guition::TUN_BATTCAP:  d.battCapAh = v; break;
        case guition::TUN_DEADBAND: d.deadbandA = v; break;
        case guition::TUN_TZ:       d.tzMin = (int)v; break;
        case guition::TUN_SOCWARN:  d.socWarn = v; break;
        case guition::TUN_SOCCRIT:  d.socCrit = v; break;
        case guition::TUN_VLOW:     d.vLow = v; break;
        case guition::TUN_VHIGH:    d.vHigh = v; break;
        default: break;
    }
}

static void displayTask(void*) {
    bool wasDown = false;
    uint32_t lastUi = 0;
    int lastSrcCount = 0;  // shared-source count from the last render (for menu hit mapping)
    int optWhich = -1;          // tunable being optimistically shown (-1 = none)
    float optVal = 0;
    float tunShown[guition::TUNABLE_N] = {0};  // last displayed tunable values (adjust base)
    uint32_t lastTapMs = 0;     // debounce: ignore down-edges too close together
    for (;;) {
        bool redraw = false;
        guition::TouchPoint tp;
        bool down = gTouch.read(tp);
        if (down && !wasDown && millis() - lastTapMs >= 150) {  // debounce: one action per tap
            lastTapMs = millis();
            int t = guition::tabHitTest(tp.x, tp.y);
            if (t >= 0) {
                if ((guition::Page)t != gPage) { gPage = (guition::Page)t; redraw = true; }
                gBindMenuRole = -1;  // leaving the page closes any open picker
                gDiagScreen = 0;     // and returns Diag to its menu
            } else if (gPage == guition::PAGE_GRAPH) {
                // Tap a zoom pill to jump straight to that window (1m..24h).
                int win = guition::graphHitTest(tp.x, tp.y);
                if (win > 0) {
                    if (win != gGraphWinMin) { gGraphWinMin = win; redraw = true; }
                } else {
                    int s = guition::graphLegendHit(tp.x, tp.y);  // tap legend to toggle a series
                    if (s >= 0) { gGraphHidden ^= (uint8_t)(1 << s); redraw = true; }
                }
            } else if (gPage == guition::PAGE_SETTINGS) {
                if (gSetView == 1 && gBindMenuRole >= 0) {
                    // Source picker open: tap an option to bind it, Back to cancel,
                    // and the nav pills page through a long source list.
                    int vis[guition::BIND_MAXSRC];
                    int vc = guition::bindVisible(gBindMenuRole, lastSrcCount, vis,
                                                  guition::BIND_MAXSRC);
                    int pages = (vc + guition::BIND_PERPAGE - 1) / guition::BIND_PERPAGE;
                    int k = guition::bindMenuHit(tp.x, tp.y);
                    if (k == -2) { gBindMenuRole = -1; redraw = true; }              // Back
                    else if (k == -3) { if (gMenuPage > 0) { gMenuPage--; redraw = true; } }
                    else if (k == -4) { if (gMenuPage < pages - 1) { gMenuPage++; redraw = true; } }
                    else if (k >= 0) {
                        int idx = gMenuPage * guition::BIND_PERPAGE + k;
                        if (idx < vc) {
                            gBindSetRole = gBindMenuRole;
                            gBindSetIdx = vis[idx];
                            gBindMenuRole = -1;
                            redraw = true;
                        }
                    }
                } else {
                    // Sub-view toggle (Tune/Bind/Diag) + per-view controls. Loop-task
                    // work is deferred; brightness applied here. A slave uses Tune +
                    // Diag (no Bind). Diag is a menu of screens (gDiagScreen).
                    int effView = gSetView;
                    if (gRole == ROLE_SLAVE && effView == 1) effView = 0;
                    int v = guition::settingsViewHit(tp.x, tp.y);
                    if (v >= 0 && !(gRole == ROLE_SLAVE && v == 1)) {
                        if (v != gSetView) { gSetView = v; gBindMenuRole = -1; gDiagScreen = 0; redraw = true; }
                    } else if (effView == 2) {
                        switch (guition::diagHit(tp.x, tp.y, gRole, gDiagScreen)) {
                            case guition::DIAG_OPEN_MON:   gDiagScreen = guition::DS_MON;   redraw = true; break;
                            case guition::DIAG_OPEN_DISC:  gDiagScreen = guition::DS_DISC;  redraw = true; break;
                            case guition::DIAG_OPEN_DEBUG: gDiagScreen = guition::DS_DEBUG; redraw = true; break;
                            case guition::DIAG_OPEN_ROLE:  gDiagScreen = guition::DS_ROLE;  redraw = true; break;
                            case guition::DIAG_OPEN_LINK:  gDiagScreen = guition::DS_LINK;  redraw = true; break;
                            case guition::DIAG_BACK:       gDiagScreen = guition::DS_MENU;  redraw = true; break;
                            case guition::DIAG_DEBUG_TOGGLE: gDebugCapture = !gDebugCapture; redraw = true; break;
                            case guition::DIAG_ROLE_TOGGLE: gRoleReq = true; break;
                            case guition::DIAG_UNPAIR: gRx.unpair(); redraw = true; break;
                            default: break;
                        }
                    } else if (effView == 1) {
                        int h = guition::bindingHit(tp.x, tp.y);
                        if (h == -2) { if (gBindPage > 0) { gBindPage--; redraw = true; } }
                        else if (h == -3) {
                            if (gBindPage < guition::bindListPages() - 1) { gBindPage++; redraw = true; }
                        } else if (h >= 0) {
                            int role = gBindPage * guition::BIND_PERPAGE + h;
                            if (role < (int)sig::kRoleCount) {
                                gBindMenuRole = role;
                                gMenuPage = 0;
                                redraw = true;
                            }
                        }
                    } else {
                        guition::SettingsHitResult h = guition::settingsHit(tp.x, tp.y, gRole);
                        switch (h.action) {
                            case guition::SA_PAIR:
                                if (gRole == ROLE_SLAVE) gRx.startAdopt(); else gPairReq = true;
                                redraw = true; break;
                            case guition::SA_PROFILE:
                                gProfileReq = h.index; break;
                            case guition::SA_SELECT_ROW:
                                gSetSel = h.index; redraw = true; break;
                            case guition::SA_ADJ_DN:
                            case guition::SA_ADJ_UP: {
                                int dir = (h.action == guition::SA_ADJ_UP) ? 1 : -1;
                                if (gSetSel == guition::TUN_BRIGHT) {
                                    int b = gDisplay.brightness() + dir * 10;
                                    if (b < 10) b = 10; if (b > 100) b = 100;
                                    gDisplay.setBrightness((uint8_t)b);
                                } else {
                                    // Show the new value instantly; the loop applies
                                    // the real change + NVS on its next cycle.
                                    float v = tunShown[gSetSel] + dir * kTunStep[gSetSel];
                                    if (v < kTunMin[gSetSel]) v = kTunMin[gSetSel];
                                    if (v > kTunMax[gSetSel]) v = kTunMax[gSetSel];
                                    optWhich = gSetSel; optVal = v; tunShown[gSetSel] = v;
                                    gSetAdjWhich = gSetSel;
                                    gSetAdjSteps += dir;
                                }
                                redraw = true; break;
                            }
                            default: break;
                        }
                    }
                }
            }
        }
        wasDown = down;

        uint32_t now = millis();
        // The Flow page animates its pulsing flow lines, so refresh it faster.
        uint32_t uiInterval = (gPage == guition::PAGE_FLOW) ? 130 : 500;
        if (redraw || now - lastUi >= uiInterval) {
            lastUi = now;
            guition::DashData d;
            if (xSemaphoreTake(gDashMux, pdMS_TO_TICKS(50)) == pdTRUE) {
                d = gDash;
                xSemaphoreGive(gDashMux);
            }
            lastSrcCount = d.srcCount;               // for the next menu-hit mapping
            d.brightness = gDisplay.brightness();  // display-owned, not in the snapshot
            d.histWinMin = (uint16_t)gGraphWinMin;  // reflect the pill instantly; data follows
            d.graphHidden = gGraphHidden;           // legend show/hide (display-owned)
            d.setSel = (uint8_t)gSetSel;            // selected tunable row (display-owned)
            d.setView = (uint8_t)gSetView;          // Settings sub-view (display-owned)
            d.menuRole = gBindMenuRole;             // open source-picker (display-owned)
            d.bindPage = (uint8_t)gBindPage;        // bindings-list page (display-owned)
            d.menuPage = (uint8_t)gMenuPage;        // source-picker page (display-owned)
            d.diagScreen = (uint8_t)gDiagScreen;    // Diag sub-screen (display-owned)
            d.debugCapture = gDebugCapture;         // reflect the toggle instantly (display-owned)
            // Optimistic tunable: show the adjusted number now; clear once the loop
            // has applied it and the snapshot caught up.
            if (optWhich >= 0) {
                if (fabsf(tunVal(d, optWhich) - optVal) < 0.001f) optWhich = -1;
                else setTunVal(d, optWhich, optVal);
            }
            for (int i = 0; i < guition::TUNABLE_N; ++i) tunShown[i] = tunVal(d, i);
            guition::renderPage(gDisplay.canvas(), gPage, d);
            gDisplay.flush();
        }
        // Poll touch ~60 Hz so a tap is caught quickly without over-sampling jitter
        // (blocking, full-frame) redraws — the panel has no partial-update DMA, so
        // each redraw briefly monopolises this task.
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

// Bring up the panel + touch + display task (shared by both roles). Seeds the
// first snapshot for the active role so the task has something to draw.
static void bringUpDisplay() {
    if (!gDisplay.begin(1 /*landscape 480x320*/)) {
        Serial.println("Display init FAILED (PSRAM/panel)");
        return;
    }
    Serial.printf("Display: %dx%d\n", gDisplay.width(), gDisplay.height());
    gTouch.begin(1);
    gDashMux = xSemaphoreCreateMutex();
    if (gRole == ROLE_SLAVE) publishSlaveDash(); else publishDash();  // seed
    // Priority 2 (above the Arduino loop's 1) so touch polling preempts the loop's
    // between-scan work; the task sleeps 8 ms between polls so it never starves it.
    xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 2, nullptr, 1);
    gDisplayOk = true;
}
#endif  // VICMON_DISPLAY

// Legacy snapshot used by slaves (Phase 4 HTTP fallback).
static String buildDataJson() {
    uint32_t now = millis();
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved ba = R(sig::Role::BatteryA, now);
    const char* mode = chargeModeName(chargeMode(ba));
    String j = "{";
    j += "\"system_status\":\"" + String(mode) + "\",";
    j += "\"battery_soc\":" + String(soc.value, 1) + ",";
    j += "\"battery_current\":" + String(ba.value, 2) + ",";
    j += "\"timestamp\":" + String(now / 1000);
    j += "}";
    return j;
}

static String jopt(float v) {  // NAN -> null
    return isnan(v) ? String("null") : String(v, 2);
}
static String bucketJson(const stats::Bucket& b) {
    String j = "{";
    j += "\"solar_ah\":" + String(b.solarAh, 1) + ",\"solar_wh\":" + String(b.solarWh, 0) + ",";
    j += "\"dcdc_ah\":" + String(b.dcdcAh, 1) + ",\"dcdc_wh\":" + String(b.dcdcWh, 0) + ",";
    j += "\"charger_ah\":" + String(b.chargerAh, 1) + ",\"charger_wh\":" + String(b.chargerWh, 0) + ",";
    j += "\"load_ah\":" + String(b.loadAh, 1) + ",\"load_wh\":" + String(b.loadWh, 0) + ",";
    j += "\"charged_ah\":" + String(b.chargedAh, 1) + ",\"charged_wh\":" + String(b.chargedWh, 0) + ",";
    j += "\"discharged_ah\":" + String(b.dischargedAh, 1) +
         ",\"discharged_wh\":" + String(b.dischargedWh, 0) + ",";
    j += "\"soc_min\":" + jopt(b.socMin) + ",\"soc_max\":" + jopt(b.socMax) + ",";
    j += "\"v_min\":" + jopt(b.vMin) + ",\"v_max\":" + jopt(b.vMax) + ",";
    j += "\"peak_solar_w\":" + String(b.peakSolarW, 0) +
         ",\"peak_load_w\":" + String(b.peakLoadW, 0) + ",";
    j += "\"peak_charge_a\":" + String(b.peakChargeA, 1) +
         ",\"peak_discharge_a\":" + String(b.peakDischargeA, 1) + ",";
    j += "\"charge_secs\":" + String(b.chargeSecs) +
         ",\"discharge_secs\":" + String(b.dischargeSecs) + ",";
    j += "\"duration_secs\":" + String(b.durationSecs) +
         ",\"start_epoch\":" + String(b.startEpoch);
    j += "}";
    return j;
}
static String buildStatsJson() {
    uint32_t epoch = currentLocalEpoch();
    String j = "{";
    j += "\"clock\":" + jbool(epoch != 0) + ",\"now_epoch\":" + String(epoch) + ",";
    j += "\"today\":" + bucketJson(gStats.bucket(stats::TODAY)) + ",";
    j += "\"trip\":" + bucketJson(gStats.bucket(stats::TRIP)) + ",";
    j += "\"total\":" + bucketJson(gStats.bucket(stats::TOTAL)) + ",";
    j += "\"days\":[";
    for (size_t i = 0; i < gStats.dayCount(); ++i) {
        const stats::DayRecord& d = gStats.day(i);
        if (i) j += ",";
        j += "{\"date\":" + String(d.dayStamp) +
             ",\"solar_wh\":" + String(d.solarWh, 0) +
             ",\"dcdc_wh\":" + String(d.dcdcWh, 0) +
             ",\"charger_wh\":" + String(d.chargerWh, 0) +
             ",\"load_wh\":" + String(d.loadWh, 0) +
             ",\"discharged_wh\":" + String(d.dischargedWh, 0) +
             ",\"soc_min\":" + jopt(d.socMin) + ",\"soc_max\":" + jopt(d.socMax) + "}";
    }
    j += "]}";
    return j;
}

// ---- ESP-NOW broadcast to slaves -------------------------------------------
// Broadcasts a packed snapshot to FF:FF:FF:FF:FF:FF ~1/s. Connectionless, so any
// number of slaves can listen with no pairing and a dropped frame self-heals on
// the next send. Shares the radio with the AP + BLE; the AP is pinned to channel
// 1, and the broadcast peer uses channel 0 ("current channel") to follow it.

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// gEspNowOk + gSnapSeq are declared with the master-identity globals near the top
// (collectDash reads them before this section).
static slavelink::Snapshot gCachedSnap;
static volatile bool gSnapReady = false;
static esp_timer_handle_t gBcastTimer = nullptr;
static void broadcastTick(void*);  // defined after buildSnapshot

static void setupEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        return;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcastMac, 6);
    peer.channel = 0;      // 0 = current WiFi channel (AP pinned to 1)
    peer.encrypt = false;  // broadcast can't be encrypted; telemetry only
    // The master always runs SoftAP (channel-pinned); transmit via the AP
    // interface. The default (STA) interface doesn't exist in AP-only mode, so
    // esp_now_send would fail silently and no slave would ever hear us.
    peer.ifidx = WIFI_IF_AP;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("ESP-NOW peer add failed");
        return;
    }
    gEspNowOk = true;
    // Transmit the cached snapshot ~4/s from a timer, independent of the loop.
    esp_timer_create_args_t ta = {};
    ta.callback = &broadcastTick;
    ta.name = "vbcast";
    if (esp_timer_create(&ta, &gBcastTimer) == ESP_OK)
        esp_timer_start_periodic(gBcastTimer, 250000);  // 250 ms
    Serial.println("ESP-NOW broadcaster ready (250ms tick)");
}

static slavelink::Snapshot buildSnapshot() {
    using namespace slavelink;
    uint32_t now = millis();
    Snapshot s = {};
    fillHeader(s);
    s.masterId = gMasterId;
    s.flags = pairingActive() ? F_PAIRING : 0;

    PanelModel p = collectPanel(now);
    switch (p.mode) {
        case ChargeMode::Charging: s.mode = M_CHARGING; break;
        case ChargeMode::Discharging: s.mode = M_DISCHARGING; break;
        case ChargeMode::Idle: s.mode = M_IDLE; break;
        default: s.mode = M_UNKNOWN; break;
    }

    uint16_t v = 0;
    if (p.soc.valid) v |= V_SOC;
    if (p.battV.valid) v |= V_BATTV;
    if (p.battA.valid) v |= V_BATTA;
    if (p.solarA.valid) v |= V_SOLAR;
    if (p.chargerA.valid) v |= V_CHARGER;
    if (p.dcdcOutA.valid) v |= V_DCDC;
    if (p.loadA.valid) v |= V_LOAD;
    if (p.ttg.valid) v |= V_TTG;
    if (p.starterV.valid) v |= V_STARTERV;
    if (p.solarW.valid) v |= V_SOLARW;
    if (p.solarV.valid) v |= V_SOLARV;
    if (p.dcdcInV.valid) v |= V_DCDCINV;
    if (p.dcdcOutV.valid) v |= V_DCDCOUTV;
    if (p.consumed.valid) v |= V_CONSUMED;
    s.valid = v;

    s.soc_d = encDeci(p.soc.valid, p.soc.value);
    s.battV_cv = encCenti(p.battV.valid, p.battV.value);
    s.battA_da = encDeci(p.battA.valid, p.battA.value);
    s.solarA_da = encDeci(p.solarA.valid, p.solarA.value);
    s.chargerA_da = encDeci(p.chargerA.valid, p.chargerA.value);
    s.dcdcA_da = encDeci(p.dcdcOutA.valid, p.dcdcOutA.value);
    s.loadA_da = encDeci(p.loadA.valid, p.loadA.value);
    s.starterV_cv = encCenti(p.starterV.valid, p.starterV.value);
    s.ttg_min = p.ttg.valid ? (uint16_t)p.ttg.value : 0xFFFF;
    s.solarW_w = encWhole(p.solarW.valid, p.solarW.value);
    s.solarV_cv = encCenti(p.solarV.valid, p.solarV.value);
    s.dcdcInV_cv = encCenti(p.dcdcInV.valid, p.dcdcInV.value);
    s.dcdcOutV_cv = encCenti(p.dcdcOutV.valid, p.dcdcOutV.value);
    s.consumedAh_da = encDeci(p.consumed.valid, p.consumed.value);
    s.capacityAh = (uint16_t)(p.capacity > 0 ? p.capacity + 0.5f : 0);

    s.alertWorst = (uint8_t)p.alertWorst;
    s.profile = (uint8_t)gProfiles.active();
    s.seq = 0;  // stamped per actual transmit in broadcastTick()
    s.uptime_s = now / 1000;
    return s;
}

// The loop refreshes gCachedSnap (registry-owning thread), and a 250 ms esp_timer
// transmits it — so the broadcast rate (~4/s) is independent of the loop's ~2 s
// BLE-blocked cadence. Without this a channel-hopping slave rarely coincides with
// a send and can take a very long time to acquire. seq is stamped per transmit so
// the slave's drop detection stays correct.
// gCachedSnap is written whole by the loop and read+stamped by the esp_timer
// task; a spinlock makes the ~54-byte copy atomic so a tick can't transmit a
// half-updated frame.
static portMUX_TYPE gSnapMux = portMUX_INITIALIZER_UNLOCKED;

static void broadcastTick(void*) {
    if (!gEspNowOk || !gSnapReady) return;
    slavelink::Snapshot s;
    portENTER_CRITICAL(&gSnapMux);
    s = gCachedSnap;
    portEXIT_CRITICAL(&gSnapMux);
    // Stamp per-transmit fields on the local copy (kept fresh between loop builds).
    s.seq = ++gSnapSeq;
    s.uptime_s = millis() / 1000;
    s.flags = pairingActive() ? slavelink::F_PAIRING : 0;
    esp_err_t e = esp_now_send(kBroadcastMac, (const uint8_t*)&s, sizeof(s));
    static uint32_t lastErrLog = 0;
    if (e != ESP_OK && millis() - lastErrLog > 3000) {
        lastErrLog = millis();
        Serial.printf("[espnow] send err 0x%x\n", e);
    }
}

// Called each loop: refresh the cached snapshot from live signals (the timer does
// the actual transmitting).
static void sendSlaveBroadcast() {
    if (!gEspNowOk) return;
    slavelink::Snapshot s = buildSnapshot();
    portENTER_CRITICAL(&gSnapMux);
    gCachedSnap = s;
    gSnapReady = true;
    portEXIT_CRITICAL(&gSnapMux);
}

// Low-rate 7-day energy frame for a slave's Week page (see StatsFrame). Built on
// the loop task (owns gStats) and sent directly — it changes only at the daily
// rollover, so a few sends a minute is plenty.
static uint16_t whU16(float x) { return x <= 0 ? 0 : (x >= 65535 ? 65535 : (uint16_t)(x + 0.5f)); }

static void sendStatsFrame() {
    if (!gEspNowOk) return;
    using namespace slavelink;
    StatsFrame f = {};
    fillStatsHeader(f);
    f.masterId = gMasterId;
    f.clockOk = currentLocalEpoch() != 0 ? 1 : 0;
    int dc = (int)gStats.dayCount();
    int start = dc > 7 ? dc - 7 : 0, out = 0;
    for (int i = start; i < dc && out < 7; ++i) {
        const stats::DayRecord& r = gStats.day(i);
        f.daySolarWh[out] = whU16(r.solarWh);
        f.dayDcdcWh[out] = whU16(r.dcdcWh);
        f.dayChargerWh[out] = whU16(r.chargerWh);
        f.dayLoadWh[out] = whU16(r.loadWh);
        f.dayStamp[out] = r.dayStamp;
        ++out;
    }
    f.dayCount = (uint8_t)out;
    const stats::Bucket& tb = gStats.bucket(stats::TODAY);
    f.todaySolarWh = whU16(tb.solarWh);
    f.todayDcdcWh = whU16(tb.dcdcWh);
    f.todayChargerWh = whU16(tb.chargerWh);
    f.todayLoadWh = whU16(tb.loadWh);
    esp_now_send(kBroadcastMac, (const uint8_t*)&f, sizeof(f));
}

// ---- web app ---------------------------------------------------------------




static String pageHead(const char* active) {
    String h = F("<!doctype html><html><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>Vicmon</title><link rel=stylesheet href=/style.css></head><body>"
                 "<header><h1>VICMON</h1>");
    h += "<span class=muted style='font-size:.8em'>" + String(gProfiles.name(gProfiles.active())) +
         "</span><nav>";
    struct {
        const char* href;
        const char* name;
    } links[] = {{"/", "Mimic"}, {"/stats", "Stats"}, {"/devices", "Devices"},
                 {"/bindings", "Settings"}, {"/diag", "Diag"}};
    for (auto& l : links) {
        // A slave has no BLE devices / local history — hide those pages, leaving
        // the live Mimic + Settings (System card: pair / role / unpair).
        if (gRole == ROLE_SLAVE && (strcmp(l.href, "/devices") == 0 ||
                                    strcmp(l.href, "/stats") == 0 ||
                                    strcmp(l.href, "/diag") == 0))
            continue;
        h += "<a href='";
        h += l.href;
        h += "'";
        if (strcmp(l.href, active) == 0) h += " class=active";
        h += ">";
        h += l.name;
        h += "</a>";
    }
    h += F("</nav></header><main>");
    return h;
}
static String pageFoot() { return F("</main></body></html>"); }

static String keyHex(const uint8_t* k) {
    char b[33];
    for (int i = 0; i < 16; ++i) snprintf(b + i * 2, 3, "%02x", k[i]);
    return String(b);
}
static String modelHex(uint16_t m) {
    char b[8];
    snprintf(b, sizeof(b), "0x%04X", m);
    return String(b);
}
static String typeOptions(victron::Record sel) {
    struct { const char* v; victron::Record r; } t[] = {
        {"battery", victron::Record::BatteryMonitor},
        {"solar", victron::Record::SolarCharger},
        {"dcdc", victron::Record::OrionXs},
        {"charger", victron::Record::AcCharger}};
    String o;
    for (auto& x : t)
        o += String("<option value=") + x.v + (x.r == sel ? " selected" : "") + ">" + x.v + "</option>";
    return o;
}
static String deviceSummary(DeviceSlot& s, uint32_t now) {
    if (s.stale(now)) return "<span class=muted>stale / not seen</span>";
    auto sgn = [](float a) { return (a >= 0 ? String("+") : String("")) + String(a, 1); };
    switch (s.type) {
        case victron::Record::BatteryMonitor:
            return String(s.battery.soc, 1) + "% &middot; " + String(s.battery.voltage, 2) +
                   "V &middot; " + sgn(s.battery.current) + "A";
        case victron::Record::OrionXs:
            return "out " + String(s.dcdc.outputVoltage, 2) + "V &middot; " +
                   String(s.dcdc.outputCurrent, 1) + "A &middot; " +
                   (s.dcdc.deviceState ? "on" : "off");
        case victron::Record::SolarCharger:
            return "PV " + String(s.solar.pvPower, 0) + "W &middot; " +
                   String(s.solar.batteryCurrent, 1) + "A &middot; " +
                   String(s.solar.batteryVoltage, 2) + "V";
        case victron::Record::AcCharger:
            return String(s.charger.batteryVoltage, 2) + "V &middot; " +
                   String(s.charger.batteryCurrent, 1) + "A";
        default:
            return "ok";
    }
}
static String jsEsc(String s) {
    s.replace("\\", "\\\\");
    s.replace("'", "\\'");
    return s;
}

static String devicesPage() {
    uint32_t now = millis();
    String h = pageHead("/devices");

    h += "<div class=card><h3>Configured devices</h3>";
    if (gConfig.count() == 0) h += "<p class=muted>None yet.</p>";
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        h += "<div style='padding:.6em 0;border-bottom:1px solid var(--line)'>";
        h += "<div style='display:flex;justify-content:space-between;align-items:baseline'><b>" +
             String(s.name) + "</b><span class=muted style='font-size:.8em'>" + typeName(s.type) +
             "</span></div>";
        h += "<div style='margin:.25em 0;font-size:1.05em'>" + deviceSummary(s, now) + "</div>";
        if (s.mac[0])
            h += "<div class=muted style='font-size:.78em'>" + String(s.mac) +
                 (s.btname[0] ? " &middot; " + String(s.btname) : "") + "</div>";
        h += "<details><summary class=muted style='cursor:pointer;font-size:.85em'>edit</summary>";
        h += "<form class=inline method=post action=/edit style='margin:.5em 0'>";
        h += "<input type=hidden name=idx value=" + String(i) + ">";
        h += "<div><label>Name</label><input name=name value='" + String(s.name) + "' required></div>";
        h += "<div><label>Type</label><select name=type>" + typeOptions(s.type) + "</select></div>";
        h += "<div><label>Encryption key</label><input name=key size=34 value='" +
             keyHex(s.key) + "'></div>";
        h += "<button>save</button></form>";
        h += "<form class=inline method=post action=/del><input type=hidden name=name value='" +
             String(s.name) + "'><button class=danger>delete</button></form></details></div>";
    }
    h += "</div>";

    h += "<div class=card id=add><h3>Add device</h3>"
         "<form class=inline method=post action=/add>"
         "<div><label>Name</label><input id=addName name=name required></div>"
         "<div><label>Type</label><select id=addType name=type>" +
         typeOptions(victron::Record::BatteryMonitor) +
         "</select></div>"
         "<div><label>Key (32 hex)</label><input id=addKey name=key size=34 required></div>"
         "<button>add</button></form></div>";

    h += "<div class=card><h3>Discovered nearby</h3>"
         "<p class=muted>Victron devices broadcasting that aren't configured yet. "
         "Tap <b>use</b> to start adding one, then paste its encryption key from VictronConnect.</p>"
         "<table><tr><th>Name</th><th>MAC</th><th>Model</th><th>Signal</th><th></th></tr>";
    size_t shown = 0;
    for (size_t i = 0; i < gDiscN; ++i) {
        if (now - gDisc[i].lastSeenMs > 30000) continue;  // only recently seen
        bool configured = false;  // hide devices we've already adopted
        for (size_t j = 0; j < gConfig.count(); ++j)
            if (strncmp(gConfig.slots()[j].mac, gDisc[i].mac, sizeof(gDisc[i].mac)) == 0) {
                configured = true;
                break;
            }
        if (configured) continue;
        String nm = gDisc[i].name[0] ? String(gDisc[i].name) : String("(unnamed)");
        h += "<tr><td>" + nm + "</td><td>" + String(gDisc[i].mac) + "</td><td>" +
             modelHex(gDisc[i].model) +
             "</td><td>" + String(gDisc[i].rssi) + " dBm</td><td>"
             "<button type=button class=ghost onclick=\"adopt('" +
             jsEsc(String(gDisc[i].name)) + "')\">use</button></td></tr>";
        ++shown;
    }
    if (shown == 0) h += "<tr><td colspan=5 class=muted>none right now</td></tr>";
    h += "</table></div>";

    h += R"JS(<script>function adopt(n){var l=(n||'').toLowerCase(),t='battery';
if(l.indexOf('solar')>=0)t='solar';else if(l.indexOf('orion')>=0)t='dcdc';
else if(l.indexOf('charg')>=0||l.indexOf('blue')>=0)t='charger';
document.getElementById('addName').value=n||'';document.getElementById('addType').value=t;
location.hash='#add';document.getElementById('addKey').focus();}</script>)JS";

    h += pageFoot();
    return h;
}

static String wifiCard();
static String profilesCard();
static String backupCard();
static String otaCard();
static String systemCard();

static String bindingsPage() {
    String h = pageHead("/bindings");
    h += profilesCard();
    h += "<div class=card><h3>Panel signals</h3>"
         "<p class=muted>Tag which device field feeds each signal the mimic / "
         "display uses. Derived options compute from the battery current vs the "
         "measured sources: <b>charge unexplained</b> = battery charge beyond "
         "solar/charger/DC-DC; <b>load</b> = consumption (sources offset by net "
         "battery flow).</p>"
         "<form method=post action=/bind>";

    for (size_t r = 0; r < sig::kRoleCount; ++r) {
        sig::Role role = static_cast<sig::Role>(r);
        const sig::Binding& cur = gSignals.binding(role);
        h += "<div style='margin-bottom:.7em'><label>" + String(sig::roleLabel(role)) +
             "</label><select name=" + sig::roleKey(role) + " style='min-width:240px'>";
        h += "<option value=''>&mdash; none &mdash;</option>";
        bool currentRole = (role == sig::Role::SolarA || role == sig::Role::ChargerA ||
                            role == sig::Role::DcDcInA || role == sig::Role::DcDcOutA ||
                            role == sig::Role::LoadA);
        if (currentRole) {
            bool selC = strcmp(cur.device, sig::kChargeOnly) == 0;
            bool selL = strcmp(cur.device, sig::kLoadOnly) == 0 ||
                        strcmp(cur.device, sig::kDerived) == 0;
            h += String("<option value='(charge_only)|0'") + (selC ? " selected" : "") +
                 ">Derived: charge unexplained by sources</option>";
            h += String("<option value='(load_only)|0'") + (selL ? " selected" : "") +
                 ">Derived: load (sources &minus; battery)</option>";
        }
        for (size_t i = 0; i < gConfig.count(); ++i) {
            DeviceSlot& s = gConfig.slots()[i];
            sig::Field fields[8];
            size_t nf = sig::fieldsForType(s.type, fields, 8);
            for (size_t k = 0; k < nf; ++k) {
                String val = String(s.name) + "|" + String(static_cast<int>(fields[k]));
                bool sel = (strcmp(cur.device, s.name) == 0 && cur.field == fields[k]);
                h += "<option value='" + val + "'" + (sel ? " selected" : "") + ">" +
                     String(s.name) + " &middot; " + sig::fieldLabel(fields[k]) + "</option>";
            }
        }
        h += "</select></div>";
    }
    h += "<button>save bindings</button></form></div>";

    h += "<div class=card><h3>System settings</h3>"
         "<form class=inline method=post action=/capacity>"
         "<div><label>Battery capacity (Ah, 0 = unknown)</label>"
         "<input name=cap type=number min=0 step=1 value='" + String(gBattCapacity, 0) + "'></div>"
         "<div><label>Idle deadband (A)</label>"
         "<input name=deadband type=number min=0 step=0.1 value='" + String(gDeadband, 1) + "'></div>"
         "<div><label>Time zone (min from UTC)</label>"
         "<input name=tzmin type=number step=15 value='" + String(gTzOffsetMin) + "'></div>"
         "<button>save</button></form>"
         "<p class=muted>Capacity shows remaining Ah on the mimic. Currents within "
         "&plusmn;deadband read as <i>idle</i>. Time zone aligns the daily stats "
         "rollover to local midnight (e.g. 600 = AEST +10h); needs WiFi/NTP.</p></div>";

    h += "<div class=card><h3>Alerts</h3>"
         "<form class=inline method=post action=/alerts>"
         "<div><label>SoC warn (%)</label>"
         "<input name=socwarn type=number min=0 max=100 step=1 value='" + String(gSocWarn, 0) + "'></div>"
         "<div><label>SoC critical (%)</label>"
         "<input name=soccrit type=number min=0 max=100 step=1 value='" + String(gSocCrit, 0) + "'></div>"
         "<div><label>Voltage low (V)</label>"
         "<input name=vlow type=number min=0 step=0.1 value='" + String(gVlow, 1) + "'></div>"
         "<div><label>Voltage high (V)</label>"
         "<input name=vhigh type=number min=0 step=0.1 value='" + String(gVhigh, 1) + "'></div>"
         "<button>save</button></form>"
         "<p class=muted>Shown as a banner on the mimic and on the onboard LED "
         "(red = critical, amber = warning, green = charging). 0 disables a check. "
         "A configured device that stops broadcasting also raises a warning.</p></div>";

    h += systemCard();
    h += wifiCard();
    h += otaCard();
    h += backupCard();
    h += pageFoot();
    return h;
}

// ---- handlers --------------------------------------------------------------

static String param(AsyncWebServerRequest* req, const char* k) {
    return req->hasParam(k, true) ? req->getParam(k, true)->value() : String("");
}

static String cleanKey(String key) {
    key.trim();
    key.replace(" ", "");
    return key;
}

static void handleAdd(AsyncWebServerRequest* req) {
    String name = param(req, "name"), type = param(req, "type"), key = cleanKey(param(req, "key"));
    uint8_t k[16];
    if (name.length() && DeviceConfig::parseHexKey(key, k)) {
        gConfig.add(name.c_str(), parseType(type), k);
        gConfig.save();
        // Auto-bind sensible defaults if this profile has no bindings yet.
        gSignals.begin(gConfig.slots(), gConfig.count(), gProfiles.active());
    }
    req->redirect("/devices");
}

static void handleEdit(AsyncWebServerRequest* req) {
    int idx = param(req, "idx").toInt();
    String name = param(req, "name"), type = param(req, "type"), key = cleanKey(param(req, "key"));
    uint8_t k[16];
    bool haveKey = DeviceConfig::parseHexKey(key, k);
    if (name.length()) {
        gConfig.update(idx, name.c_str(), parseType(type), haveKey ? k : nullptr);
        gConfig.save();
    }
    req->redirect("/devices");
}

static void handleDel(AsyncWebServerRequest* req) {
    String name = param(req, "name");
    if (name.length()) {
        gConfig.remove(name.c_str());
        gConfig.save();
    }
    req->redirect("/devices");
}

static void handleCapacity(AsyncWebServerRequest* req) {
    float c = param(req, "cap").toFloat();
    if (c < 0) c = 0;
    float db = param(req, "deadband").toFloat();
    if (db < 0) db = 0;
    int tz = param(req, "tzmin").toInt();
    if (tz < -720 || tz > 840) tz = gTzOffsetMin;
    saveSettings(c, db, tz);
    req->redirect("/bindings");
}

static void handleAlerts(AsyncWebServerRequest* req) {
    float sw = param(req, "socwarn").toFloat();
    float sc = param(req, "soccrit").toFloat();
    float vl = param(req, "vlow").toFloat();
    float vh = param(req, "vhigh").toFloat();
    saveAlertSettings(sw, sc, vl, vh);
    req->redirect("/bindings");
}

static void handleStatsReset(AsyncWebServerRequest* req) {
    String scope = param(req, "scope");
    stats::Scope sc = scope == "today" ? stats::TODAY : (scope == "total" ? stats::TOTAL : stats::TRIP);
    gStats.reset(sc);
    gStats.maybePersist(millis(), /*force=*/true);
    req->redirect("/stats");
}

static void handleBind(AsyncWebServerRequest* req) {
    for (size_t r = 0; r < sig::kRoleCount; ++r) {
        sig::Role role = static_cast<sig::Role>(r);
        String v = param(req, sig::roleKey(role));
        int bar = v.indexOf('|');
        if (bar < 0) {
            gSignals.set(role, "", sig::Field::None);
        } else {
            String dev = v.substring(0, bar);
            sig::Field f = static_cast<sig::Field>(v.substring(bar + 1).toInt());
            gSignals.set(role, dev.c_str(), f);
        }
    }
    gSignals.save();
    req->redirect("/bindings");
}

static String wifiCard() {
    String status;
    if (gStaSsid.length()) {
        status = (WiFi.status() == WL_CONNECTED)
                     ? "Connected to <b>" + gStaSsid + "</b> &middot; IP " + WiFi.localIP().toString()
                     : "Configured for <b>" + gStaSsid + "</b> &middot; <span class=muted>connecting / not connected</span>";
    } else {
        status = "<span class=muted>Not configured (AP only)</span>";
    }
    return "<div class=card><h3>Join a WiFi network</h3>"
           "<p class=muted>The master always keeps its own <b>" + String(kApSsid) +
           "</b> access point, and can additionally join an existing network (e.g. a "
           "van router) so you can reach it there too.</p>"
           "<p>Status: " + status + "</p>"
           "<form class=inline method=post action=/wifi>"
           "<div><label>SSID</label><input name=ssid value='" + gStaSsid + "' required></div>"
           "<div><label>Password (blank = keep)</label><input name=pass type=password></div>"
           "<button>save &amp; connect</button></form>"
           "<form method=post action=/wifi style='margin-top:.6em'>"
           "<input type=hidden name=ssid value=''><button class=ghost>forget</button></form></div>";
}

static void handleWifi(AsyncWebServerRequest* req) {
    String ssid = param(req, "ssid"), pass = param(req, "pass");
    if (ssid.length() == 0) {  // forget
        saveWifiCreds("", "");
        gStaSsid = ""; gStaPass = "";
        WiFi.disconnect();
        req->redirect("/bindings");
        return;
    }
    if (pass.length() == 0) pass = gStaPass;  // keep existing when blank
    saveWifiCreds(ssid, pass);
    gStaSsid = ssid; gStaPass = pass;
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(gStaSsid.c_str(), gStaPass.c_str());
    configTime(0, 0, "pool.ntp.org");  // sync clock for daily-stats rollover
    req->redirect("/bindings");
}

static String profilesCard() {
    String h = "<div class=card><h3>Profiles</h3>"
         "<p class=muted>Each profile has its own devices, signal bindings and "
         "settings (e.g. Home vs 4WD). Switching applies immediately.</p>";
    for (int i = 0; i < ProfileManager::kMax; ++i) {
        if (!gProfiles.used(i)) continue;
        bool act = (i == gProfiles.active());
        h += "<div style='padding:.5em 0;border-bottom:1px solid var(--line);display:flex;"
             "gap:.5em;align-items:center;flex-wrap:wrap'>";
        h += "<b style='flex:1'>" + String(gProfiles.name(i)) +
             (act ? " <span class=muted>(active)</span>" : "") + "</b>";
        if (!act)
            h += "<form method=post action=/profile/switch style='margin:0'>"
                 "<input type=hidden name=id value=" + String(i) + "><button>switch</button></form>";
        h += "<form class=inline method=post action=/profile/rename style='margin:0'>"
             "<input type=hidden name=id value=" + String(i) + ">"
             "<input name=name value='" + String(gProfiles.name(i)) + "' size=12>"
             "<button class=ghost>rename</button></form>";
        if (!act && gProfiles.usedCount() > 1)
            h += "<form method=post action=/profile/del style='margin:0' "
                 "onsubmit=\"return confirm('Delete this profile and all its data?')\">"
                 "<input type=hidden name=id value=" + String(i) + ">"
                 "<button class=danger>delete</button></form>";
        h += "</div>";
    }
    if (gProfiles.usedCount() < ProfileManager::kMax)
        h += "<form class=inline method=post action=/profile/new style='margin-top:.8em'>"
             "<div><label>New profile name</label><input name=name required></div>"
             "<button>create</button></form>";
    else
        h += "<p class=muted>Maximum profiles reached.</p>";
    h += "</div>";
    return h;
}

static void handleProfileSwitch(AsyncWebServerRequest* req) {
#ifndef VICMON_SIM
    saveHistFile(gProfiles.active());  // flush the outgoing profile's history first
#endif
    gProfiles.setActive(param(req, "id").toInt());
    applyProfile(gProfiles.active());
    req->redirect("/");
}
static void handleProfileNew(AsyncWebServerRequest* req) {
    String name = param(req, "name");
    if (name.length()) gProfiles.create(name.c_str());
    req->redirect("/bindings");
}
static void handleProfileRename(AsyncWebServerRequest* req) {
    gProfiles.rename(param(req, "id").toInt(), param(req, "name").c_str());
    req->redirect("/bindings");
}
static void handleProfileDel(AsyncWebServerRequest* req) {
    int id = param(req, "id").toInt();
    if (id != gProfiles.active()) {
        wipeProfile(id);
        gProfiles.remove(id);
    }
    req->redirect("/bindings");
}

// ---- config backup / restore -----------------------------------------------

static String jsonEsc(const String& s) {
    String o;
    o.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<uint8_t>(c) < 0x20) { /* drop other control chars */ }
        else o += c;
    }
    return o;
}

// Reusable temporaries kept off the (small) async-task stack.
static DeviceConfig gTmpCfg;
static sig::SignalMap gTmpSig;

static String buildExportJson() {
    String j = "{\"version\":1,\"active\":" + String(gProfiles.active()) + ",";
    j += "\"wifi\":{\"ssid\":\"" + jsonEsc(gStaSsid) + "\",\"pass\":\"" + jsonEsc(gStaPass) +
         "\"},\"profiles\":[";
    bool firstP = true;
    for (int pid = 0; pid < ProfileManager::kMax; ++pid) {
        if (!gProfiles.used(pid)) continue;
        if (!firstP) j += ",";
        firstP = false;
        gTmpCfg.begin(pid);
        gTmpSig.begin(gTmpCfg.slots(), gTmpCfg.count(), pid);
        j += "{\"id\":" + String(pid) + ",\"name\":\"" + jsonEsc(gProfiles.name(pid)) + "\",";
        Preferences p;
        p.begin(settingsNs(pid).c_str(), true);
        j += "\"settings\":{";
        j += "\"battcap\":" + String(p.getFloat("battcap", 0), 0) + ",";
        j += "\"deadband\":" + String(p.getFloat("deadband", 0.2f), 2) + ",";
        j += "\"tzmin\":" + String(p.getInt("tzmin", 600)) + ",";
        j += "\"socwarn\":" + String(p.getFloat("socwarn", 50), 0) + ",";
        j += "\"soccrit\":" + String(p.getFloat("soccrit", 30), 0) + ",";
        j += "\"vlow\":" + String(p.getFloat("vlow", 11.8f), 2) + ",";
        j += "\"vhigh\":" + String(p.getFloat("vhigh", 15.0f), 2) + "},";
        p.end();
        j += "\"devices\":[";
        for (size_t i = 0; i < gTmpCfg.count(); ++i) {
            if (i) j += ",";
            DeviceSlot& s = gTmpCfg.slots()[i];
            j += "{\"name\":\"" + jsonEsc(s.name) + "\",\"type\":\"" + typeName(s.type) +
                 "\",\"key\":\"" + keyHex(s.key) + "\"}";
        }
        j += "],\"bindings\":[";
        bool firstB = true;
        for (size_t r = 0; r < sig::kRoleCount; ++r) {
            const sig::Binding& b = gTmpSig.binding(static_cast<sig::Role>(r));
            if (!b.device[0]) continue;
            if (!firstB) j += ",";
            firstB = false;
            j += "{\"role\":\"" + String(sig::roleKey(static_cast<sig::Role>(r))) +
                 "\",\"device\":\"" + jsonEsc(b.device) + "\",\"field\":" +
                 String(static_cast<int>(b.field)) + "}";
        }
        j += "]}";
    }
    j += "]}";
    return j;
}

// Restores profiles present in the backup (overwriting them); profiles absent
// from the file are left untouched.
static bool applyImport(const String& body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonArray profs = doc["profiles"].as<JsonArray>();
    if (profs.isNull()) return false;

    for (JsonObject pr : profs) {
        int pid = pr["id"] | -1;
        if (pid < 0 || pid >= ProfileManager::kMax) continue;
        gProfiles.setName(pid, pr["name"] | "Profile");
        wipeProfile(pid);

        gTmpCfg.begin(pid);
        gTmpCfg.clear();  // drop any seeded defaults; install exactly the backup
        for (JsonObject d : pr["devices"].as<JsonArray>()) {
            const char* dn = d["name"] | "";
            uint8_t k[16];
            if (dn[0] && DeviceConfig::parseHexKey(String((const char*)(d["key"] | "")), k))
                gTmpCfg.add(dn, parseType(String((const char*)(d["type"] | "battery"))), k);
        }
        gTmpCfg.save();

        gTmpSig.begin(gTmpCfg.slots(), gTmpCfg.count(), pid);
        for (size_t r = 0; r < sig::kRoleCount; ++r)
            gTmpSig.set(static_cast<sig::Role>(r), "", sig::Field::None);
        for (JsonObject bd : pr["bindings"].as<JsonArray>()) {
            const char* rk = bd["role"] | "";
            for (size_t r = 0; r < sig::kRoleCount; ++r)
                if (strcmp(sig::roleKey(static_cast<sig::Role>(r)), rk) == 0)
                    gTmpSig.set(static_cast<sig::Role>(r), bd["device"] | "",
                                static_cast<sig::Field>(bd["field"] | 0));
        }
        gTmpSig.save();

        JsonObject st = pr["settings"];
        Preferences p;
        p.begin(settingsNs(pid).c_str(), false);
        p.putFloat("battcap", st["battcap"] | 0.0f);
        p.putFloat("deadband", st["deadband"] | 0.2f);
        p.putInt("tzmin", st["tzmin"] | 600);
        p.putFloat("socwarn", st["socwarn"] | 50.0f);
        p.putFloat("soccrit", st["soccrit"] | 30.0f);
        p.putFloat("vlow", st["vlow"] | 11.8f);
        p.putFloat("vhigh", st["vhigh"] | 15.0f);
        p.end();
    }

    JsonObject w = doc["wifi"];
    if (!w.isNull()) {
        gStaSsid = String((const char*)(w["ssid"] | ""));
        gStaPass = String((const char*)(w["pass"] | ""));
        saveWifiCreds(gStaSsid, gStaPass);
    }
    int active = doc["active"] | 0;
    if (active < 0 || active >= ProfileManager::kMax || !gProfiles.used(active))
        active = gProfiles.active();
    gProfiles.setActive(active);
    applyProfile(active);
    return true;
}

static String gImportBuf;
static void handleImportBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                             size_t index, size_t total) {
    if (index == 0) { gImportBuf = ""; gImportBuf.reserve(total + 1); }
    for (size_t i = 0; i < len; ++i) gImportBuf += static_cast<char>(data[i]);
}
static void handleImport(AsyncWebServerRequest* req) {
    bool ok = applyImport(gImportBuf);
    gImportBuf = String();
    req->send(ok ? 200 : 400, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static String backupCard() {
    return F(
        "<div class=card><h3>Backup &amp; restore</h3>"
        "<p class=muted>Download every profile (devices, encryption keys, signal "
        "bindings and settings) as a JSON file, or restore from one &mdash; handy "
        "before <code>erase</code>/reflash and to clone a second unit. Restoring "
        "overwrites the profiles contained in the file.</p>"
        "<a href=/api/config/export download=vicmon-config.json>"
        "<button type=button>Download backup</button></a>"
        "<div style='margin-top:.9em;display:flex;gap:.6em;flex-wrap:wrap;align-items:center'>"
        "<input type=file id=rf accept=.json,application/json>"
        "<button type=button id=rb class=ghost>Restore from file</button></div>"
        "<div id=rmsg class=muted style='margin-top:.5em'></div>"
        "<script>"
        "document.getElementById('rb').addEventListener('click',function(){"
        "var f=document.getElementById('rf').files[0];var m=document.getElementById('rmsg');"
        "if(!f){m.textContent='Choose a file first.';return;}"
        "if(!confirm('Restore from this file? Profiles in the file will be overwritten.'))return;"
        "var r=new FileReader();r.onload=function(){"
        "fetch('/api/config/import',{method:'POST',body:r.result})"
        ".then(function(x){return x.json();}).then(function(j){"
        "m.textContent=j.ok?'Restored \\u2014 reloading\\u2026':'Import failed: invalid file.';"
        "if(j.ok)setTimeout(function(){location.href='/';},1200);})"
        ".catch(function(){m.textContent='Import request failed.';});};r.readAsText(f);});"
        "</script></div>");
}

// ---- diagnostics (raw decode) ----------------------------------------------

static String hexBytes(const uint8_t* p, size_t n) {
    String s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
        if (i + 1 < n) s += ' ';
    }
    return s;
}

static String diagFields(DeviceSlot& s) {
    String a = "[";
    bool first = true;
    auto fmt = [](bool valid, float v, int dp, const char* unit) {
        return valid ? String(v, dp) + unit : String("--");
    };
    auto add = [&](const char* k, const String& v) {
        if (!first) a += ",";
        first = false;
        a += "[\"" + String(k) + "\",\"" + v + "\"]";
    };
    switch (s.type) {
        case victron::Record::BatteryMonitor: {
            auto& b = s.battery;
            add("SoC", fmt(b.socValid, b.soc, 1, "%"));
            add("Voltage", fmt(b.voltageValid, b.voltage, 2, "V"));
            add("Current", fmt(b.currentValid, b.current, 2, "A"));
            add("Consumed", fmt(b.consumedValid, b.consumedAh, 1, "Ah"));
            add("Aux (starter)", fmt(b.auxValid, b.auxValue, 2, "V"));
            add("Time to go", b.ttgValid ? String(b.timeToGoMin) + "min" : String("--"));
            add("Alarm", String(b.alarm));
            break;
        }
        case victron::Record::OrionXs: {
            auto& d = s.dcdc;
            add("State", String(d.deviceState));
            add("Input V", fmt(d.inputVValid, d.inputVoltage, 2, "V"));
            add("Output V", fmt(d.outputVValid, d.outputVoltage, 2, "V"));
            add("Input A", fmt(d.inputIValid, d.inputCurrent, 1, "A"));
            add("Output A", fmt(d.outputIValid, d.outputCurrent, 1, "A"));
            add("Error", String(d.chargerError));
            break;
        }
        case victron::Record::SolarCharger: {
            auto& v = s.solar;
            add("State", String(v.deviceState));
            add("Battery V", fmt(v.battVValid, v.batteryVoltage, 2, "V"));
            add("Battery A", fmt(v.battIValid, v.batteryCurrent, 1, "A"));
            add("PV power", fmt(v.pvValid, v.pvPower, 0, "W"));
            add("Yield today", fmt(v.yieldValid, v.yieldToday, 2, "kWh"));
            add("Load A", fmt(v.loadValid, v.loadCurrent, 1, "A"));
            add("Error", String(v.chargerError));
            break;
        }
        case victron::Record::AcCharger: {
            auto& c = s.charger;
            add("State", String(c.deviceState));
            add("Battery V", fmt(c.battVValid, c.batteryVoltage, 2, "V"));
            add("Battery A", fmt(c.battIValid, c.batteryCurrent, 1, "A"));
            add("Error", String(c.chargerError));
            break;
        }
        default:
            break;
    }
    a += "]";
    return a;
}

static String buildDiagJson() {
    uint32_t now = millis();
    String j = "[";
    for (size_t i = 0; i < gConfig.count(); ++i) {
        if (i) j += ",";
        DeviceSlot& s = gConfig.slots()[i];
        uint32_t age = s.everSeen ? (now - s.lastSeenMs) / 1000 : 0;
        j += "{\"name\":\"" + jsonEsc(s.name) + "\",\"type\":\"" + typeName(s.type) + "\",";
        j += "\"mac\":\"" + String(s.mac) + "\",\"model\":\"" + modelHex(s.modelId) + "\",";
        j += "\"seen\":" + jbool(s.everSeen) + ",\"stale\":" + jbool(s.stale(now)) +
             ",\"age\":" + String(age) + ",";
        j += "\"raw\":\"" + hexBytes(s.raw, s.rawLen) + "\",";
        j += "\"fields\":" + diagFields(s);
        j += "}";
    }
    j += "]";
    return j;
}


// ---- OTA firmware update ----------------------------------------------------

static void handleOtaUpload(AsyncWebServerRequest* req, String filename, size_t index,
                            uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        Serial.printf("[OTA] start: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    }
    if (Update.write(data, len) != len) Update.printError(Serial);
    if (final) {
        if (Update.end(true))
            Serial.printf("[OTA] success: %u bytes\n", (unsigned)(index + len));
        else
            Update.printError(Serial);
    }
}
static void handleOtaDone(AsyncWebServerRequest* req) {
    bool ok = !Update.hasError();
    AsyncWebServerResponse* res =
        req->beginResponse(200, "text/plain", ok ? "OK, rebooting" : "FAILED");
    res->addHeader("Connection", "close");
    req->send(res);
    if (ok) {
        delay(200);
        ESP.restart();
    }
}

static String otaCard() {
    return F(
        "<div class=card><h3>Firmware update (OTA)</h3>"
        "<p class=muted>Upload a compiled <code>firmware.bin</code> (the atoms3 build, at "
        "<code>.pio/build/atoms3/firmware.bin</code>) to flash over WiFi. The device "
        "reboots when done &mdash; wait ~10 s then reload.</p>"
        "<input type=file id=fw accept=.bin>"
        "<button type=button id=fwb class=ghost>Upload &amp; flash</button>"
        "<div id=fwmsg class=muted style='margin-top:.5em'></div>"
        "<script>"
        "document.getElementById('fwb').addEventListener('click',function(){"
        "var f=document.getElementById('fw').files[0];var m=document.getElementById('fwmsg');"
        "if(!f){m.textContent='Choose a firmware.bin first.';return;}"
        "if(!confirm('Flash this firmware? The device will reboot.'))return;"
        "m.textContent='Uploading\\u2026 do not close this page.';"
        "var fd=new FormData();fd.append('f',f);"
        "fetch('/api/ota',{method:'POST',body:fd}).then(function(x){return x.text();})"
        ".then(function(t){m.textContent=t.indexOf('OK')>=0?"
        "'Flashed \\u2014 rebooting, reload in ~10 s.':'Update failed (see serial log).';})"
        ".catch(function(){m.textContent='Upload sent; if it succeeded the device is rebooting.';});});"
        "</script></div>");
}

// System / pairing card — the web equivalent of the on-screen Diag controls, so
// anything doable on the display is doable from the AP (pair a slave, toggle debug
// capture, switch role). Shown on the settings page for both roles.
static String systemCard() {
    char idbuf[12];
    snprintf(idbuf, sizeof(idbuf), "%08X", gRole == ROLE_SLAVE ? gRx.pairedMaster() : gMasterId);
    String h = "<div class=card><h3>System</h3>";
    h += "<p class=muted>Role: <b>" + String(gRole == ROLE_SLAVE ? "Slave" : "Master") + "</b> &middot; " +
         String(gRole == ROLE_SLAVE ? "paired master" : "id") + " " + String(idbuf) + "</p>";
    if (gRole == ROLE_SLAVE) {
        h += "<button onclick=\"fetch('/api/pair',{method:'POST'})\">Pair to a master</button> ";
        h += "<button onclick=\"if(confirm('Forget the paired master?'))fetch('/api/unpair',{method:'POST'})\">Unpair</button> ";
        h += "<button onclick=\"if(confirm('Switch to Master and reboot?'))fetch('/api/role',{method:'POST'})\">Switch to Master</button>";
        h += "<p class=muted>Pair while a master's pairing window is open. Switching role reboots.</p>";
    } else {
        h += "<button onclick=\"fetch('/api/pair',{method:'POST'}).then(()=>alert('Pairing window open 60s'))\">Pair a slave</button> ";
        h += "<button onclick=\"fetch('/api/debug',{method:'POST'}).then(()=>location.reload())\">Toggle debug capture</button> ";
        h += "<button onclick=\"if(confirm('Switch to Slave and reboot?'))fetch('/api/role',{method:'POST'})\">Switch to Slave</button>";
        h += "<p class=muted>Pairing lets a slave display adopt this master (60 s window). Debug "
             "capture records raw bytes of unknown Victron devices. Switching role reboots.</p>";
    }
    return h + "</div>";
}

static void setupServer() {
    gServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/css", kStyle);
    });
    // Pairing / role / debug — parity with the on-screen Diag controls.
    gServer.on("/api/pair", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (gRole == ROLE_SLAVE) gRx.startAdopt(); else startPairing();
        req->send(200, "text/plain", "ok");
    });
    gServer.on("/api/unpair", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (gRole == ROLE_SLAVE) gRx.unpair();
        req->send(200, "text/plain", "ok");
    });
    gServer.on("/api/role", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "text/plain", "rebooting");
        gRoleReq = true;  // serviceRole() reboots into the other role
    });
    gServer.on("/api/debug", HTTP_POST, [](AsyncWebServerRequest* req) {
        gDebugCapture = !gDebugCapture;
        req->send(200, "text/plain", gDebugCapture ? "on" : "off");
    });
    gServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", pageHead("/") + kMimicPage + pageFoot());
    });
    gServer.on("/devices", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", devicesPage());
    });
    gServer.on("/bindings", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", bindingsPage());
    });
    gServer.on("/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", pageHead("/stats") + kStatsPage + pageFoot());
    });
    gServer.on("/diag", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", pageHead("/diag") + kDiagPage + pageFoot());
    });
    gServer.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildDiagJson());
    });
    gServer.on("/api/panel", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildPanelJson());
    });
    gServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildStatsJson());
    });
    gServer.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildDataJson());
    });
    gServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
        int mins = req->hasParam("mins") ? req->getParam("mins")->value().toInt() : 10;
        req->send(200, "application/json", buildHistoryJson(mins));
    });
    gServer.on("/api/config/export", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res =
            req->beginResponse(200, "application/json", buildExportJson());
        res->addHeader("Content-Disposition", "attachment; filename=vicmon-config.json");
        req->send(res);
    });
    gServer.on("/api/config/import", HTTP_POST, handleImport, nullptr, handleImportBody);
    gServer.on("/wifi", HTTP_POST, handleWifi);
    gServer.on("/profile/switch", HTTP_POST, handleProfileSwitch);
    gServer.on("/profile/new", HTTP_POST, handleProfileNew);
    gServer.on("/profile/rename", HTTP_POST, handleProfileRename);
    gServer.on("/profile/del", HTTP_POST, handleProfileDel);
    gServer.on("/add", HTTP_POST, handleAdd);
    gServer.on("/edit", HTTP_POST, handleEdit);
    gServer.on("/del", HTTP_POST, handleDel);
    gServer.on("/bind", HTTP_POST, handleBind);
    gServer.on("/capacity", HTTP_POST, handleCapacity);
    gServer.on("/alerts", HTTP_POST, handleAlerts);
    gServer.on("/stats/reset", HTTP_POST, handleStatsReset);
    gServer.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
    gServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(200, "text/html", pageHead("/") + kMimicPage + pageFoot());
    });
    gServer.begin();
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

static void slaveLoop() {
    gDns.processNextRequest();
    gRx.poll();
    sampleSlaveHistory();  // build the Trend history from received frames
    serviceRole();  // "Switch to Master" (reboots)
#ifdef VICMON_DISPLAY
    if (gDisplayOk) { serviceDashRequests(); publishSlaveDash(); }  // apply tunable/brightness taps
#endif
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last >= 1000) {
        last = now;
        if (gRx.isPaired())
            Serial.printf("[slave] %08X %s ch%u drops=%lu\n", gRx.pairedMaster(),
                          gRx.live() ? "live" : "stale", gRx.channel(), (unsigned long)gRx.drops());
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
    Serial.printf("[boot] master id %08X, AP '%s'\n", gMasterId, kApSsid);

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
#endif

    Serial.printf("[state] victron_adverts=%d decoded=%d |", gScanVictron, gScanDecoded);
    for (size_t i = 0; i < gConfig.count(); ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
    }
    Serial.println();
}
