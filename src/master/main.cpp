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
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

#include <cmath>
#include <cstring>
#include <string>

#include "DeviceConfig.h"
#include "Profiles.h"
#include "Registry.h"
#include "Signals.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "VictronTypes.h"

static const char* kApSsid = "Vicmon-Master";
static const char* kApPass = "vicmon1234";  // >= 8 chars; change before the field

static DeviceConfig gConfig;
static sig::SignalMap gSignals;
static ProfileManager gProfiles;
static NimBLEScan* gScan = nullptr;
static AsyncWebServer gServer(80);
static DNSServer gDns;

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

static void ingest(NimBLEAdvertisedDevice* dev) {
    if (!dev->haveManufacturerData()) return;
    std::string md = dev->getManufacturerData();
    if (md.size() < 2 + 9) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(md.data());
    if (!(p[0] == 0xE1 && p[1] == 0x02)) return;  // Victron company id

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
        return;
    }
    // No configured key matched -> a device we could adopt.
    noteDiscovered(dev->getAddress().toString().c_str(), dev->getName().c_str(),
                   victron::modelId(extra), dev->getRSSI());
}

static void pollBle() {
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

// Resolve a role honouring derived sentinels (let bat = net battery current
// (+charge/-discharge), src = measured sources):
//   (charge_only)        = max(0, bat - src)   charge not explained by sources
//   (load_only)/(derived) = bat < 0 ? (src - bat) : 0   load, 0 when charging
// Otherwise resolves the bound device field directly.
static sig::Resolved resolveSignal(sig::Role role, uint32_t now) {
    const sig::Binding& b = gSignals.binding(role);
    bool chargeOnly = strcmp(b.device, sig::kChargeOnly) == 0;
    bool load = strcmp(b.device, sig::kLoadOnly) == 0 || strcmp(b.device, sig::kDerived) == 0;
    if (chargeOnly || load) {
        sig::Resolved ba = R(sig::Role::BatteryA, now);
        sig::Resolved r;
        if (ba.valid) {
            float src = measuredSources(now);
            // Complementary halves of the energy balance: charge = bat - src,
            // load = src - bat (battery flow offset by solar + DC-DC output).
            float v = chargeOnly ? (ba.value - src) : (src - ba.value);
            r.valid = true;
            r.value = v > 0 ? v : 0;
        }
        return r;
    }
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

static const size_t HIST_CAP = 720;          // 60 min @ 5 s
static const uint32_t HIST_INTERVAL = 5000;  // ms
struct HistSample {
    int16_t battery, solar, charger, dcdc, load;  // deci-amps, -32768 = n/a
};
static HistSample gHist[HIST_CAP];
static size_t gHistHead = 0, gHistCount = 0;
static uint32_t gLastSample = 0;

static int16_t encA(bool v, float a) {
    return v ? static_cast<int16_t>(lroundf(a * 10.0f)) : -32768;
}
static int16_t sampleField(const HistSample& s, int idx) {
    switch (idx) {
        case 0: return s.battery;
        case 1: return s.solar;
        case 2: return s.charger;
        case 3: return s.dcdc;
        default: return s.load;
    }
}

// Runs continuously from loop() regardless of any connected client.
static void sampleHistory() {
    uint32_t now = millis();
    if (gHistCount > 0 && now - gLastSample < HIST_INTERVAL) return;
    gLastSample = now;
    Currents c = computeCurrents(now);
    HistSample s;
    s.battery = encA(c.bV, c.battery);
    s.solar = encA(c.sV, c.solar);
    s.charger = encA(c.cV, c.charger);
    s.dcdc = encA(c.dV, c.dcdc);
    s.load = encA(c.lV, c.load);
    gHist[gHistHead] = s;
    gHistHead = (gHistHead + 1) % HIST_CAP;
    if (gHistCount < HIST_CAP) ++gHistCount;
}

static String buildHistoryJson(int mins) {
    if (mins < 1) mins = 1;
    if (mins > 60) mins = 60;
    int want = mins * 60 * 1000 / static_cast<int>(HIST_INTERVAL);
    if (want > static_cast<int>(gHistCount)) want = gHistCount;
    if (want < 0) want = 0;
    size_t start = (gHistHead + HIST_CAP - want) % HIST_CAP;
    const char* names[5] = {"battery", "solar", "charger", "dcdc", "load"};
    String j = "{\"interval\":" + String(HIST_INTERVAL / 1000) +
               ",\"mins\":" + String(mins) + ",\"series\":{";
    for (int f = 0; f < 5; ++f) {
        j += "\"" + String(names[f]) + "\":[";
        for (int k = 0; k < want; ++k) {
            size_t idx = (start + k) % HIST_CAP;
            int16_t v = sampleField(gHist[idx], f);
            if (k) j += ",";
            j += (v == -32768) ? "null" : String(v / 10.0f, 1);
        }
        j += "]";
        if (f < 4) j += ",";
    }
    j += "}}";
    return j;
}

// ---- WiFi STA (join an existing network) -----------------------------------

static float gBattCapacity = 0;     // Ah, 0 = unknown
static float gDeadband = 0.2f;      // A; |current| below this reads as idle
static String settingsNs(int profile) {
    return profile == 0 ? String("vicset") : "vicset" + String(profile);
}
static void loadSettings(int profile) {
    Preferences p;
    p.begin(settingsNs(profile).c_str(), true);
    gBattCapacity = p.getFloat("battcap", 0);
    gDeadband = p.getFloat("deadband", 0.2f);
    p.end();
}
static void saveSettings(float capacity, float deadband) {
    Preferences p;
    p.begin(settingsNs(gProfiles.active()).c_str(), false);
    p.putFloat("battcap", capacity);
    p.putFloat("deadband", deadband);
    p.end();
    gBattCapacity = capacity;
    gDeadband = deadband;
}

// Loads a profile's config/signals/settings and clears runtime caches so the
// mimic, history and discovery don't mix data across profiles.
static void applyProfile(int pid) {
    gConfig.begin(pid);
    gSignals.begin(gConfig.slots(), gConfig.count(), pid);
    loadSettings(pid);
    gDiscN = 0;
    gHistCount = 0;
    gHistHead = 0;
    gLastSample = 0;
}

// Erases a profile's persisted data (devices / signals / settings).
static void wipeProfile(int pid) {
    String dns = pid == 0 ? String("vicmon") : "vicmon" + String(pid);
    String sns = pid == 0 ? String("vicsig2") : "vicsig2_" + String(pid);
    String tns = settingsNs(pid);
    Preferences p;
    p.begin(dns.c_str(), false); p.clear(); p.end();
    p.begin(sns.c_str(), false); p.clear(); p.end();
    p.begin(tns.c_str(), false); p.clear(); p.end();
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

    const char* mode = "unknown";
    if (ba.valid) mode = ba.value > gDeadband ? "charging" : (ba.value < -gDeadband ? "discharging" : "idle");

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
    j += "\"solar\":{\"valid\":" + jbool(sa.valid) +
         ",\"a\":" + String(sa.value, 1) +
         ",\"w\":" + String(sw.value, 0) + "},";
    j += "\"charger\":{\"valid\":" + jbool(chg.valid) +
         ",\"a\":" + String(chg.value, 1) + "},";
    j += "\"dcdc\":{\"valid\":" + jbool(dcdcValid) +
         ",\"out_a\":" + String(doa.value, 1) +
         ",\"in_a\":" + String(dia.value, 1) + "},";
    j += "\"load\":{\"valid\":" + jbool(loadValid) +
         ",\"a\":" + String(loadV, 1) +
         ",\"derived\":" + jbool(loadDerived) + "}";
    j += "}";
    return j;
}

// Legacy snapshot used by slaves (Phase 4 HTTP fallback).
static String buildDataJson() {
    uint32_t now = millis();
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved ba = R(sig::Role::BatteryA, now);
    const char* mode = "unknown";
    if (ba.valid) mode = ba.value > gDeadband ? "charging" : (ba.value < -gDeadband ? "discharging" : "idle");
    String j = "{";
    j += "\"system_status\":\"" + String(mode) + "\",";
    j += "\"battery_soc\":" + String(soc.value, 1) + ",";
    j += "\"battery_current\":" + String(ba.value, 2) + ",";
    j += "\"timestamp\":" + String(now / 1000);
    j += "}";
    return j;
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
  <text x="180" y="32" text-anchor="middle" font-size="22">&#128268;</text>
  <text x="180" y="49" text-anchor="middle" font-size="10" class="muted">Charger</text>
  <text id="chargerTxt" x="180" y="68" text-anchor="middle" font-size="13">--</text>
  <text x="310" y="40" text-anchor="middle" font-size="22">&#9889;</text>
  <text x="310" y="57" text-anchor="middle" font-size="10" class="muted">DC-DC</text>
  <text id="dcdcTxt" x="310" y="76" text-anchor="middle" font-size="13">--</text>
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
      <button class="winbtn ghost" data-m="30">30m</button>
      <button class="winbtn ghost" data-m="60">60m</button>
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
 setLine('lineCharger',(p.charger.valid&&p.charger.a>0.05)?1:0,'#34d399');
 setNode('chargerTxt',p.charger.valid,p.charger.a);
 setLine('lineDcdc',(p.dcdc.valid&&p.dcdc.out_a>0.05)?1:0,'#34d399');
 setNode('dcdcTxt',p.dcdc.valid,p.dcdc.out_a);
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
 {k:'load',label:'Load',color:'#f87171'}
];
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
 var ctx=c.getContext('2d'),W=c.width,H=c.height,padL=34,padR=8,padT=8,padB=18;
 ctx.clearRect(0,0,W,H);
 var s=chartData.series,N=0;
 SERIES.forEach(function(se){if(s[se.k]&&s[se.k].length>N)N=s[se.k].length;});
 var mn=0,mx=0;
 SERIES.forEach(function(se){(s[se.k]||[]).forEach(function(v){
  if(v!=null){if(v<mn)mn=v;if(v>mx)mx=v;}});});
 if(mx-mn<2){mx=mn+2;}
 // Right-align by real time over the full window, so 30/60m zoom out even
 // before the buffer has that much history (data sits at the right edge).
 var interval=chartData.interval||5;
 var totalSlots=Math.max(2,Math.round(chartData.mins*60/interval));
 function Y(v){return padT+(H-padT-padB)*(1-(v-mn)/(mx-mn));}
 function X(i){var frac=1-((N-1-i)/(totalSlots-1));if(frac<0)frac=0;
  return padL+(W-padL-padR)*frac;}
 // grid + Y labels (max, 0, min)
 ctx.fillStyle='#7d8da1';ctx.font='10px system-ui';ctx.textAlign='right';
 [mx,0,mn].forEach(function(v){var y=Y(v);
  ctx.strokeStyle=v===0?'#3a4a5c':'#1f2c3a';ctx.lineWidth=1;
  ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();
  ctx.fillText(v.toFixed(0)+'A',padL-4,y+3);});
 // X labels (oldest .. now)
 ctx.textAlign='left';ctx.fillText('-'+chartWin+'m',padL,H-5);
 ctx.textAlign='right';ctx.fillText('now',W-padR,H-5);
 // series lines
 SERIES.forEach(function(se){var a=s[se.k]||[];
  ctx.strokeStyle=se.color;ctx.lineWidth=2;ctx.beginPath();var started=false;
  for(var i=0;i<a.length;i++){var v=a[i];if(v==null){started=false;continue;}
   var x=X(i),y=Y(v);if(started)ctx.lineTo(x,y);else{ctx.moveTo(x,y);started=true;}}
  ctx.stroke();});
}
document.getElementById('legend').innerHTML=SERIES.map(function(se){
 return '<span style="color:'+se.color+'">&#9632; '+se.label+'</span>';}).join('');
var wb=document.querySelectorAll('.winbtn');
for(var i=0;i<wb.length;i++)wb[i].addEventListener('click',function(){setWin(+this.dataset.m);});
setWin(10);
setInterval(tick,1000);tick();
setInterval(loadChart,5000);
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
    } links[] = {{"/", "Mimic"}, {"/devices", "Devices"}, {"/bindings", "Settings"}};
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
        char model[8];
        snprintf(model, sizeof(model), "0x%04X", gDisc[i].model);
        String nm = gDisc[i].name[0] ? String(gDisc[i].name) : String("(unnamed)");
        h += "<tr><td>" + nm + "</td><td>" + String(gDisc[i].mac) + "</td><td>" + model +
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
         "<button>save</button></form>"
         "<p class=muted>Capacity shows remaining Ah on the mimic. Currents within "
         "&plusmn;deadband read as <i>idle</i>.</p></div>";

    h += wifiCard();
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
    saveSettings(c, db);
    req->redirect("/bindings");
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
    gServer.on("/api/panel", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildPanelJson());
    });
    gServer.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildDataJson());
    });
    gServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
        int mins = req->hasParam("mins") ? req->getParam("mins")->value().toInt() : 10;
        req->send(200, "application/json", buildHistoryJson(mins));
    });
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
    gServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(200, "text/html", pageHead("/") + kMimicPage + pageFoot());
    });
    gServer.begin();
}

