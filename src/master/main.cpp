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
#include <WiFi.h>

#include <cstring>
#include <string>

#include "DeviceConfig.h"
#include "Registry.h"
#include "Signals.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "VictronTypes.h"

static const char* kApSsid = "Vicmon-Master";
static const char* kApPass = "vicmon1234";  // >= 8 chars; change before the field

static DeviceConfig gConfig;
static sig::SignalMap gSignals;
static NimBLEScan* gScan = nullptr;
static AsyncWebServer gServer(80);
static DNSServer gDns;

// Victron devices seen but not configured (no matching key).
struct Discovered {
    char mac[20] = {0};
    uint16_t model = 0;
    int rssi = 0;
    uint32_t lastSeenMs = 0;
};
static Discovered gDisc[12];
static size_t gDiscN = 0;

// ---- type <-> string helpers ----------------------------------------------

static victron::Record parseType(const String& t) {
    if (t == "dcdc") return victron::Record::OrionXs;
    return victron::Record::BatteryMonitor;
}
static const char* typeName(victron::Record r) {
    switch (r) {
        case victron::Record::OrionXs: return "dcdc";
        case victron::Record::BatteryMonitor: return "battery";
        default: return "?";
    }
}

// ---- BLE ingestion ---------------------------------------------------------

static void noteDiscovered(const char* mac, uint16_t model, int rssi) {
    uint32_t now = millis();
    for (size_t i = 0; i < gDiscN; ++i) {
        if (strncmp(gDisc[i].mac, mac, sizeof(gDisc[i].mac)) == 0) {
            gDisc[i].rssi = rssi;
            gDisc[i].model = model;
            gDisc[i].lastSeenMs = now;
            return;
        }
    }
    if (gDiscN < (sizeof(gDisc) / sizeof(gDisc[0]))) {
        Discovered& d = gDisc[gDiscN++];
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
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
        if (s.type == victron::Record::BatteryMonitor) {
            if (!victron::parseBatteryMonitor(out, n, s.battery)) return;
        } else if (s.type == victron::Record::OrionXs) {
            if (!victron::parseOrionXs(out, n, s.dcdc)) return;
        }
        s.everSeen = true;
        s.lastSeenMs = millis();
        return;
    }
    // No configured key matched -> a device we could adopt.
    noteDiscovered(dev->getAddress().toString().c_str(), victron::modelId(extra),
                   dev->getRSSI());
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

static String buildPanelJson() {
    uint32_t now = millis();
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved bv = R(sig::Role::BatteryV, now);
    sig::Resolved ba = R(sig::Role::BatteryA, now);
    sig::Resolved con = R(sig::Role::BatteryConsumed, now);
    sig::Resolved stv = R(sig::Role::BatteryStarterV, now);
    sig::Resolved ttg = R(sig::Role::BatteryTTG, now);
    sig::Resolved sa = R(sig::Role::SolarA, now);
    sig::Resolved sw = R(sig::Role::SolarW, now);
    sig::Resolved chg = R(sig::Role::ChargerA, now);
    sig::Resolved dia = R(sig::Role::DcDcInA, now);
    sig::Resolved doa = R(sig::Role::DcDcOutA, now);

    const char* mode = "unknown";
    if (ba.valid) mode = ba.value > 0.5f ? "charging" : (ba.value < -0.5f ? "discharging" : "idle");

    // Load: dedicated binding wins; "(derived)" computes from energy balance.
    const sig::Binding& lb = gSignals.binding(sig::Role::LoadA);
    bool loadDerivedSel = strcmp(lb.device, sig::kDerived) == 0;
    sig::Resolved la = loadDerivedSel ? sig::Resolved{} : R(sig::Role::LoadA, now);
    bool loadValid = false, loadDerived = false;
    float loadV = 0.0f;
    if (la.valid) {
        loadV = la.value;
        loadValid = true;
    } else if (loadDerivedSel && ba.valid) {
        float chargeIn = (sa.valid ? sa.value : 0.0f) + (doa.valid ? doa.value : 0.0f) +
                         (chg.valid ? chg.value : 0.0f);
        loadV = chargeIn - ba.value;  // sources - net battery (charge +)
        if (loadV < 0) loadV = 0;
        loadValid = true;
        loadDerived = true;
    }

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
         ",\"ttg\":" + String(ttg.value, 0) + ",\"ttg_valid\":" + jbool(ttg.valid) + "},";
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
    if (ba.valid) mode = ba.value > 0.5f ? "charging" : (ba.value < -0.5f ? "discharging" : "idle");
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
<script>
function set(id,t){document.getElementById(id).textContent=t;}
function setNode(id,valid,a){set(id,valid?a.toFixed(1)+'A':'--');}
function setLine(id,mode,col){var e=document.getElementById(id);e.classList.remove('flow','flowrev');
 if(!mode){e.setAttribute('stroke','#2c3a4a');return;}e.setAttribute('stroke',col);
 e.classList.add(mode==1?'flow':'flowrev');}
function ttgStr(m){return m>=6000?Math.round(m/60)+'h':Math.round(m)+'m';}
async function tick(){
 let p; try{p=await(await fetch('/api/panel')).json();}catch(e){return;}
 var b=p.battery,soc=b.valid?b.soc:0;
 set('soc',b.valid?Math.round(soc)+'%':'--');
 set('dV',b.valid?b.v.toFixed(2)+' V':'--');
 set('dA',b.valid?(b.a>=0?'+':'')+b.a.toFixed(1)+' A':'--');
 set('dAh',b.consumed_valid?b.consumed.toFixed(1)+' Ah used':'-- Ah');
 set('dStarter',b.starter_valid?'Starter '+b.starter_v.toFixed(2)+' V':'Starter --');
 set('dTTG',b.ttg_valid?'TTG '+ttgStr(b.ttg):'TTG ∞');
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
 // battery<->load line follows charge/discharge: charging pulls UP into the
 // battery (green), discharging pushes DOWN to the load (amber).
 var lm=0,lc='#fbbf24';
 if(b.valid){if(b.a>0.5){lm=2;lc='#34d399';}else if(b.a<-0.5){lm=1;lc='#fbbf24';}}
 setLine('lineLoad',lm,lc);
 set('loadTxt','Load '+(p.load.valid?p.load.a.toFixed(1)+'A':'--'));
}
setInterval(tick,1000);tick();
</script>
)HTML";

static String pageHead(const char* active) {
    String h = F("<!doctype html><html><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>Vicmon</title><link rel=stylesheet href=/style.css></head><body>"
                 "<header><h1>VICMON</h1><nav>");
    struct {
        const char* href;
        const char* name;
    } links[] = {{"/", "Mimic"}, {"/devices", "Devices"}, {"/bindings", "Signals"}};
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

static String devicesPage() {
    uint32_t now = millis();
    String h = pageHead("/devices");

    h += "<div class=card><h3>Configured devices</h3>";
    if (gConfig.count() == 0) h += "<p class=muted>None yet.</p>";
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        String state;
        if (s.stale(now)) state = "stale";
        else if (s.type == victron::Record::BatteryMonitor)
            state = String(s.battery.soc, 1) + "% " + String(s.battery.voltage, 2) + "V";
        else if (s.type == victron::Record::OrionXs)
            state = "out " + String(s.dcdc.outputVoltage, 2) + "V";
        else state = "ok";

        h += "<form class=inline method=post action=/edit style='margin-bottom:.8em'>";
        h += "<input type=hidden name=idx value=" + String(i) + ">";
        h += "<div><label>Name</label><input name=name value='" + String(s.name) + "' required></div>";
        h += "<div><label>Type</label><select name=type>";
        h += String("<option value=battery") + (s.type == victron::Record::BatteryMonitor ? " selected" : "") + ">battery</option>";
        h += String("<option value=dcdc") + (s.type == victron::Record::OrionXs ? " selected" : "") + ">dcdc</option>";
        h += "</select></div>";
        h += "<div><label>Key (blank = keep)</label><input name=key pattern='[0-9a-fA-F]{32}' size=20></div>";
        h += "<div class=muted style='align-self:center'>" + state + "</div>";
        h += "<button>save</button></form>";
        h += "<form class=inline method=post action=/del>"
             "<input type=hidden name=name value='" + String(s.name) + "'>"
             "<button class=danger>delete</button></form><hr style='border-color:#243140'>";
    }
    h += "</div>";

    // Add
    h += "<div class=card id=add><h3>Add device</h3>"
         "<form class=inline method=post action=/add>"
         "<div><label>Name</label><input name=name required></div>"
         "<div><label>Type</label><select name=type>"
         "<option value=battery>battery</option><option value=dcdc>dcdc</option></select></div>"
         "<div><label>Key (32 hex)</label><input name=key pattern='[0-9a-fA-F]{32}' size=34 required></div>"
         "<button>add</button></form></div>";

    // Discovered (unadopted)
    h += "<div class=card><h3>Discovered nearby</h3>"
         "<p class=muted>Victron devices broadcasting that aren't configured. "
         "Add one above using its encryption key from VictronConnect.</p>"
         "<table><tr><th>MAC</th><th>Model</th><th>RSSI</th></tr>";
    size_t shown = 0;
    for (size_t i = 0; i < gDiscN; ++i) {
        if (now - gDisc[i].lastSeenMs > 30000) continue;  // only recently seen
        char model[8];
        snprintf(model, sizeof(model), "0x%04X", gDisc[i].model);
        h += "<tr><td>" + String(gDisc[i].mac) + "</td><td>" + model + "</td><td>" +
             String(gDisc[i].rssi) + " dBm</td></tr>";
        ++shown;
    }
    if (shown == 0) h += "<tr><td colspan=3 class=muted>none right now</td></tr>";
    h += "</table></div>";

    h += pageFoot();
    return h;
}

static String bindingsPage() {
    String h = pageHead("/bindings");
    h += "<div class=card><h3>Panel signals</h3>"
         "<p class=muted>Tag which device field feeds each signal the mimic / "
         "display uses. Load can be set to <b>Derived</b> to compute it from the "
         "solar / charger / DC-DC inputs minus net battery current.</p>"
         "<form method=post action=/bind>";

    for (size_t r = 0; r < sig::kRoleCount; ++r) {
        sig::Role role = static_cast<sig::Role>(r);
        const sig::Binding& cur = gSignals.binding(role);
        h += "<div style='margin-bottom:.7em'><label>" + String(sig::roleLabel(role)) +
             "</label><select name=" + sig::roleKey(role) + " style='min-width:240px'>";
        h += "<option value=''>&mdash; none &mdash;</option>";
        if (role == sig::Role::LoadA) {
            bool sel = strcmp(cur.device, sig::kDerived) == 0;
            h += String("<option value='(derived)|0'") + (sel ? " selected" : "") +
                 ">Derived (computed)</option>";
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
    h += pageFoot();
    return h;
}

// ---- handlers --------------------------------------------------------------

static String param(AsyncWebServerRequest* req, const char* k) {
    return req->hasParam(k, true) ? req->getParam(k, true)->value() : String("");
}

static void handleAdd(AsyncWebServerRequest* req) {
    String name = param(req, "name"), type = param(req, "type"), key = param(req, "key");
    uint8_t k[16];
    if (name.length() && DeviceConfig::parseHexKey(key, k)) {
        gConfig.add(name.c_str(), parseType(type), k);
        gConfig.save();
    }
    req->redirect("/devices");
}

static void handleEdit(AsyncWebServerRequest* req) {
    int idx = param(req, "idx").toInt();
    String name = param(req, "name"), type = param(req, "type"), key = param(req, "key");
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
    gServer.on("/add", HTTP_POST, handleAdd);
    gServer.on("/edit", HTTP_POST, handleEdit);
    gServer.on("/del", HTTP_POST, handleDel);
    gServer.on("/bind", HTTP_POST, handleBind);
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

    gConfig.begin();
    gSignals.begin(gConfig.slots(), gConfig.count());
    Serial.printf("Loaded %u device(s) from NVS\n", (unsigned)gConfig.count());

    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/4);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP '%s' up at http://%s/  (pass: %s)\n", kApSsid,
                  ip.toString().c_str(), kApPass);

    gDns.start(53, "*", ip);
    setupServer();

    NimBLEDevice::init("");
    gScan = NimBLEDevice::getScan();
    gScan->setActiveScan(false);
    gScan->setInterval(160);
    gScan->setWindow(48);
}

void loop() {
    gDns.processNextRequest();
    pollBle();  // blocks ~2s per scan

    uint32_t now = millis();
    Serial.print("[state]");
    for (size_t i = 0; i < gConfig.count(); ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
    }
    Serial.println();
}
