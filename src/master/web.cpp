// Web app: the WiFi-AP HTTP interface — HTML pages, JSON serializers and route
// handlers. Split out of main.cpp (P2). The registry/signal/stats/settings core
// stays in main.cpp; this file only presents and mutates it through the shared
// contract in app.h.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cmath>
#include <cstring>

#include "app.h"
#include "web_assets.h"  // kStyle / kMimicPage / kStatsPage / kDiagPage (HTML/CSS/JS)

// ---- small JSON helpers ----------------------------------------------------
static String jbool(bool b) { return b ? "true" : "false"; }

static String buildPanelJson() {
    // Slave role: the registry is empty (no BLE) — build the panel from the last
    // ESP-NOW frame so the mimic/dashboard show the master's live data.
    if (gRole == ROLE_SLAVE) {
        using namespace slavelink;
        const Snapshot& s = gRx.snapshot();
        // Retain the last-known frame when the link is stale (flag it) instead of
        // blanking the mimic; only blank if we've never heard the master.
        bool have = gRx.haveSnapshot();
        bool stale = !gRx.live();
        auto has = [&](uint16_t f) { return have && (s.valid & f) != 0; };
        const char* mode = !have ? "unknown"
            : (s.mode == M_CHARGING ? "charging"
             : s.mode == M_DISCHARGING ? "discharging"
             : s.mode == M_IDLE ? "idle" : "unknown");
        String j = "{";
        j += "\"stale\":" + jbool(stale) + ",";
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
             ",\"ttg_valid\":" + jbool(has(V_TTG)) +
             ",\"capacity\":" + String(s.capacityAh) + "},";  // v4: enables charging "Full"/remaining-Ah, matching the master
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
// A slave has no local stats::Stats — it receives the master's energy summary as
// an ESP-NOW StatsFrame (Ah + durations, plus per-day Ah bars). Emit the same JSON
// shape the Stats page expects; the fields the frame doesn't carry (Wh, peaks, SoC/
// V ranges, per-scope start epoch) are 0 / null (the page treats SoC as optional).
static String slaveBucketJson(const slavelink::StatMeterW& m) {
    String j = "{";
    j += "\"solar_ah\":" + String((float)m.solarAh, 1) + ",\"solar_wh\":0,";
    j += "\"dcdc_ah\":" + String((float)m.dcdcAh, 1) + ",\"dcdc_wh\":0,";
    j += "\"charger_ah\":" + String((float)m.chargerAh, 1) + ",\"charger_wh\":0,";
    j += "\"load_ah\":" + String((float)m.loadAh, 1) + ",\"load_wh\":0,";
    j += "\"charged_ah\":" + String((float)m.inAh, 1) + ",\"charged_wh\":0,";
    j += "\"discharged_ah\":" + String((float)m.outAh, 1) + ",\"discharged_wh\":0,";
    j += "\"soc_min\":null,\"soc_max\":null,\"v_min\":null,\"v_max\":null,";
    j += "\"peak_solar_w\":0,\"peak_load_w\":0,\"peak_charge_a\":0,\"peak_discharge_a\":0,";
    j += "\"charge_secs\":0,\"discharge_secs\":0,";
    j += "\"duration_secs\":" + String(m.durSecs) + ",\"start_epoch\":0";
    j += "}";
    return j;
}
static String buildSlaveStatsJson() {
    const slavelink::StatsFrame& f = gRx.stats();  // all-zero until the first frame
    uint32_t epoch = currentLocalEpoch();           // slave adopts the master's clock
    String j = "{";
    j += "\"clock\":" + jbool(f.clockOk != 0) + ",\"clock_ro\":true,\"now_epoch\":" + String(epoch) +
         ",\"run_day\":" + String(f.clockOk ? 0 : f.dayNow) + ",";
    j += "\"today\":" + slaveBucketJson(f.today) + ",";
    j += "\"trip\":"  + slaveBucketJson(f.trip)  + ",";
    j += "\"total\":" + slaveBucketJson(f.total) + ",";
    j += "\"days\":[";
    int nd = f.dayCount > 7 ? 7 : f.dayCount;
    for (int i = 0; i < nd; ++i) {
        if (i) j += ",";
        j += "{\"stamp\":" + String(f.dayStamp[i]) +
             ",\"solar_ah\":" + String((float)f.daySolarAh[i], 1) +
             ",\"dcdc_ah\":" + String((float)f.dayDcdcAh[i], 1) +
             ",\"charger_ah\":" + String((float)f.dayChargerAh[i], 1) +
             ",\"load_ah\":" + String((float)f.dayLoadAh[i], 1) +
             ",\"soc_min\":null,\"soc_max\":null}";
    }
    j += "]}";
    return j;
}
static String buildStatsJson() {
    if (gRole == ROLE_SLAVE) return buildSlaveStatsJson();
    uint32_t epoch = currentLocalEpoch();
    String j = "{";
    // clock=true when a real/manual clock is set (day labels are dates); otherwise
    // days come from the run-time odometer and run_day is the current index.
    j += "\"clock\":" + jbool(epoch != 0) + ",\"clock_ro\":false,\"now_epoch\":" + String(epoch) +
         ",\"run_day\":" + String(gStats.runDay()) + ",";
    j += "\"today\":" + bucketJson(gStats.bucket(stats::TODAY)) + ",";
    j += "\"trip\":" + bucketJson(gStats.bucket(stats::TRIP)) + ",";
    j += "\"total\":" + bucketJson(gStats.bucket(stats::TOTAL)) + ",";
    j += "\"days\":[";
    for (size_t i = 0; i < gStats.dayCount(); ++i) {
        const stats::DayRecord& d = gStats.day(i);
        if (i) j += ",";
        j += "{\"stamp\":" + String(d.dayStamp) +
             ",\"solar_ah\":" + String(d.solarAh, 1) +
             ",\"dcdc_ah\":" + String(d.dcdcAh, 1) +
             ",\"charger_ah\":" + String(d.chargerAh, 1) +
             ",\"load_ah\":" + String(d.loadAh, 1) +
             ",\"soc_min\":" + jopt(d.socMin) + ",\"soc_max\":" + jopt(d.socMax) + "}";
    }
    j += "]}";
    return j;
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
        // A slave has no BLE devices of its own — hide those pages. It DOES get the
        // energy stats from the master (ESP-NOW StatsFrame), so /stats stays. Leaves
        // the live Mimic + Stats + Settings (System card: pair / role / unpair).
        if (gRole == ROLE_SLAVE && (strcmp(l.href, "/devices") == 0 ||
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

static String apCard();
static String wifiCard();
static String profilesCard();
static String backupCard();
static String otaCard();
static String systemCard();

static String bindingsPage() {
    String h = pageHead("/bindings");
    // A slave owns no BLE devices/profiles/bindings/alerts of its own — it mirrors a
    // master over ESP-NOW. Show only what it actually controls: pair/role (system),
    // its config AP, and OTA. The master-only cards (profiles, panel signals, system
    // tunables, alerts, WiFi-join, backup) would be empty or would break the link
    // (joining a router moves the SoftAP off ch1), so they're hidden.
    if (gRole == ROLE_SLAVE) {
        h += systemCard();
        h += apCard();
        h += otaCard();
        h += pageFoot();
        return h;
    }
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
    h += apCard();
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
        RegLock lk;  // mutate the registry off the loop task's readers
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
        RegLock lk;
        gConfig.update(idx, name.c_str(), parseType(type), haveKey ? k : nullptr);
        gConfig.save();
    }
    req->redirect("/devices");
}

static void handleDel(AsyncWebServerRequest* req) {
    String name = param(req, "name");
    if (name.length()) {
        RegLock lk;
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

// Manually set the clock (UTC epoch from the browser). Lets the day rollover use
// a real calendar without NTP; RAM-only, so lost on reboot (run-days take over).
static void handleTime(AsyncWebServerRequest* req) {
    auto pv = [&](const char* k) -> String {
        if (req->hasParam(k, true)) return req->getParam(k, true)->value();   // POST body
        if (req->hasParam(k, false)) return req->getParam(k, false)->value(); // query string
        return String();
    };
    // Manual entry: h (0-23) + m — the date doesn't matter, only the time-of-day
    // (drives the midnight rollover). Anchor to a fixed UTC-midnight date and back
    // out the TZ so currentLocalEpoch() reports exactly the entered local time.
    String hh = pv("h");
    if (hh.length()) {
        int h = hh.toInt(), m = pv("m").toInt();
        if (h < 0 || h > 23 || m < 0 || m > 59) { req->send(200, "text/plain", "bad"); return; }
        const uint32_t base = 1735689600u;  // 2025-01-01 00:00 UTC
        int64_t v = (int64_t)base + h * 3600 + m * 60 - (int64_t)gTzOffsetMin * 60;
        gManualEpoch = (uint32_t)v;
        gManualMillis = millis();
        saveClock();  // persist the just-set time so a reboot keeps it
        req->send(200, "text/plain", "ok");
        return;
    }
    // Or a full UTC epoch straight from the browser clock ("Now").
    uint32_t e = (uint32_t)strtoul(pv("epoch").c_str(), nullptr, 10);
    bool ok = e > 1700000000;
    if (ok) { gManualEpoch = e; gManualMillis = millis(); saveClock(); }
    req->send(200, "text/plain", ok ? "ok" : "bad");
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

static String apCard() {
    return "<div class=card><h3>Access point</h3>"
           "<p class=muted>This master's own WiFi hotspot. Saving reboots the device "
           "&mdash; you'll need to reconnect your phone/laptop to the new network, then "
           "open <b>http://192.168.4.1/</b>.</p>"
           "<form class=inline method=post action=/apcfg>"
           "<div><label>Name (SSID)</label><input name=ssid value='" + jsEsc(String(kApSsid)) +
           "' maxlength=23 required></div>"
           "<div><label>Password (8+ chars, blank = keep)</label>"
           "<input name=pass type=password minlength=8 maxlength=23></div>"
           "<button>save &amp; reboot</button></form>"
           "<form method=post action=/apcfg style='margin-top:.6em'>"
           "<input type=hidden name=reset value=1>"
           "<button class=ghost>reset to default</button></form></div>";
}

static void handleApCfg(AsyncWebServerRequest* req) {
    if (param(req, "reset") == "1") {
        saveApCfg("", "");  // clear -> Vicmon-<mac3> / default password at next boot
    } else {
        String ssid = param(req, "ssid"), pass = param(req, "pass");
        if (ssid.length() == 0) { req->redirect("/bindings"); return; }
        if (pass.length() && pass.length() < 8) {
            req->send(200, "text/html", "AP password must be at least 8 characters. "
                                        "<a href=/bindings>back</a>");
            return;
        }
        saveApCfg(ssid, pass.length() ? pass : String(kApPass));  // blank = keep current
    }
    gRebootReq = true;  // the loop reboots (don't restart from the async task)
    req->send(200, "text/html",
              "<meta charset=utf-8><body style='font-family:system-ui;background:#0f1720;"
              "color:#e6edf3;padding:2em'><h3>Applying&hellip;</h3><p>The access point is "
              "restarting. Reconnect to the new WiFi network, then open "
              "<b>http://192.168.4.1/</b>.</p></body>");
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
    int id = param(req, "id").toInt();
    RegLock lk;  // flushes + reloads history and the whole registry
#ifndef VICMON_SIM
    saveHistFile(gProfiles.active());  // flush the outgoing profile's history first
#endif
    gProfiles.setActive(id);
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

String jsonEsc(const String& s) {  // shared: buildAlerts() in main.cpp calls it
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
        "<p class=muted>Upload a compiled <code>firmware.bin</code> (the universal build, at "
        "<code>.pio/build/s3/firmware.bin</code>) to flash over WiFi. The device "
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

void setupServer() {
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
    gServer.on("/api/time", HTTP_POST, handleTime);
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
    gServer.on("/apcfg", HTTP_POST, handleApCfg);
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
