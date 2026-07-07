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
#include <esp_wifi.h>

#include <cmath>
#include <cstring>
#include <string>

#include "DeviceConfig.h"
#include "Profiles.h"
#include "Registry.h"
#include "Signals.h"
#include "SlaveLink.h"
#include "Stats.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "VictronTypes.h"

#ifdef VICMON_DISPLAY
#include <GuitionDisplay.h>
#include <GuitionTouch.h>
#include <GfxDashboard.h>
#endif

static const char* kApSsid = "Vicmon-Master";
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
// Set by the display task (Settings page) when the user taps "Next profile";
// consumed on the loop task, which owns the registry / profile switch.
static volatile bool gProfileNextReq = false;
// Graph-page zoom window (minutes), set by the display task, read by collectDash
// on the loop task. Matches the web chart's windows.
static const int kGraphWins[] = {1, 10, 60, 720, 1440};  // 1m 10m 1h 12h 24h
static volatile int gGraphWinMin = 60;
#endif

// Victron devices seen but not configured (no matching key).
struct Discovered {
    char mac[20] = {0};
    char name[24] = {0};  // BLE advertised (friendly) name, if any
    uint16_t model = 0;
    int rssi = 0;
    uint32_t lastSeenMs = 0;
};
static Discovered gDisc[12];
static size_t gDiscN = 0;

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