// ---- Arduino entry points --------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nVicmon Master (headless): BLE + WiFi AP");

    gProfiles.begin();
    applyProfile(gProfiles.active());
    Serial.printf("Profile '%s': %u device(s)\n", gProfiles.name(gProfiles.active()),
                  (unsigned)gConfig.count());

    loadWifi();
    WiFi.mode(gStaSsid.length() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(kApSsid, kApPass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/4);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP '%s' up at http://%s/  (pass: %s)\n", kApSsid,
                  ip.toString().c_str(), kApPass);
    if (gStaSsid.length()) {
        WiFi.begin(gStaSsid.c_str(), gStaPass.c_str());
        Serial.printf("Joining WiFi '%s'...\n", gStaSsid.c_str());
    }

    gDns.start(53, "*", ip);
    setupServer();

    NimBLEDevice::init("");
    gScan = NimBLEDevice::getScan();
    gScan->setActiveScan(false);
    // Keep BLE duty cycle low so the WiFi AP gets enough radio airtime to stay
    // joinable (window/interval ~= 30%). Victron advertises ~1/s.
    gScan->setInterval(160);
    gScan->setWindow(48);
}

void loop() {
    gDns.processNextRequest();
    pollBle();  // blocks ~2s per scan
    sampleHistory();  // continuous logging, regardless of any connected client

    uint32_t now = millis();
    Serial.print("[state]");
    for (size_t i = 0; i < gConfig.count(); ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
    }
    Serial.println();
}
