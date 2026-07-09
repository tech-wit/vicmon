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
char kApSsid[24] = "Vicmon";
const char* kApPass = "vicmon1234";  // >= 8 chars; change before the field
const char* kFwVersion = "0.3.0";    // shown on the display Settings page

DeviceConfig gConfig;
sig::SignalMap gSignals;
ProfileManager gProfiles;
stats::Stats gStats;
NimBLEScan* gScan = nullptr;
AsyncWebServer gServer(80);
DNSServer gDns;

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

int gScanVictron = 0, gScanDecoded = 0;  // per-scan diagnostics

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
uint32_t currentLocalEpoch() {
    time_t t = time(nullptr);
    if (t < 1700000000) return 0;  // ~2023-11; NTP not synced yet
    return static_cast<uint32_t>(t) + gTzOffsetMin * 60;
}

// Loads a profile's config/signals/settings and clears runtime caches so the
// mimic, history and discovery don't mix data across profiles.
void applyProfile(int pid) {
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