static void noteDiscovered(const char* mac, const char* name, uint16_t model, int rssi) {
    uint32_t now = millis();
    for (size_t i = 0; i < gDiscN; ++i) {
        if (strncmp(gDisc[i].mac, mac, sizeof(gDisc[i].mac)) == 0) {
            gDisc[i].rssi = rssi;
            gDisc[i].model = model;
            gDisc[i].lastSeenMs = now;
            if (name && name[0]) strncpy(gDisc[i].name, name, sizeof(gDisc[i].name) - 1);
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
    // No configured key matched -> a device we could adopt.
    noteDiscovered(dev->getAddress().toString().c_str(), dev->getName().c_str(),
                   victron::modelId(extra), dev->getRSSI());
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
        if (s.everSeen && s.stale(now, kOfflineMs)) emit(1, String(s.name) + " not responding");
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

static String buildPanelJson() {
    uint32_t now = millis();
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved bv = R(sig::Role::BatteryV, now);
    sig::Resolved ba = R(sig::Role::BatteryA, now);
    sig::Resolved con = R(sig::Role::BatteryConsumed, now);
    sig::Resolved stv = R(sig::Role::BatteryStarterV, now);
    sig::Resolved ttg = R(sig::Role::BatteryTTG, now);
    sig::Resolved sa = resolveSignal(sig::Role::SolarA, now);
    sig::Resolved sw = R(sig::Role::SolarW, now);
    sig::Resolved chg = resolveSignal(sig::Role::ChargerA, now);
    sig::Resolved dia = resolveSignal(sig::Role::DcDcInA, now);
    sig::Resolved doa = resolveSignal(sig::Role::DcDcOutA, now);

    const char* mode = chargeModeName(chargeMode(ba));

    // Load honours derived sentinels ((derived)/(charge_only)/(load_only)).
    sig::Resolved la = resolveSignal(sig::Role::LoadA, now);
    bool loadValid = la.valid;
    bool loadDerived = roleIsDerived(sig::Role::LoadA);
    float loadV = la.value;

    bool battValid = soc.valid || bv.valid || ba.valid;
    bool dcdcValid = doa.valid || dia.valid;

    String j = "{";
    j += "\"mode\":\"" + String(mode) + "\",";
    j += "\"battery\":{\"valid\":" + jbool(battValid) +
         ",\"soc\":" + String(soc.value, 1) +
         ",\"v\":" + String(bv.value, 2) +
         ",\"a\":" + String(ba.value, 2) +
         ",\"consumed\":" + String(con.value, 1) + ",\"consumed_valid\":" + jbool(con.valid) +
         ",\"starter_v\":" + String(stv.value, 2) + ",\"starter_valid\":" + jbool(stv.valid) +
         ",\"ttg\":" + String(ttg.value, 0) + ",\"ttg_valid\":" + jbool(ttg.valid) +
         ",\"capacity\":" + String(gBattCapacity, 0) + "},";
    // Secondary voltages not carried by a signal role (first device of each
    // type). NB: the SmartSolar advert has no PV-array voltage — only the
    // charger's battery-side voltage, PV power and yield — so "solar V" is the
    // battery-side reading.
    sig::Resolved sv = fieldOfType(victron::Record::SolarCharger, sig::Field::SolarBattV, now);
    sig::Resolved div = fieldOfType(victron::Record::OrionXs, sig::Field::DcDcInV, now);
    sig::Resolved dov = fieldOfType(victron::Record::OrionXs, sig::Field::DcDcOutV, now);

    j += "\"solar\":{\"valid\":" + jbool(sa.valid) +
         ",\"a\":" + String(sa.value, 1) +
         ",\"w\":" + String(sw.value, 0) +
         ",\"v\":" + String(sv.value, 2) + ",\"v_valid\":" + jbool(sv.valid) + "},";
    j += "\"charger\":{\"valid\":" + jbool(chg.valid) +
         ",\"a\":" + String(chg.value, 1) + "},";
    j += "\"dcdc\":{\"valid\":" + jbool(dcdcValid) +
         ",\"out_a\":" + String(doa.value, 1) +
         ",\"in_a\":" + String(dia.value, 1) +
         ",\"in_v\":" + String(div.value, 2) + ",\"in_v_valid\":" + jbool(div.valid) +
         ",\"out_v\":" + String(dov.value, 2) + ",\"out_v_valid\":" + jbool(dov.valid) + "},";
    j += "\"load\":{\"valid\":" + jbool(loadValid) +
         ",\"a\":" + String(loadV, 1) +
         ",\"derived\":" + jbool(loadDerived) + "},";
    String alerts;
    buildAlerts(now, &alerts);
    j += "\"alerts\":" + alerts;
    j += "}";
    return j;
}

#ifdef VICMON_DISPLAY
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
        d.histSoc[k]  = s.soc;
        d.histBatt[k] = s.battery;
    }
    d.histCount = out;
}

static void collectDash(guition::DashData& d) {
    uint32_t now = millis();
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved bv  = R(sig::Role::BatteryV, now);
    sig::Resolved ba  = R(sig::Role::BatteryA, now);
    sig::Resolved stv = R(sig::Role::BatteryStarterV, now);
    sig::Resolved ttg = R(sig::Role::BatteryTTG, now);
    sig::Resolved sa  = resolveSignal(sig::Role::SolarA, now);
    sig::Resolved sw  = R(sig::Role::SolarW, now);
    sig::Resolved chg = resolveSignal(sig::Role::ChargerA, now);
    sig::Resolved dia = resolveSignal(sig::Role::DcDcInA, now);
    sig::Resolved doa = resolveSignal(sig::Role::DcDcOutA, now);
    sig::Resolved la  = resolveSignal(sig::Role::LoadA, now);
    sig::Resolved div = fieldOfType(victron::Record::OrionXs, sig::Field::DcDcInV, now);

    d.mode  = chargeModeName(chargeMode(ba));
    d.worst = buildAlerts(now);

    d.battValid = soc.valid || bv.valid || ba.valid;
    d.soc = soc.value; d.v = bv.value; d.a = ba.value;
    d.ttgValid = ttg.valid; d.ttg = ttg.value;
    d.starterValid = stv.valid; d.starterV = stv.value;

    d.solarValid = sa.valid; d.solarW = sw.value; d.solarA = sa.value;
    d.chargerValid = chg.valid; d.chargerA = chg.value;
    d.dcdcValid = doa.valid || dia.valid; d.dcdcOutA = doa.value;
    d.dcdcInVValid = div.valid; d.dcdcInV = div.value;
    d.loadValid = la.valid; d.loadA = la.value;
    d.loadDerived = roleIsDerived(sig::Role::LoadA);

    // Graph page.
    collectHistory(d);

    // Settings page: read-only status (brightness is filled by the display task).
    strncpy(d.profileName, gProfiles.name(gProfiles.active()), sizeof(d.profileName) - 1);
    d.profileName[sizeof(d.profileName) - 1] = '\0';
    d.profileId = gProfiles.active();
    d.profileCount = gProfiles.usedCount();
    strncpy(d.apSsid, kApSsid, sizeof(d.apSsid) - 1);
    d.apSsid[sizeof(d.apSsid) - 1] = '\0';
    WiFi.softAPIP().toString().toCharArray(d.ipStr, sizeof(d.ipStr));
    d.devPaired = (int)gConfig.count();
    d.devSeen = (int)gDiscN;
    d.uptimeSec = now / 1000;
    d.freeHeapKb = ESP.getFreeHeap() / 1024;
    strncpy(d.version, kFwVersion, sizeof(d.version) - 1);
    d.version[sizeof(d.version) - 1] = '\0';
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

// Handle deferred requests from the display task that must run on the loop task
// (registry owner). Currently: "Next profile" from the Settings page.
static void serviceDashRequests() {
    if (!gProfileNextReq) return;
    gProfileNextReq = false;
    int cur = gProfiles.active();
    for (int i = 1; i <= ProfileManager::kMax; ++i) {
        int cand = (cur + i) % ProfileManager::kMax;
        if (cand != cur && gProfiles.used(cand)) {
            saveHistFile(cur);           // flush the outgoing profile's history
            gProfiles.setActive(cand);
            applyProfile(cand);
            Serial.printf("[display] switched to profile '%s'\n", gProfiles.name(cand));
            break;
        }
    }
}

// Display + touch task: polls touch at ~30ms (responsive tab switching) and
// redraws the current page a couple of times/sec or on page change, from the
// mutex-protected snapshot. Never touches the registry, so it's independent of
// the loop's blocking BLE scan.
static void displayTask(void*) {
    bool wasDown = false;
    uint32_t lastUi = 0;
    for (;;) {
        bool redraw = false;
        guition::TouchPoint tp;
        bool down = gTouch.read(tp);
        if (down && !wasDown) {
            int t = guition::tabHitTest(tp.x, tp.y);
            if (t >= 0) {
                if ((guition::Page)t != gPage) { gPage = (guition::Page)t; redraw = true; }
            } else if (gPage == guition::PAGE_GRAPH) {
                // Tap the window pill to cycle the zoom window (1m..24h).
                if (guition::graphHitTest(tp.x, tp.y)) {
                    int n = (int)(sizeof(kGraphWins) / sizeof(kGraphWins[0]));
                    int cur = 0;
                    for (int i = 0; i < n; ++i) if (kGraphWins[i] == gGraphWinMin) cur = i;
                    gGraphWinMin = kGraphWins[(cur + 1) % n];
                    redraw = true;
                }
            } else if (gPage == guition::PAGE_SETTINGS) {
                // Settings controls. Brightness is display-owned (handle here);
                // profile switching is deferred to the loop task.
                switch (guition::settingsHitTest(tp.x, tp.y)) {
                    case guition::SET_BRIGHT_DN: {
                        int b = gDisplay.brightness() - 10; if (b < 10) b = 10;
                        gDisplay.setBrightness((uint8_t)b); redraw = true; break;
                    }
                    case guition::SET_BRIGHT_UP: {
                        int b = gDisplay.brightness() + 10; if (b > 100) b = 100;
                        gDisplay.setBrightness((uint8_t)b); redraw = true; break;
                    }
                    case guition::SET_PROFILE_NEXT:
                        gProfileNextReq = true; break;
                    default: break;
                }
            }
        }
        wasDown = down;

        uint32_t now = millis();
        if (redraw || now - lastUi >= 500) {
            lastUi = now;
            guition::DashData d;
            if (xSemaphoreTake(gDashMux, pdMS_TO_TICKS(50)) == pdTRUE) {
                d = gDash;
                xSemaphoreGive(gDashMux);
            }
            d.brightness = gDisplay.brightness();  // display-owned, not in the snapshot
            d.histWinMin = (uint16_t)gGraphWinMin;  // reflect the pill instantly; data follows
            guition::renderPage(gDisplay.canvas(), gPage, d);
            gDisplay.flush();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
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
static bool gEspNowOk = false;
static uint16_t gSnapSeq = 0;

static void setupEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        return;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcastMac, 6);
    peer.channel = 0;      // 0 = current WiFi channel (AP pinned to 1)
    peer.encrypt = false;  // broadcast can't be encrypted; telemetry only
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("ESP-NOW peer add failed");
        return;
    }
    gEspNowOk = true;
    Serial.println("ESP-NOW broadcaster ready");
}

static slavelink::Snapshot buildSnapshot() {
    using namespace slavelink;
    uint32_t now = millis();
    Snapshot s = {};
    fillHeader(s);

    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved bv = R(sig::Role::BatteryV, now);
    sig::Resolved ba = R(sig::Role::BatteryA, now);
    sig::Resolved stv = R(sig::Role::BatteryStarterV, now);
    sig::Resolved ttg = R(sig::Role::BatteryTTG, now);
    sig::Resolved sa = resolveSignal(sig::Role::SolarA, now);
    sig::Resolved cg = resolveSignal(sig::Role::ChargerA, now);
    sig::Resolved doa = resolveSignal(sig::Role::DcDcOutA, now);
    sig::Resolved la = resolveSignal(sig::Role::LoadA, now);

    switch (chargeMode(ba)) {
        case ChargeMode::Charging: s.mode = M_CHARGING; break;
        case ChargeMode::Discharging: s.mode = M_DISCHARGING; break;
        case ChargeMode::Idle: s.mode = M_IDLE; break;
        default: s.mode = M_UNKNOWN; break;
    }

    uint16_t v = 0;
    if (soc.valid) v |= V_SOC;
    if (bv.valid) v |= V_BATTV;
    if (ba.valid) v |= V_BATTA;
    if (sa.valid) v |= V_SOLAR;
    if (cg.valid) v |= V_CHARGER;
    if (doa.valid) v |= V_DCDC;
    if (la.valid) v |= V_LOAD;
    if (ttg.valid) v |= V_TTG;
    if (stv.valid) v |= V_STARTERV;
    s.valid = v;

    s.soc_d = encDeci(soc.valid, soc.value);
    s.battV_cv = encCenti(bv.valid, bv.value);
    s.battA_da = encDeci(ba.valid, ba.value);
    s.solarA_da = encDeci(sa.valid, sa.value);
    s.chargerA_da = encDeci(cg.valid, cg.value);
    s.dcdcA_da = encDeci(doa.valid, doa.value);
    s.loadA_da = encDeci(la.valid, la.value);
    s.starterV_cv = encCenti(stv.valid, stv.value);
    s.ttg_min = ttg.valid ? (uint16_t)ttg.value : 0xFFFF;

    s.alertWorst = (uint8_t)buildAlerts(now);
    s.profile = (uint8_t)gProfiles.active();
    s.seq = ++gSnapSeq;
    s.uptime_s = now / 1000;
    return s;
}

static void sendSlaveBroadcast() {
    if (!gEspNowOk) return;
    slavelink::Snapshot s = buildSnapshot();
    esp_now_send(kBroadcastMac, (const uint8_t*)&s, sizeof(s));
}

// ---- web app ---------------------------------------------------------------

static const char kStyle[] = R"CSS(
:root{--bg:#0f1720;--card:#172230;--fg:#e6edf3;--muted:#7d8da1;--line:#243140;
--accent:#22d3ee;--green:#34d399;--red:#f87171;--amber:#fbbf24}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
header{display:flex;gap:1em;align-items:center;padding:.7em 1em;background:#0b1118;
border-bottom:1px solid var(--line);position:sticky;top:0}
header h1{font-size:1em;margin:0;color:var(--accent);letter-spacing:.12em}
nav a{color:var(--muted);text-decoration:none;margin-right:1em;font-size:.95em}
nav a.active,nav a:hover{color:var(--fg)}
main{padding:1em;max-width:760px;margin:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1em;margin-bottom:1em}
h3{margin:.2em 0 .8em}
table{width:100%;border-collapse:collapse}
td,th{padding:.5em;border-bottom:1px solid var(--line);text-align:left;font-size:.92em}
button{background:var(--accent);color:#06121a;border:0;border-radius:8px;padding:.45em .8em;font-weight:600;cursor:pointer}
button.danger{background:var(--red);color:#1a0606}
button.ghost{background:transparent;color:var(--muted);border:1px solid #2c3a4a}
.winbtn.active{background:var(--accent);color:#06121a;border-color:var(--accent)}
input,select{background:#0d1620;color:var(--fg);border:1px solid #2c3a4a;border-radius:8px;padding:.45em;font-size:.92em}
label{font-size:.8em;color:var(--muted);display:block;margin-bottom:.2em}
form.inline{display:flex;gap:.6em;flex-wrap:wrap;align-items:end;margin:0}
.banner{text-align:center;font-weight:700;letter-spacing:.18em;padding:.5em;
border:1px solid #2c3a4a;border-radius:10px;margin-top:.6em;color:var(--muted)}
.legend{font-size:.8em;display:flex;gap:1em;flex-wrap:wrap;margin-top:.5em}
line{stroke-width:4;stroke-linecap:round;fill:none}
.flow{stroke-dasharray:7 7;animation:dash 1s linear infinite}
.flowrev{stroke-dasharray:7 7;animation:dashrev 1s linear infinite}
@keyframes dash{to{stroke-dashoffset:-14}}
@keyframes dashrev{to{stroke-dashoffset:14}}
svg text{fill:#e6edf3;font-family:system-ui,sans-serif}
svg text.muted{fill:var(--muted)}
.muted{color:var(--muted)}
)CSS";

static const char kMimicPage[] = R"HTML(
<div id="alerts"></div>
<div class=card>
<svg viewBox="0 0 360 350" id="mimic" style="width:100%;max-width:460px;display:block;margin:auto">
  <line id="lineSolar"   x1="62"  y1="86" x2="150" y2="152" stroke="#2c3a4a" />
  <line id="lineCharger" x1="180" y1="78" x2="180" y2="150" stroke="#2c3a4a" />
  <line id="lineDcdc"    x1="298" y1="86" x2="210" y2="152" stroke="#2c3a4a" />
  <line id="lineLoad"    x1="180" y1="244" x2="180" y2="298" stroke="#2c3a4a" />
  <rect x="150" y="150" width="60" height="92" rx="9" fill="#0d1620" stroke="#2c3a4a" stroke-width="3" />
  <rect id="fill" x="153" y="242" width="54" height="0" fill="#34d399" opacity="0.85" />
  <rect id="batt" x="150" y="150" width="60" height="92" rx="9" fill="none" stroke="#7d8da1" stroke-width="3" />
  <rect x="167" y="145" width="26" height="7" rx="2" fill="#7d8da1" />
  <text id="soc" x="180" y="202" text-anchor="middle" font-size="20" font-weight="700">--</text>
  <text x="50" y="40" text-anchor="middle" font-size="22">&#9728;&#65039;</text>
  <text x="50" y="57" text-anchor="middle" font-size="10" class="muted">Solar</text>
  <text id="solarTxt" x="50" y="76" text-anchor="middle" font-size="13">--</text>
  <text id="solarSub" x="50" y="90" text-anchor="middle" font-size="10" class="muted"></text>
  <text x="180" y="32" text-anchor="middle" font-size="22">&#128268;</text>
  <text x="180" y="49" text-anchor="middle" font-size="10" class="muted">Charger</text>
  <text id="chargerTxt" x="180" y="68" text-anchor="middle" font-size="13">--</text>
  <text x="310" y="40" text-anchor="middle" font-size="22">&#9889;</text>
  <text x="310" y="57" text-anchor="middle" font-size="10" class="muted">DC-DC</text>
  <text id="dcdcTxt" x="310" y="76" text-anchor="middle" font-size="13">--</text>
  <text id="dcdcSub" x="310" y="90" text-anchor="middle" font-size="10" class="muted"></text>
  <text x="152" y="318" text-anchor="middle" font-size="22">&#128161;</text>
  <text id="loadTxt" x="172" y="314" text-anchor="start" font-size="13">Load --</text>
  <text id="dV" x="222" y="170" text-anchor="start" font-size="13">--</text>
  <text id="dA" x="222" y="188" text-anchor="start" font-size="13">--</text>
  <text id="dAh" x="222" y="206" text-anchor="start" font-size="13" class="muted">--</text>
  <text id="dStarter" x="222" y="224" text-anchor="start" font-size="13" class="muted">--</text>
  <text id="dTTG" x="222" y="242" text-anchor="start" font-size="13" class="muted">--</text>
</svg>
<div id="modeBanner" class="banner">--</div>
</div>
<div class=card>
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:.5em">
    <h3 style="margin:0">Trend</h3>
    <div id="winbtns">
      <button class="winbtn ghost" data-m="1">1m</button>
      <button class="winbtn ghost" data-m="10">10m</button>
      <button class="winbtn ghost" data-m="60">1h</button>
      <button class="winbtn ghost" data-m="720">12h</button>
      <button class="winbtn ghost" data-m="1440">24h</button>
    </div>
  </div>
  <canvas id="chart" width="700" height="160" style="width:100%;height:160px;margin-top:.5em"></canvas>
  <div id="legend" class="legend"></div>
</div>
<script>
function set(id,t){document.getElementById(id).textContent=t;}
function setNode(id,valid,a){set(id,valid?a.toFixed(1)+'A':'--');}
function setLine(id,mode,col){var e=document.getElementById(id);e.classList.remove('flow','flowrev');
 if(!mode){e.setAttribute('stroke','#2c3a4a');return;}e.setAttribute('stroke',col);
 e.classList.add(mode==1?'flow':'flowrev');}
function ttgStr(m){
 if(m>=1440){var d=Math.floor(m/1440),h=Math.round((m%1440)/60);return d+'d'+(h?' '+h+'h':'');}
 if(m>=60){var hh=Math.floor(m/60),mm=Math.round(m%60);return hh+'h'+(mm?' '+mm+'m':'');}
 return Math.round(m)+'m';}
async function tick(){
 let p; try{p=await(await fetch('/api/panel')).json();}catch(e){return;}
 var av=document.getElementById('alerts');
 if(p.alerts&&p.alerts.length){av.innerHTML=p.alerts.map(function(a){
  var c=a.sev=='crit'?'#f87171':'#fbbf24';
  return '<div class=card style="border-color:'+c+';color:'+c+';padding:.6em 1em;margin-bottom:.6em;font-weight:600">&#9888; '+a.msg+'</div>';}).join('');}
 else av.innerHTML='';
 var b=p.battery,soc=b.valid?b.soc:0;
 set('soc',b.valid?Math.round(soc)+'%':'--');
 set('dV',b.valid?b.v.toFixed(2)+' V':'--');
 set('dA',b.valid?(b.a>=0?'+':'')+b.a.toFixed(1)+' A':'--');
 if(b.capacity>0&&b.valid){var rem=b.capacity*(b.soc/100);
  set('dAh',rem.toFixed(0)+' / '+b.capacity.toFixed(0)+' Ah');}
 else if(b.consumed_valid){set('dAh',Math.abs(b.consumed).toFixed(1)+' Ah used');}
 else set('dAh','-- Ah');
 set('dStarter',b.starter_valid?'Starter '+b.starter_v.toFixed(2)+' V':'Starter --');
 // Estimate from instantaneous current so it settles in seconds, instead of the
 // BMV's heavily-filtered (multi-minute) time-to-go. Needs a known capacity.
 if(b.capacity>0&&b.valid&&Math.abs(b.a)>0.05){
  if(b.a>0){var mf=(b.capacity*(1-b.soc/100))/b.a*60;set('dTTG','Full '+ttgStr(mf));}
  else{var me=(b.capacity*(b.soc/100))/Math.abs(b.a)*60;set('dTTG','TTG '+ttgStr(me));}}
 else if(b.ttg_valid)set('dTTG','TTG '+ttgStr(b.ttg));
 else set('dTTG','TTG ∞');
 var h=Math.max(0,Math.min(1,soc/100))*88,f=document.getElementById('fill');
 f.setAttribute('y',242-h);f.setAttribute('height',h);
 var col=p.mode=='charging'?'#34d399':p.mode=='discharging'?'#f87171':'#7d8da1';
 document.getElementById('batt').setAttribute('stroke',col);f.setAttribute('fill',col);
 var mb=document.getElementById('modeBanner');mb.textContent=p.mode.toUpperCase();
 mb.style.color=col;mb.style.borderColor=col;
 setLine('lineSolar',(p.solar.valid&&p.solar.a>0.05)?1:0,'#34d399');
 setNode('solarTxt',p.solar.valid,p.solar.a);
 set('solarSub',p.solar.valid?(p.solar.w.toFixed(0)+'W'+(p.solar.v_valid?' · '+p.solar.v.toFixed(1)+'V':'')):'');
 setLine('lineCharger',(p.charger.valid&&p.charger.a>0.05)?1:0,'#34d399');
 setNode('chargerTxt',p.charger.valid,p.charger.a);
 setLine('lineDcdc',(p.dcdc.valid&&p.dcdc.out_a>0.05)?1:0,'#34d399');
 setNode('dcdcTxt',p.dcdc.valid,p.dcdc.out_a);
 set('dcdcSub',p.dcdc.in_v_valid?('in '+p.dcdc.in_v.toFixed(1)+'V'):'');
 // Load line is driven by the load signal itself: it flows DOWN to the load
 // (amber) whenever there is load, independent of battery charge/discharge.
 var ld=p.load.valid?p.load.a:0;
 setLine('lineLoad',(p.load.valid&&ld>0.05)?1:0,'#fbbf24');
 set('loadTxt','Load '+(p.load.valid?ld.toFixed(1)+'A':'--'));
}
var SERIES=[
 {k:'battery',label:'Battery',color:'#22d3ee'},
 {k:'solar',label:'Solar',color:'#facc15'},
 {k:'charger',label:'Charger',color:'#60a5fa'},
 {k:'dcdc',label:'DC-DC',color:'#a78bfa'},
 {k:'load',label:'Load',color:'#f87171'},
 {k:'soc',label:'SoC %',color:'#f1f5f9',right:true,dash:true}
];
var hidden={};
var chartWin=10,chartData=null;
function setWin(m){chartWin=m;
 var bs=document.querySelectorAll('.winbtn');for(var i=0;i<bs.length;i++)
  bs[i].classList.toggle('active',+bs[i].dataset.m===m);
 loadChart();}
async function loadChart(){
 try{chartData=await(await fetch('/api/history?mins='+chartWin)).json();}catch(e){return;}
 drawChart();}
function drawChart(){
 var c=document.getElementById('chart');if(!c||!c.getContext||!chartData)return;
 var ctx=c.getContext('2d'),W=c.width,H=c.height,padL=40,padR=32,padT=8,padB=18;
 ctx.clearRect(0,0,W,H);
 var s=chartData.series,N=0;
 SERIES.forEach(function(se){if(s[se.k]&&s[se.k].length>N)N=s[se.k].length;});
 var mn=0,mx=0;
 SERIES.forEach(function(se){if(se.right||hidden[se.k])return;(s[se.k]||[]).forEach(function(v){
  if(v!=null){if(v<mn)mn=v;if(v>mx)mx=v;}});});
 if(mx-mn<2){mx=mn+2;}
 // Right-align by real time over the full window, so 30/60m zoom out even
 // before the buffer has that much history (data sits at the right edge).
 var interval=chartData.interval||5;
 var totalSlots=Math.max(2,Math.round(chartData.mins*60/interval));
 function Y(v){return padT+(H-padT-padB)*(1-(v-mn)/(mx-mn));}
 function Yr(v){return padT+(H-padT-padB)*(1-v/100);}
 function X(i){var frac=1-((N-1-i)/(totalSlots-1));if(frac<0)frac=0;
  return padL+(W-padL-padR)*frac;}
 // left grid + A labels: 10 gradations across the auto-scaled current range
 var divs=10,step=(mx-mn)/divs,dec=step<1?1:0;
 ctx.fillStyle='#7d8da1';ctx.font='9px system-ui';ctx.textAlign='right';
 for(var gi=0;gi<=divs;gi++){var val=mn+step*gi,y=Y(val);
  ctx.strokeStyle='#1f2c3a';ctx.lineWidth=1;
  ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();
  ctx.fillText(val.toFixed(dec)+'A',padL-4,y+3);}
 if(mn<0&&mx>0){var y0=Y(0);ctx.strokeStyle='#3a4a5c';ctx.lineWidth=1;
  ctx.beginPath();ctx.moveTo(padL,y0);ctx.lineTo(W-padR,y0);ctx.stroke();}
 // right axis labels for the SoC overlay, matching at 10% gradations
 ctx.textAlign='left';ctx.fillStyle='#9aa7b5';
 for(var pi=0;pi<=10;pi++){ctx.fillText(pi*10+'%',W-padR+4,Yr(pi*10)+3);}
 // Vertical scale marks: 1/min @10m, 1/10min @30m & 1h, 1/3h @24h.
 var stepMin=chartWin<=10?1:(chartWin<=60?10:180);
 var plot=W-padL-padR;
 ctx.font='9px system-ui';
 for(var t=stepMin;t<chartWin-0.001;t+=stepMin){
  var xx=padL+plot*(1-t/chartWin);
  ctx.strokeStyle='#243140';ctx.lineWidth=1;
  ctx.beginPath();ctx.moveTo(xx,padT);ctx.lineTo(xx,H-padB);ctx.stroke();
  if(plot*(stepMin/chartWin)>=38){ctx.fillStyle='#5b6b7d';ctx.textAlign='center';
   ctx.fillText(t<60?t+'m':(t/60)+'h',xx,H-5);}
 }
 // X end labels (oldest .. now)
 ctx.font='10px system-ui';ctx.fillStyle='#7d8da1';
 ctx.textAlign='left';ctx.fillText('-'+(chartWin>=60?chartWin/60+'h':chartWin+'m'),padL,H-5);
 ctx.textAlign='right';ctx.fillText('now',W-padR,H-5);
 // series lines (SoC uses the right 0-100% axis + a dashed stroke)
 SERIES.forEach(function(se){if(hidden[se.k])return;var a=s[se.k]||[];var yf=se.right?Yr:Y;
  ctx.strokeStyle=se.color;ctx.lineWidth=2;ctx.setLineDash(se.dash?[5,3]:[]);
  ctx.beginPath();var started=false;
  for(var i=0;i<a.length;i++){var v=a[i];if(v==null){started=false;continue;}
   var x=X(i),y=yf(v);if(started)ctx.lineTo(x,y);else{ctx.moveTo(x,y);started=true;}}
  ctx.stroke();});
 ctx.setLineDash([]);
}
function renderLegend(){
 document.getElementById('legend').innerHTML=SERIES.map(function(se){var off=hidden[se.k];
  return '<span class=legitem data-k="'+se.k+'" style="cursor:pointer;user-select:none;color:'+
   (off?'#54606e':se.color)+';'+(off?'text-decoration:line-through':'')+'">&#9632; '+se.label+
   '</span>';}).join('');
 var it=document.querySelectorAll('.legitem');
 for(var i=0;i<it.length;i++)it[i].addEventListener('click',function(){
  hidden[this.dataset.k]=!hidden[this.dataset.k];renderLegend();drawChart();});
}
renderLegend();
var wb=document.querySelectorAll('.winbtn');
for(var i=0;i<wb.length;i++)wb[i].addEventListener('click',function(){setWin(+this.dataset.m);});
setWin(10);
setInterval(tick,1000);tick();
setInterval(loadChart,5000);
</script>
)HTML";

static const char kStatsPage[] = R"HTML(
<div class=card>
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:.5em">
    <div id="scopebtns">
      <button class="winbtn ghost" data-s="today">Today</button>
      <button class="winbtn ghost" data-s="trip">Trip</button>
      <button class="winbtn ghost" data-s="total">Total</button>
    </div>
    <form id="resetForm" method=post action=/stats/reset style="margin:0">
      <input type=hidden name=scope id=resetScope value=trip>
      <button class=ghost id=resetBtn>reset</button>
    </form>
  </div>
  <div id="since" class="muted" style="font-size:.82em;margin-top:.5em">--</div>
</div>
<div class=card>
  <h3>Energy in</h3>
  <table>
    <tr><th>Source</th><th style="text-align:right">Ah</th><th style="text-align:right">Wh</th></tr>
    <tr><td>&#9728;&#65039; Solar</td><td id="s_solar_ah" style="text-align:right">--</td><td id="s_solar_wh" style="text-align:right">--</td></tr>
    <tr><td>&#9889; DC-DC</td><td id="s_dcdc_ah" style="text-align:right">--</td><td id="s_dcdc_wh" style="text-align:right">--</td></tr>
    <tr><td>&#128268; Charger</td><td id="s_charger_ah" style="text-align:right">--</td><td id="s_charger_wh" style="text-align:right">--</td></tr>
    <tr><td><b>Into battery</b></td><td id="s_charged_ah" style="text-align:right"><b>--</b></td><td id="s_charged_wh" style="text-align:right"><b>--</b></td></tr>
  </table>
</div>
<div class=card>
  <h3>Energy out</h3>
  <table>
    <tr><th>&nbsp;</th><th style="text-align:right">Ah</th><th style="text-align:right">Wh</th></tr>
    <tr><td>&#128161; Load</td><td id="s_load_ah" style="text-align:right">--</td><td id="s_load_wh" style="text-align:right">--</td></tr>
    <tr><td><b>From battery</b></td><td id="s_discharged_ah" style="text-align:right"><b>--</b></td><td id="s_discharged_wh" style="text-align:right"><b>--</b></td></tr>
  </table>
  <div id="net" class="muted" style="font-size:.82em;margin-top:.5em">--</div>
</div>
<div class=card>
  <h3>Extremes</h3>
  <table>
    <tr><td>State of charge</td><td id="s_soc" style="text-align:right">--</td></tr>
    <tr><td>Voltage</td><td id="s_v" style="text-align:right">--</td></tr>
    <tr><td>Peak solar</td><td id="s_psolar" style="text-align:right">--</td></tr>
    <tr><td>Peak load</td><td id="s_pload" style="text-align:right">--</td></tr>
    <tr><td>Peak charge / discharge</td><td id="s_pcur" style="text-align:right">--</td></tr>
    <tr><td>Time charging / discharging</td><td id="s_time" style="text-align:right">--</td></tr>
  </table>
</div>
<div class=card>
  <h3>Last 7 days</h3>
  <canvas id="dayChart" width="700" height="170" style="width:100%;height:170px"></canvas>
  <div class="legend" style="margin-top:.4em">
    <span style="color:#facc15">&#9632; Solar</span>
    <span style="color:#a78bfa">&#9632; DC-DC</span>
    <span style="color:#60a5fa">&#9632; Charger</span>
    <span style="color:#f87171">&#9632; Load</span>
    <span class=muted>Wh in (stacked) vs out</span>
  </div>
  <div id="dayEmpty" class="muted" style="font-size:.82em"></div>
</div>
<script>
function ss(id,t){document.getElementById(id).textContent=t;}
function durStr(s){s=Math.round(s);var d=Math.floor(s/86400);s-=d*86400;
 var h=Math.floor(s/3600);s-=h*3600;var m=Math.floor(s/60);
 if(d>0)return d+'d '+h+'h';if(h>0)return h+'h '+m+'m';return m+'m';}
function dt2(e){var d=new Date(e*1000);return d.toISOString().slice(0,16).replace('T',' ');}
function fmtDay(ymd){var s=''+ymd;return s.slice(4,6)+'/'+s.slice(6,8);}
function drawDays(){
 var c=document.getElementById('dayChart');if(!c||!c.getContext)return;
 var ctx=c.getContext('2d'),W=c.width,H=c.height,padL=38,padR=8,padT=8,padB=18;
 ctx.clearRect(0,0,W,H);
 var days=(data&&data.days)?data.days.slice(-7):[];
 var em=document.getElementById('dayEmpty');
 if(!days.length){em.textContent=(data&&data.clock)?
  'No completed days yet — check back after midnight.':
  'Needs an NTP clock (configure WiFi) to track daily history.';return;}
 em.textContent='';
 var mx=1;days.forEach(function(d){var i=d.solar_wh+d.dcdc_wh+d.charger_wh;
  if(i>mx)mx=i;if(d.load_wh>mx)mx=d.load_wh;});
 function Y(v){return padT+(H-padT-padB)*(1-v/mx);}
 ctx.fillStyle='#7d8da1';ctx.font='9px system-ui';ctx.textAlign='right';
 [mx,mx/2,0].forEach(function(v){var y=Y(v);ctx.strokeStyle='#1f2c3a';
  ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();
  ctx.fillText(v.toFixed(0),padL-4,y+3);});
 var n=days.length,slot=(W-padL-padR)/n,bw=slot*0.30,base=Y(0);
 ctx.textAlign='center';
 days.forEach(function(d,i){var cx=padL+slot*(i+0.5),xi=cx-bw-1,xo=cx+1,acc=0;
  [['solar_wh','#facc15'],['dcdc_wh','#a78bfa'],['charger_wh','#60a5fa']].forEach(function(p){
   var v=d[p[0]]||0;if(v<=0)return;var y0=Y(acc),y1=Y(acc+v);
   ctx.fillStyle=p[1];ctx.fillRect(xi,y1,bw,y0-y1);acc+=v;});
  ctx.fillStyle='#f87171';var yo=Y(d.load_wh);ctx.fillRect(xo,yo,bw,base-yo);
  ctx.fillStyle='#7d8da1';ctx.fillText(fmtDay(d.date),cx,H-5);});
}
var scope='trip',data=null;
function setScope(s){scope=s;document.getElementById('resetScope').value=s;
 var bs=document.querySelectorAll('.winbtn');for(var i=0;i<bs.length;i++)
  bs[i].classList.toggle('active',bs[i].dataset.s===s);render();}
async function load(){try{data=await(await fetch('/api/stats')).json();}catch(e){return;}render();drawDays();}
function render(){
 if(!data)return;var b=data[scope];if(!b)return;
 ss('s_solar_ah',b.solar_ah.toFixed(1));ss('s_solar_wh',b.solar_wh.toFixed(0));
 ss('s_dcdc_ah',b.dcdc_ah.toFixed(1));ss('s_dcdc_wh',b.dcdc_wh.toFixed(0));
 ss('s_charger_ah',b.charger_ah.toFixed(1));ss('s_charger_wh',b.charger_wh.toFixed(0));
 ss('s_charged_ah',b.charged_ah.toFixed(1));ss('s_charged_wh',b.charged_wh.toFixed(0));
 ss('s_load_ah',b.load_ah.toFixed(1));ss('s_load_wh',b.load_wh.toFixed(0));
 ss('s_discharged_ah',b.discharged_ah.toFixed(1));ss('s_discharged_wh',b.discharged_wh.toFixed(0));
 var net=b.charged_ah-b.discharged_ah;
 ss('net','Net battery balance: '+(net>=0?'+':'')+net.toFixed(1)+' Ah ('+(net>=0?'+':'')+(b.charged_wh-b.discharged_wh).toFixed(0)+' Wh)');
 ss('s_soc',b.soc_min==null?'--':b.soc_min.toFixed(0)+'% .. '+b.soc_max.toFixed(0)+'%');
 ss('s_v',b.v_min==null?'--':b.v_min.toFixed(2)+' .. '+b.v_max.toFixed(2)+' V');
 ss('s_psolar',b.peak_solar_w.toFixed(0)+' W');
 ss('s_pload',b.peak_load_w.toFixed(0)+' W');
 ss('s_pcur',b.peak_charge_a.toFixed(1)+' / '+b.peak_discharge_a.toFixed(1)+' A');
 ss('s_time',durStr(b.charge_secs)+' / '+durStr(b.discharge_secs));
 var since='Accumulated over '+durStr(b.duration_secs);
 if(data.clock&&b.start_epoch>0)since+=' &middot; since '+dt2(b.start_epoch);
 else if(scope=='today')since+=' (no clock yet — “since boot”; set WiFi for NTP)';
 document.getElementById('since').innerHTML=since;
 document.getElementById('resetBtn').textContent=scope=='total'?'reset lifetime':'reset '+scope;
}
document.getElementById('resetForm').addEventListener('submit',function(e){
 if(scope=='total'&&!confirm('Reset lifetime totals? This cannot be undone.'))e.preventDefault();});
var sb=document.querySelectorAll('.winbtn');
for(var i=0;i<sb.length;i++)sb[i].addEventListener('click',function(){setScope(this.dataset.s);});
setScope('trip');load();setInterval(load,5000);
</script>
)HTML";

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

static const char kDiagPage[] = R"HTML(
<div class=card><h3>Diagnostics</h3>
<p class=muted>Live decoded values plus the raw decrypted advertisement bytes for
each configured device &mdash; use this to confirm a parser against
VictronConnect.</p>
<div id=diag>Loading&hellip;</div></div>
<script>
function esc(s){return (s+'').replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
async function load(){let d;try{d=await(await fetch('/api/diag')).json();}catch(e){return;}
 var el=document.getElementById('diag');
 if(!d.length){el.innerHTML='<p class=muted>No devices configured.</p>';return;}
 el.innerHTML=d.map(function(dev){
  var rows=dev.fields.map(function(f){return '<tr><td class=muted>'+esc(f[0])+
   '</td><td style="text-align:right">'+esc(f[1])+'</td></tr>';}).join('');
  var st=dev.seen?(dev.stale?'<span style="color:#f87171">stale '+dev.age+'s</span>':
   '<span style="color:#34d399">live, '+dev.age+'s ago</span>'):'<span class=muted>never seen</span>';
  return '<div style="margin-bottom:1em;border-bottom:1px solid var(--line);padding-bottom:.7em">'+
   '<div style="display:flex;justify-content:space-between"><b>'+esc(dev.name)+
   '</b><span class=muted>'+esc(dev.type)+' &middot; '+esc(dev.model)+'</span></div>'+
   '<div class=muted style="font-size:.78em">'+esc(dev.mac)+' &middot; '+st+'</div>'+
   '<table>'+rows+'</table>'+
   '<div class=muted style="font-size:.74em;word-break:break-all;margin-top:.3em">raw: '+
   (dev.raw||'(none)')+'</div></div>';
 }).join('');}
load();setInterval(load,2000);
</script>
)HTML";

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

static void setupServer() {
    gServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/css", kStyle);
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

    gFine.init(gFineBuf, HIST_CAP, HIST_INTERVAL);
    gCoarse.init(gCoarseBuf, HIST2_CAP, HIST2_INTERVAL);

    gFsOk = LittleFS.begin(/*formatOnFail=*/true);
    Serial.printf("LittleFS: %s\n", gFsOk ? "mounted" : "unavailable (history not persisted)");

    gProfiles.begin();
    applyProfile(gProfiles.active());
    Serial.printf("Profile '%s': %u device(s)\n", gProfiles.name(gProfiles.active()),
                  (unsigned)gConfig.count());

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
    Serial.printf("AP '%s' up at http://%s/  (pass: %s)\n", kApSsid,
                  ip.toString().c_str(), kApPass);
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
    if (!gDisplay.begin(1 /*landscape 480x320*/)) {
        Serial.println("Display init FAILED (PSRAM/panel)");
    } else {
        Serial.printf("Display: %dx%d\n", gDisplay.width(), gDisplay.height());
        gTouch.begin(1);
        gDashMux = xSemaphoreCreateMutex();
        publishDash();  // seed the snapshot before the task starts
        // Display + touch on core 1 (runs during the loop's blocking BLE scan).
        xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, nullptr, 1);
        gDisplayOk = true;
    }
#endif
}

void loop() {
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
    static uint32_t lastBroadcast = 0;  // push live data to slaves ~1/s
    if (now - lastBroadcast >= 1000) {
        lastBroadcast = now;
        sendSlaveBroadcast();
    }
#endif

    Serial.printf("[state] victron_adverts=%d decoded=%d |", gScanVictron, gScanDecoded);
    for (size_t i = 0; i < gConfig.count(); ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
    }
    Serial.println();
}
