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
#include <memory>
#include <vector>

#include "app.h"
#include "web_assets.h"  // kStyle / kMimicPage / kStatsPage / kDiagPage (HTML/CSS/JS)

// ---- small JSON helpers ----------------------------------------------------
static String jbool(bool b) { return b ? "true" : "false"; }

static void jf(OutSink& o, float v, int dp) { char b[24]; dtostrf(v, 0, dp, b); o.put(b, strlen(b)); }
static void joptf(OutSink& o, float v) { if (isnan(v)) o.put("null", 4); else jf(o, v, 2); }
static void jb(OutSink& o, bool v) { if (v) o.put("true", 4); else o.put("false", 5); }
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
        // Environment mirrored from the master's sensor. "present" follows the
        // validity bits — a slave has no way to tell "no sensor fitted" from
        // "sensor not reading yet", and either way there is nothing to show.
        j += "\"env\":{\"present\":" + jbool(has(V_ENV)) +
             ",\"valid\":" + jbool(has(V_ENV)) +
             ",\"temp\":" + String(decDeci(s.envTemp_dc), 1) +
             ",\"humidity\":" + String(decDeci(s.envHum_dp), 1) +
             ",\"pressure\":" + String(s.envPress_dhpa / 10.0f, 1) +
             ",\"gas\":" + String(has(V_ENVGAS) ? s.envGas_kohm : 0) +
             ",\"gas_valid\":" + jbool(has(V_ENVGAS)) + "},";
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
    {
        // Environment (Unit ENV Pro / BME688). "present" tells the UI whether to
        // show the card at all; "valid"/"gas_valid" whether the numbers are good.
        const EnvReading& e = envReading();
        j += "\"env\":{\"present\":" + jbool(envPresent()) +
             ",\"valid\":" + jbool(e.valid) +
             ",\"temp\":" + String(e.tempC, 1) +
             ",\"humidity\":" + String(e.humidity, 1) +
             ",\"pressure\":" + String(e.pressureHpa, 1) +
             ",\"gas\":" + String(e.gasOhm / 1000.0f, 1) +
             ",\"gas_valid\":" + jbool(e.gasValid) + "},";
    }
    String alerts;
    buildAlerts(now, &alerts);
    j += "\"alerts\":" + alerts;
    j += "}";
    return j;
}

// Legacy snapshot used by slaves (Phase 4 HTTP fallback).
static void dataInto(OutSink& o) {
    uint32_t now = millis();
    sig::Resolved soc = R(sig::Role::BatterySOC, now);
    sig::Resolved ba = R(sig::Role::BatteryA, now);
    o.f("{\"system_status\":\"%s\",\"battery_soc\":", chargeModeName(chargeMode(ba))); jf(o, soc.value, 1);
    o.put(",\"battery_current\":",19); jf(o, ba.value, 2);
    o.f(",\"timestamp\":%lu}", (unsigned long)(now / 1000));
}

static String jopt(float v) {  // NAN -> null
    return isnan(v) ? String("null") : String(v, 2);
}
static void bucketInto(OutSink& o, const stats::Bucket& b) {
    o.put("{\"solar_ah\":",12); jf(o,b.solarAh,1); o.put(",\"solar_wh\":",12); jf(o,b.solarWh,0);
    o.put(",\"dcdc_ah\":",11); jf(o,b.dcdcAh,1); o.put(",\"dcdc_wh\":",11); jf(o,b.dcdcWh,0);
    o.put(",\"charger_ah\":",14); jf(o,b.chargerAh,1); o.put(",\"charger_wh\":",14); jf(o,b.chargerWh,0);
    o.put(",\"load_ah\":",11); jf(o,b.loadAh,1); o.put(",\"load_wh\":",11); jf(o,b.loadWh,0);
    o.put(",\"charged_ah\":",14); jf(o,b.chargedAh,1); o.put(",\"charged_wh\":",14); jf(o,b.chargedWh,0);
    o.put(",\"discharged_ah\":",17); jf(o,b.dischargedAh,1); o.put(",\"discharged_wh\":",17); jf(o,b.dischargedWh,0);
    o.put(",\"soc_min\":",11); joptf(o,b.socMin); o.put(",\"soc_max\":",11); joptf(o,b.socMax);
    o.put(",\"v_min\":",9); joptf(o,b.vMin); o.put(",\"v_max\":",9); joptf(o,b.vMax);
    o.put(",\"peak_solar_w\":",16); jf(o,b.peakSolarW,0); o.put(",\"peak_load_w\":",15); jf(o,b.peakLoadW,0);
    o.put(",\"peak_charge_a\":",17); jf(o,b.peakChargeA,1); o.put(",\"peak_discharge_a\":",20); jf(o,b.peakDischargeA,1);
    o.f(",\"charge_secs\":%lu,\"discharge_secs\":%lu,\"duration_secs\":%lu,\"start_epoch\":%lu}",
        (unsigned long)b.chargeSecs,(unsigned long)b.dischargeSecs,(unsigned long)b.durationSecs,(unsigned long)b.startEpoch);
}
// A slave has no local stats::Stats — it receives the master's energy summary as
// an ESP-NOW StatsFrame (Ah + durations, plus per-day Ah bars). Emit the same JSON
// shape the Stats page expects; the fields the frame doesn't carry (Wh, peaks, SoC/
// V ranges, per-scope start epoch) are 0 / null (the page treats SoC as optional).
static void slaveBucketInto(OutSink& o, const slavelink::StatMeterW& m) {
    o.f("{\"solar_ah\":%lu.0,\"solar_wh\":0,\"dcdc_ah\":%lu.0,\"dcdc_wh\":0,\"charger_ah\":%lu.0,\"charger_wh\":0,"
        "\"load_ah\":%lu.0,\"load_wh\":0,\"charged_ah\":%lu.0,\"charged_wh\":0,\"discharged_ah\":%lu.0,\"discharged_wh\":0,",
        (unsigned long)m.solarAh,(unsigned long)m.dcdcAh,(unsigned long)m.chargerAh,(unsigned long)m.loadAh,(unsigned long)m.inAh,(unsigned long)m.outAh);
    o.f("\"soc_min\":null,\"soc_max\":null,\"v_min\":null,\"v_max\":null,\"peak_solar_w\":0,\"peak_load_w\":0,"
        "\"peak_charge_a\":0,\"peak_discharge_a\":0,\"charge_secs\":0,\"discharge_secs\":0,\"duration_secs\":%lu,\"start_epoch\":0}",
        (unsigned long)m.durSecs);
}
static void slaveStatsInto(OutSink& o) {
    const slavelink::StatsFrame& f = gRx.stats();  // all-zero until the first frame
    uint32_t epoch = currentLocalEpoch();           // slave adopts the master's clock
    o.f("{\"clock\":%s,\"clock_ro\":true,\"now_epoch\":%lu,\"run_day\":%lu,\"today\":",
        f.clockOk ? "true" : "false", (unsigned long)epoch, (unsigned long)(f.clockOk ? 0 : f.dayNow));
    slaveBucketInto(o, f.today); o.put(",\"trip\":",8); slaveBucketInto(o, f.trip);
    o.put(",\"total\":",9); slaveBucketInto(o, f.total); o.put(",\"days\":[",9);
    int nd = f.dayCount > 7 ? 7 : f.dayCount;
    for (int i = 0; i < nd; ++i)
        o.f("%s{\"stamp\":%lu,\"solar_ah\":%u.0,\"dcdc_ah\":%u.0,\"charger_ah\":%u.0,\"load_ah\":%u.0,\"soc_min\":null,\"soc_max\":null}",
            i ? "," : "", (unsigned long)f.dayStamp[i], (unsigned)f.daySolarAh[i], (unsigned)f.dayDcdcAh[i],
            (unsigned)f.dayChargerAh[i], (unsigned)f.dayLoadAh[i]);
    o.put("]}",2);
}
static void statsInto(OutSink& o) {
    if (gRole == ROLE_SLAVE) { slaveStatsInto(o); return; }
    uint32_t epoch = currentLocalEpoch();
    // clock=true when a real/manual clock is set (day labels are dates); otherwise
    // days come from the run-time odometer and run_day is the current index.
    o.f("{\"clock\":%s,\"clock_ro\":false,\"now_epoch\":%lu,\"run_day\":%lu,\"today\":",
        epoch ? "true" : "false", (unsigned long)epoch, (unsigned long)gStats.runDay());
    bucketInto(o, gStats.bucket(stats::TODAY)); o.put(",\"trip\":",8); bucketInto(o, gStats.bucket(stats::TRIP));
    o.put(",\"total\":",9); bucketInto(o, gStats.bucket(stats::TOTAL)); o.put(",\"days\":[",9);
    for (size_t i = 0; i < gStats.dayCount(); ++i) {
        const stats::DayRecord& d = gStats.day(i);
        o.f("%s{\"stamp\":%lu,\"solar_ah\":", i ? "," : "", (unsigned long)d.dayStamp); jf(o,d.solarAh,1);
        o.put(",\"dcdc_ah\":",11); jf(o,d.dcdcAh,1); o.put(",\"charger_ah\":",14); jf(o,d.chargerAh,1);
        o.put(",\"load_ah\":",11); jf(o,d.loadAh,1); o.put(",\"charged_ah\":",14); jf(o,d.chargedAh,1);
        o.put(",\"discharged_ah\":",17); jf(o,d.dischargedAh,1);
        o.put(",\"soc_min\":",11); joptf(o,d.socMin); o.put(",\"soc_max\":",11); joptf(o,d.socMax); o.put("}",1);
    }
    o.put("]}",2);
}

// ---- web app ---------------------------------------------------------------




// Lowest free heap seen on the web path since the last reset of the counter
// (GET /api/sys?reset=1). ESP.getMinFreeHeap() is since boot and cannot be
// reset, which makes per-request measurement impossible without rebooting.
static uint32_t gWebMinFree = 0xFFFFFFFF;
static inline void webSample() { uint32_t f = ESP.getFreeHeap(); if (f < gWebMinFree) gWebMinFree = f; }

static String pageHead(const char* active) {
    String h = F("<!doctype html><html><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>Vicmon</title><link rel=stylesheet href=/style.css></head><body>"
                 "<header><div class=hrow><h1>VICMON</h1>");
    // Top row: title + profile (left) and the live clock (right). Tabs go on row 2.
    h += "<span class=muted style='font-size:.8em'>" + String(gProfiles.name(gProfiles.active())) +
         "</span><span id=clk></span></div><nav>";
    struct {
        const char* href;
        const char* name;
    } links[] = {{"/", "Mimic"}, {"/stats", "Stats"}, {"/devices", "Devices"},
                 {"/bindings", "Settings"}, {"/network", "Network"}, {"/diag", "Diag"}};
    for (auto& l : links) {
        // A slave has no BLE devices of its own — hide those pages. It DOES get the
        // energy stats from the master (ESP-NOW StatsFrame), so /stats stays. Leaves
        // the live Mimic + Stats + Settings (System card: pair / role / unpair).
        // A slave owns no devices/bindings of its own; what it DOES control
        // (pair, role, its AP, OTA) all lives on /network.
        if (gRole == ROLE_SLAVE && (strcmp(l.href, "/devices") == 0 ||
                                    strcmp(l.href, "/bindings") == 0 ||
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
    // Live clock in the header, on every page: seed the offset from the device's
    // own time (RTC/NTP/manual via GET /api/time), then tick locally and re-sync
    // each minute so it stays right through an NTP/RTC correction. now_epoch is the
    // device LOCAL epoch (TZ already applied), so it's read with getUTC* methods.
    h += F("</nav>"
           "<script>(function(){var off=0,ok=false,synced=false,"
           "D=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'],"
           "M=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];"
           "function p(n){return(n<10?'0':'')+n;}"
           "function sync(){fetch('/api/time').then(function(r){return r.json();}).then(function(t){"
           "synced=true;if(t.epoch>0){off=t.epoch*1000-Date.now();ok=true;}else{ok=false;}}).catch(function(){});}"
           "function tick(){var e=document.getElementById('clk');if(!e)return;"
           "if(!ok){e.textContent=synced?'no clock set':'';return;}"
           "var d=new Date(Date.now()+off);"
           "e.textContent=D[d.getUTCDay()]+' '+d.getUTCDate()+' '+M[d.getUTCMonth()]+' '+d.getUTCFullYear()"
           "+' \\u00b7 '+p(d.getUTCHours())+':'+p(d.getUTCMinutes())+':'+p(d.getUTCSeconds());}"
           "sync();tick();setInterval(tick,1000);setInterval(sync,60000);})();</script>"
           "</header><main>");
    return h;
}
static String pageFoot() { return F("</main></body></html>"); }

// ---- static page cache -------------------------------------------------------
// The three literal pages (/, /stats, /diag) are head + flash body + foot. The
// head depends only on the role (fixed at boot) and the profile name, so ONE
// copy is rendered at boot into a fixed buffer and re-rendered on a profile
// switch or rename; which tab is highlighted is set by a one-line script from
// location.pathname. Serving a static page is three segments and no build: it
// never touches an arena, so a page load and the API poll a browser fires
// alongside it cannot contend.
static const char* const kFootLit = "</main></body></html>";
static const size_t kHeadCap = 1536;
static char gStaticHead[kHeadCap];
static size_t gStaticHeadLen = 0;
static int gStaticProfile = -1;
static bool gStaticDirty = false;
static int gStaticInflight = 0;
static const char* gStaticBodies[3] = {nullptr, nullptr, nullptr};
static void staticRender() {
    String h = pageHead("");   // the one remaining String here: at boot / profile change only
    h += F("<script>(function(){var a=document.querySelector(\"nav a[href='\"+location.pathname+\"']\");if(a)a.className='active';})();</script>");
    if (h.length() >= kHeadCap) {  // never truncate silently: a first cut at 1280 bytes clipped the clock script
        Serial.printf("[web] static head %u bytes exceeds %u — NOT cached\n", (unsigned)h.length(), (unsigned)kHeadCap);
        gStaticHeadLen = 0; return;
    }
    memcpy(gStaticHead, h.c_str(), h.length()); gStaticHeadLen = h.length();
    gStaticProfile = gProfiles.active(); gStaticDirty = false;
    Serial.printf("[web] static head cached: %u bytes\n", (unsigned)gStaticHeadLen);
}
void webRenderStaticPages() {
    gStaticBodies[0] = kMimicPage; gStaticBodies[1] = kStatsPage; gStaticBodies[2] = kDiagPage;
    if (gStaticInflight == 0) staticRender(); else gStaticDirty = true;
}
static void serveStaticCached(AsyncWebServerRequest* req, int idx) {
    if ((gStaticDirty || gStaticProfile != gProfiles.active()) && gStaticInflight == 0) staticRender();
    if (!gStaticHeadLen) { req->send(507, "text/plain", "page head not cached"); return; }
    const char* body = gStaticBodies[idx];
    ++gStaticInflight;
    std::shared_ptr<void> token(nullptr, [](void*) { --gStaticInflight; });
    const size_t hl = gStaticHeadLen, bodyLen = strlen(body), footLen = strlen(kFootLit);
    req->send(req->beginChunkedResponse("text/html",
        [token, body, hl, bodyLen, footLen](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            webSample();
            const char* src; size_t avail;
            if (index < hl)                          { src = gStaticHead + index; avail = hl - index; }
            else if (index < hl + bodyLen)           { src = body + (index - hl); avail = hl + bodyLen - index; }
            else if (index < hl + bodyLen + footLen) { src = kFootLit + (index - hl - bodyLen); avail = hl + bodyLen + footLen - index; }
            else return 0;
            const size_t n = maxLen < avail ? maxLen : avail;
            memcpy(buf, src, n);
            return n;
        }));
}
// Generated pages use the same cached head as a RAM segment (no build, no copy).
static void headInto(struct HtmlOut& h);

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

// Collects HTML into a list of SMALL pieces (each ~1.4 KB) that a chunked response
// then streams through a tiny buffer (see servePieces). The large master pages
// (Settings / Devices) must never be one big contiguous String on the no-PSRAM
// M5Capsule: its heap is only ~58 KB free / ~47 KB largest block, so a whole-page
// String (+ the copy req->send(String) makes) either came back BLANK or, when
// pre-allocated big, crashed the AsyncTCP task and dropped WiFi. Pieces stay small
// and non-contiguous, so they fit the fragmented heap and need no copy.
// ---- boot-allocated build arenas --------------------------------------------
// Every page and every API body is built into a fixed buffer allocated at
// boot, with a fixed segment table whose entries point either straight into
// flash (literals, never copied) or into the arena (generated text). Nothing in
// this path is allocated per request: no vector, no retained String, no new.
//
// One 10KB arena: generated pages (<= ~4.2KB), the history JSON (<= ~7.2KB at
// 100 columns) and the API bodies (<= ~3.5KB). A second 5KB arena for the APIs
// was tried to keep a page and its poll from waiting on each other; measured,
// contention was gone once the static pages stopped building, and its 5KB
// was worth more back in the pool. Taken LAST in setup() so the big early WiFi/NimBLE blocks stay
// contiguous (largest free block at boot: 53,236 none / 32,756 carved first /
// 40,948 carved last). One build per arena at a time; the mutex is held until
// the response has fully drained, and a second taker waits up to 50ms.
struct Seg { const char* p = nullptr; size_t len = 0; };
struct Arena {
    const char* name; size_t size; Seg* segs; int maxSegs;
    uint8_t* buf = nullptr; int n = 0; size_t used = 0, high = 0; bool overflow = false;
    SemaphoreHandle_t mux = nullptr;
    void init() {
        buf = static_cast<uint8_t*>(malloc(size)); mux = xSemaphoreCreateMutex();
        Serial.printf("[mem] %s arena %uB: %s\n", name, (unsigned)size, (buf && mux) ? "ok" : "FAILED");
    }
    bool take() {
        if (!buf || !mux) return false;
        if (xSemaphoreTake(mux, pdMS_TO_TICKS(50)) != pdTRUE) { Serial.printf("[arena] %s busy\n", name); return false; }
        n = 0; used = 0; overflow = false; return true;
    }
    void give() { if (mux) xSemaphoreGive(mux); }
};
static const size_t kArenaSize = 10 * 1024;  // history <= ~7.2KB at 100 cols; largest generated page ~4.2KB
static Seg gPageSegs[160];   // segment table: static, allocated at start-up
static Arena gPageArena{"page", kArenaSize, gPageSegs, 160};

// Aliases so the page-arena diagnostics (webtest, /api/sys) read naturally.
static int& gSegN = gPageArena.n;
static Seg* const gSegs = gPageSegs;
static size_t& gArenaUsed = gPageArena.used;
static size_t& gArenaHigh = gPageArena.high;
static bool& gArenaOverflow = gPageArena.overflow;

void webPreallocate() {
    gPageArena.init();
    webRenderStaticPages();
}
static bool arenaTake() { return gPageArena.take(); }
static void arenaGive() { gPageArena.give(); }

struct HtmlOut : OutSink {
    Arena& a;
    explicit HtmlOut(Arena& ar = gPageArena) : a(ar) {}
    static const size_t kMinLit = 48;  // shorter literals are copied; a segment costs a table slot
    void put(const char* src, size_t n) override {
        if (a.overflow) return;
        if (a.used + n > a.size) { a.overflow = true; return; }
        const char* tailEnd = reinterpret_cast<char*>(a.buf) + a.used;
        Seg* tail = (a.n && a.segs[a.n - 1].p + a.segs[a.n - 1].len == tailEnd) ? &a.segs[a.n - 1] : nullptr;
        if (!tail) {
            if (a.n >= a.maxSegs) { a.overflow = true; return; }
            tail = &a.segs[a.n++]; tail->p = tailEnd; tail->len = 0;
        }
        memcpy(a.buf + a.used, src, n);
        tail->len += n; a.used += n;
        if (a.used > a.high) a.high = a.used;
    }
    char* reserve(size_t n) override {   // f()'s fast path: format straight into the arena tail
        if (a.overflow || a.used + n + 1 > a.size) return nullptr;
        return reinterpret_cast<char*>(a.buf) + a.used;
    }
    void commit(size_t n) override {     // the bytes are already in place; account for them like put()
        const char* tailEnd = reinterpret_cast<char*>(a.buf) + a.used;
        Seg* tail = (a.n && a.segs[a.n - 1].p + a.segs[a.n - 1].len == tailEnd) ? &a.segs[a.n - 1] : nullptr;
        if (!tail) { if (a.n >= a.maxSegs) { a.overflow = true; return; } tail = &a.segs[a.n++]; tail->p = tailEnd; tail->len = 0; }
        tail->len += n; a.used += n; if (a.used > a.high) a.high = a.used;
    }
    void lit(const char* p, size_t n) {   // flash literal: reference, never copy
        if (a.overflow) return;
        if (a.n >= a.maxSegs) { a.overflow = true; return; }
        a.segs[a.n].p = p; a.segs[a.n].len = n; ++a.n;
    }
    HtmlOut& operator+=(const String& s) { put(s.c_str(), s.length()); return *this; }
    HtmlOut& operator+=(const char* s) { size_t n = strlen(s); if (n < kMinLit) put(s, n); else lit(s, n); return *this; }
    HtmlOut& operator+=(const __FlashStringHelper* f) { return operator+=(reinterpret_cast<const char*>(f)); }
    void flush() {}
};
// printf straight into whichever sink — the allocation-free way to emit numbers.
void OutSink::f(const char* fmt, ...) {
    // A first version formatted into a 96-byte stack buffer and silently
    // truncated anything longer, which broke the diag JSON at character 95 and
    // left the stats bucket line one wide number away from the same. Size the
    // output first; short goes via the stack, long is written straight into the
    // sink's tail when it offers one, and otherwise through a temporary.
    char b[96]; va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if (static_cast<size_t>(n) < sizeof(b)) { put(b, n); va_end(ap2); return; }
    char* dst = reserve(n);
    if (dst) { vsnprintf(dst, n + 1, fmt, ap2); commit(n); }
    else { char* t = static_cast<char*>(malloc(n + 1)); if (t) { vsnprintf(t, n + 1, fmt, ap2); put(t, n); free(t); } }
    va_end(ap2);
}

static void headInto(HtmlOut& h) {
    if ((gStaticDirty || gStaticProfile != gProfiles.active()) && gStaticInflight == 0) staticRender();
    if (gStaticHeadLen) h.lit(gStaticHead, gStaticHeadLen);   // lit() just references memory; RAM works as well as flash
    else h += pageHead("");
}

// Stream an arena's segment table. The token's deleter releases the arena when
// the response is destroyed, i.e. after the last chunk has gone out.
static void serveArena(AsyncWebServerRequest* req, const char* type, Arena& a = gPageArena) {
    if (a.overflow) {
        a.give();
        req->send(507, "text/plain", "response exceeds build arena");
        return;
    }
    // The arena is needed only until the library has copied the last byte into
    // its own send buffer — not until the client closes the socket, which can be
    // hundreds of milliseconds later and is what made a page and its poll, fired
    // together by the browser, collide. Release on the final read; the token's
    // deleter is the backstop if the response is torn down early.
    Arena* ap = &a;
    size_t total = 0; for (int i = 0; i < a.n; ++i) total += a.segs[i].len;
    auto released = std::make_shared<bool>(false);
    auto release = [ap, released]() { if (!*released) { *released = true; ap->give(); } };
    std::shared_ptr<void> token(nullptr, [release](void*) { release(); });
    req->send(req->beginChunkedResponse(type,
        [token, ap, total, release](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            webSample();
            if (index >= total) { release(); return 0; }
            size_t pos = 0, out = 0;
            for (int i = 0; i < ap->n && out < maxLen && index + out < total; ++i) {
                const size_t len = ap->segs[i].len;
                if (index + out < pos + len) {
                    const size_t off = index + out - pos;
                    const size_t n = ((maxLen - out) < len - off) ? (maxLen - out) : (len - off);
                    memcpy(buf + out, ap->segs[i].p + off, n);
                    out += n;
                }
                pos += len;
            }
            if (index + out >= total) release();   // last bytes handed over: free the arena now
            return out;
        }));
}

// Answer 503 instead of building anything when the heap is already low. A
// browser click-through drives free heap down through connection concurrency,
// not any one payload; refusing the next page while short costs the user one
// retry and costs the board nothing, where building it anyway is how it wedges.
// API polls retry silently every few seconds, so they are shed early; a page
// is what the user is looking at, so it goes through unless memory is critical.
static bool shedIfLow(AsyncWebServerRequest* req, bool page = false) {
    webSample();
    const uint32_t blk = ESP.getMaxAllocHeap(), fr = ESP.getFreeHeap();
    // Measured in-flight cost on the Capsule: a small chunked API ~5KB, a page
    // 10-14KB, so a page load with two polls is 25-30KB out of ~50KB idle. The
    // API threshold sits at the supervisor's own floor (12KB / 16KB): shed a
    // poll rather than let the pool reach the level that triggers a restart.
    const bool ok = page ? (blk >= 10 * 1024 && fr >= 14 * 1024)
                         : (blk >= 12 * 1024 && fr >= 16 * 1024);
    if (ok) return false;
    Serial.printf("[shed] %s %s blk=%lu free=%lu\n", page ? "page" : "api", req->url().c_str(),
                  (unsigned long)blk, (unsigned long)fr);
    AsyncWebServerResponse* r = req->beginResponse(503, "text/plain", "busy");
    r->addHeader("Retry-After", "2");
    req->send(r);
    return true;
}



static void devicesPage(HtmlOut& h) {
    uint32_t now = millis();
    headInto(h);

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

    h.lit(kFootLit, strlen(kFootLit));
}

static void apCard(HtmlOut& out);
static void wifiCard(HtmlOut& out);
static void profilesCard(HtmlOut& out);
static void backupCard(HtmlOut& out);
static void otaCard(HtmlOut& out);
static void systemCard(HtmlOut& out);

static void bindingsPage(HtmlOut& h) {
    headInto(h);
    // A slave owns no BLE devices/profiles/bindings/alerts of its own — it mirrors a
    // master over ESP-NOW. Show only what it actually controls: pair/role (system),
    // its config AP, and OTA. The master-only cards (profiles, panel signals, system
    // tunables, alerts, WiFi-join, backup) would be empty or would break the link
    // (joining a router moves the SoftAP off ch1), so they're hidden.
    if (gRole == ROLE_SLAVE) {  // nothing here belongs to a slave — see /network
        h.lit(kFootLit, strlen(kFootLit));
        return;
    }
    profilesCard(h);
    h += "<div class=card><h3>Panel signals</h3>"
         "<p class=muted>Tag which device field feeds each signal the mimic / "
         "display uses. Derived options compute from the battery current vs the "
         "measured sources: <b>charge unexplained</b> = battery charge beyond "
         "solar/charger/DC-DC; <b>load</b> = consumption (sources offset by net "
         "battery flow).</p>"
         "<form method=post action=/bind><div id=sigs></div>";

    // The option catalogue is emitted ONCE and the twelve selects are built in the
    // browser. Emitting it per role meant this page grew as roles x devices x
    // fields — 24KB of heap for two devices, the largest allocation the firmware
    // made, and the one that pushed free heap to the floor. The catalogue is ~1KB
    // and the builder below is a literal, so it streams from flash.
    h += "<script>var DEV=[";
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& sl = gConfig.slots()[i];
        sig::Field fields[8];
        size_t nf = sig::fieldsForType(sl.type, fields, 8);
        for (size_t k = 0; k < nf; ++k)
            h += "[\"" + String(sl.name) + "|" + String(static_cast<int>(fields[k])) + "\",\"" +
                 String(sl.name) + " &middot; " + sig::fieldLabel(fields[k]) + "\"],";
    }
    h += "];var DRV=[[\"(charge_only)|0\",\"Derived: charge unexplained by sources\"],"
         "[\"(load_only)|0\",\"Derived: load (sources &minus; battery)\"]];var ROLES=[";
    for (size_t r = 0; r < sig::kRoleCount; ++r) {
        sig::Role role = static_cast<sig::Role>(r);
        const sig::Binding& cur = gSignals.binding(role);
        // A legacy "(derived)" binding means the load half; normalise it here so the
        // browser only has to string-compare against the two canonical values.
        String sel = cur.device[0] ? String(cur.device) + "|" + String(static_cast<int>(cur.field))
                                   : String("");
        if (strcmp(cur.device, sig::kDerived) == 0) sel = "(load_only)|0";
        bool currentRole = (role == sig::Role::SolarA || role == sig::Role::ChargerA ||
                            role == sig::Role::DcDcInA || role == sig::Role::DcDcOutA ||
                            role == sig::Role::LoadA);
        h += "[\"" + String(sig::roleKey(role)) + "\",\"" + String(sig::roleLabel(role)) +
             "\",\"" + sel + "\"," + (currentRole ? "1" : "0") + "],";
    }
    h += "];";
    h += "(function(){var o='';for(var i=0;i<ROLES.length;i++){var r=ROLES[i];"
         "o+=\"<div style='margin-bottom:.7em'><label>\"+r[1]+\"</label>\";"
         "o+=\"<select name=\"+r[0]+\" style='min-width:240px'>\";"
         "o+=\"<option value=''>&mdash; none &mdash;</option>\";"
         "var L=(r[3]?DRV:[]).concat(DEV);"
         "for(var j=0;j<L.length;j++){o+=\"<option value='\"+L[j][0]+\"'\"+"
         "(L[j][0]==r[2]?' selected':'')+\">\"+L[j][1]+\"</option>\";}"
         "o+=\"</select></div>\";}"
         "document.getElementById('sigs').innerHTML=o;})();</script>";
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
         "rollover to local midnight (e.g. 600 = AEST +10h) and is applied to the "
         "date/time below.</p></div>";

    // Date & time — set the device clock (drives the daily-stats rollover, the SD
    // history log timestamps and the header clock). On an M5Capsule it's held in the
    // RTC. Moved here from the Stats page. Local wall-clock; the TZ above is applied.
    h += "<div class=card><h3>Date &amp; time</h3>"
         "<div class=inline style='align-items:flex-end'>"
         "<div><label>Date (D / M / Y)</label>"
         "<span style='display:flex;gap:.3em;align-items:center'>"
         "<input id=cd type=number min=1 max=31 placeholder=D style='width:3.6em;text-align:center'>"
         "<span class=muted>/</span>"
         "<input id=cmo type=number min=1 max=12 placeholder=M style='width:3.6em;text-align:center'>"
         "<span class=muted>/</span>"
         "<input id=cy type=number min=2023 max=2099 placeholder=Y style='width:5em;text-align:center'></span></div>"
         "<div><label>Time (24h &nbsp; H : M)</label>"
         "<span style='display:flex;gap:.3em;align-items:center'>"
         "<input id=ch type=number min=0 max=23 placeholder=H style='width:3.6em;text-align:center'>"
         "<span class=muted>:</span>"
         "<input id=cmi type=number min=0 max=59 placeholder=M style='width:3.6em;text-align:center'></span></div>"
         "<div><button id=setClock type=button>Set</button> "
         "<button id=nowClock class=ghost type=button title=\"copy this browser's clock\">Now</button></div>"
         "</div>"
         "<p class=muted id=clockNow>&mdash;</p>"
         "<p class=muted><b>Now</b> copies the clock from the browser you're on; or type a "
         "local date &amp; time and press <b>Set</b>.</p>"
         "<script>(function(){function g(i){return document.getElementById(i);}"
         "function p(n){return(n<10?'0':'')+n;}"
         "function show(){if(document.hidden)return;fetch('/api/time').then(function(r){return r.json();}).then(function(t){"
         "var e=g('clockNow');if(!e)return;"
         "if(t.epoch>0){var d=new Date(t.epoch*1000);"
         "e.textContent='Device clock: '+d.getUTCFullYear()+'-'+p(d.getUTCMonth()+1)+'-'+p(d.getUTCDate())+' '+p(d.getUTCHours())+':'+p(d.getUTCMinutes())+':'+p(d.getUTCSeconds());"
         "if(document.activeElement&&document.activeElement.tagName=='INPUT')return;"
         "g('cy').value=d.getUTCFullYear();g('cmo').value=d.getUTCMonth()+1;g('cd').value=d.getUTCDate();g('ch').value=d.getUTCHours();g('cmi').value=d.getUTCMinutes();"
         "}else e.textContent='Device clock: not set';}).catch(function(){});}"
         "g('setClock').addEventListener('click',function(){var b=this;b.textContent='\\u2026';"
         "fetch('/api/time?y='+g('cy').value+'&mo='+g('cmo').value+'&d='+g('cd').value+'&h='+g('ch').value+'&m='+g('cmi').value,{method:'POST'})"
         ".then(function(r){return r.text();}).then(function(t){b.textContent=(t=='ok')?'\\u2713':'?';setTimeout(function(){b.textContent='Set';},1500);show();});});"
         "g('nowClock').addEventListener('click',function(){var b=this;b.textContent='\\u2026';"
         "fetch('/api/time?epoch='+Math.floor(Date.now()/1000),{method:'POST'})"
         ".then(function(r){return r.text();}).then(function(){b.textContent='\\u2713';setTimeout(function(){b.textContent='Now';},1500);show();});});"
         "show();setInterval(show,5000);})();</script></div>";

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
         "<div><label>Buzzer (M5Capsule)</label>"
         "<input name=buzzer type=checkbox " + String(gBuzzerEnable ? "checked" : "") + "></div>"
         "<button>save</button></form>"
         "<p class=muted>Shown as a banner on the mimic and on the onboard LED "
         "(red = critical, amber = warning, green = charging). 0 disables a check. "
         "A configured device that stops broadcasting also raises a warning. On an "
         "M5Capsule the buzzer chirps while the battery is SoC-critical.</p></div>";

    h.lit(kFootLit, strlen(kFootLit));
}

// Connectivity and system control, split off /bindings so neither page has to be
// built whole in RAM at once. That page reached 24KB of heap for a two-device
// setup — the largest allocation the firmware made, and the one that walked free
// heap down to nothing. Splitting halves the peak again on top of building the
// selects client-side, and it is also where a slave's own controls live.
static void networkPage(HtmlOut& h) {
    headInto(h);
    systemCard(h);
    apCard(h);
    if (gRole != ROLE_SLAVE) wifiCard(h);  // joining a router moves the AP off ch1
    otaCard(h);
    if (gRole != ROLE_SLAVE) backupCard(h);
    h.lit(kFootLit, strlen(kFootLit));
}

// Serial self-test: build both big pages into pieces (the path that was OOM-crashing)
// and report piece count / total size / heap, WITHOUT serving. Lets us verify the
// memory behaviour on the no-PSRAM Capsule when no HTTP client is reachable here.
void webSelfTest() {
    // Build each page and the widest history window into the arena and report
    // how much of it they used: with a fixed arena the number that matters is
    // the high-water mark against kArenaSize, not the heap.
    struct { const char* name; void (*fn)(HtmlOut&); } pages[] = {
        {"devices", devicesPage}, {"bindings", bindingsPage}, {"network", networkPage}};
    for (auto& pg : pages) {
        if (!arenaTake()) { Serial.println("[webtest] arena busy"); return; }
        HtmlOut h; pg.fn(h);
        size_t total = 0; for (int i = 0; i < gSegN; ++i) total += gSegs[i].len;
        Serial.printf("[webtest] %s: %u bytes, %u generated into arena, %d segments%s\n", pg.name,
                      (unsigned)total, (unsigned)gArenaUsed, gSegN, gArenaOverflow ? " OVERFLOW" : "");
        arenaGive();
    }
    for (int mins : {1, 1440}) {
        if (!arenaTake()) return;
        HtmlOut h; buildHistoryInto(mins, h);
        Serial.printf("[webtest] /api/history?mins=%d: %u bytes into arena%s\n", mins,
                      (unsigned)gArenaUsed, gArenaOverflow ? " OVERFLOW" : "");
        arenaGive();
    }
    Serial.printf("[webtest] arena high-water %u / %u\n", (unsigned)gArenaHigh, (unsigned)kArenaSize);
    Serial.printf("[webtest] /api/panel: %s\n", buildPanelJson().c_str());
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
    saveBuzzerEnable(req->hasParam("buzzer", true));  // unchecked box => absent => mute
    req->redirect("/bindings");
}

static void handleStatsReset(AsyncWebServerRequest* req) {
    String scope = param(req, "scope");
    stats::Scope sc = scope == "today" ? stats::TODAY : (scope == "total" ? stats::TOTAL : stats::TRIP);
    gStats.reset(sc);
    gStats.maybePersist(millis(), /*force=*/true);
    req->redirect("/stats");
}

// Set the clock, from the Settings "Date & time" card or the "Now" button. On an
// M5Capsule this writes through to the RTC (see setManualClock/saveClock).
static void handleTime(AsyncWebServerRequest* req) {
    auto pv = [&](const char* k) -> String {
        if (req->hasParam(k, true)) return req->getParam(k, true)->value();   // POST body
        if (req->hasParam(k, false)) return req->getParam(k, false)->value(); // query string
        return String();
    };
    // Full manual entry: local Y/Mo/D + H:M -> UTC epoch (TZ backed out).
    String ys = pv("y");
    if (ys.length()) {
        int Y = ys.toInt(), Mo = pv("mo").toInt(), D = pv("d").toInt();
        int h = pv("h").toInt(), m = pv("m").toInt();
        if (Y < 2023 || Y > 2099 || Mo < 1 || Mo > 12 || D < 1 || D > 31 ||
            h < 0 || h > 23 || m < 0 || m > 59) { req->send(200, "text/plain", "bad"); return; }
        setManualClock(localClockToUtc(Y, Mo, D, h, m));
        req->send(200, "text/plain", "ok");
        return;
    }
    // "Now" — a full UTC epoch straight from the browser clock.
    uint32_t e = (uint32_t)strtoul(pv("epoch").c_str(), nullptr, 10);
    bool ok = e > 1700000000;
    if (ok) setManualClock(e);
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

static void wifiCard(HtmlOut& out) {
    String status;
    if (gStaSsid.length()) {
        status = (WiFi.status() == WL_CONNECTED)
                     ? "Connected to <b>" + gStaSsid + "</b> &middot; IP " + WiFi.localIP().toString()
                     : "Configured for <b>" + gStaSsid + "</b> &middot; <span class=muted>connecting / not connected</span>";
    } else {
        status = "<span class=muted>Not configured (AP only)</span>";
    }
    out += "<div class=card><h3>Join a WiFi network</h3>"
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

static void apCard(HtmlOut& out) {
    out += "<div class=card><h3>Access point</h3>"
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
        if (ssid.length() == 0) { req->redirect("/network"); return; }
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
        req->redirect("/network");
        return;
    }
    if (pass.length() == 0) pass = gStaPass;  // keep existing when blank
    saveWifiCreds(ssid, pass);
    gStaSsid = ssid; gStaPass = pass;
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(gStaSsid.c_str(), gStaPass.c_str());
    configTime(0, 0, "pool.ntp.org");  // sync clock for daily-stats rollover
    req->redirect("/network");
}

static void profilesCard(HtmlOut& out) {
    out += "<div class=card><h3>Profiles</h3>"
         "<p class=muted>Each profile has its own devices, signal bindings and "
         "settings (e.g. Home vs 4WD). Switching applies immediately.</p>";
    for (int i = 0; i < ProfileManager::kMax; ++i) {
        if (!gProfiles.used(i)) continue;
        bool act = (i == gProfiles.active());
        out += "<div style='padding:.5em 0;border-bottom:1px solid var(--line);display:flex;"
             "gap:.5em;align-items:center;flex-wrap:wrap'>";
        out += "<b style='flex:1'>" + String(gProfiles.name(i)) +
             (act ? " <span class=muted>(active)</span>" : "") + "</b>";
        if (!act)
            out += "<form method=post action=/profile/switch style='margin:0'>"
                 "<input type=hidden name=id value=" + String(i) + "><button>switch</button></form>";
        out += "<form class=inline method=post action=/profile/rename style='margin:0'>"
             "<input type=hidden name=id value=" + String(i) + ">"
             "<input name=name value='" + String(gProfiles.name(i)) + "' size=12>"
             "<button class=ghost>rename</button></form>";
        if (!act && gProfiles.usedCount() > 1)
            out += "<form method=post action=/profile/del style='margin:0' "
                 "onsubmit=\"return confirm('Delete this profile and all its data?')\">"
                 "<input type=hidden name=id value=" + String(i) + ">"
                 "<button class=danger>delete</button></form>";
        out += "</div>";
    }
    if (gProfiles.usedCount() < ProfileManager::kMax)
        out += "<form class=inline method=post action=/profile/new style='margin-top:.8em'>"
             "<div><label>New profile name</label><input name=name required></div>"
             "<button>create</button></form>";
    else
        out += "<p class=muted>Maximum profiles reached.</p>";
    out += "</div>";
    return;
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
    webRenderStaticPages();  // the profile name is baked into the cached head
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

// Scratch config + signal map for backup/restore. These used to be file-scope
// statics "kept off the async-task stack", which is right, but that made them
// a permanent 2KB+ of RAM for two operations a user runs a few times a year.
// Each caller now allocates one on the heap for the call and frees it after.
struct TmpCfg { DeviceConfig cfg; sig::SignalMap sig; };
static TmpCfg gTmp;  // allocated at boot, used by backup/restore only

static String buildExportJson() {
    TmpCfg* tmp = &gTmp;
    String j = "{\"version\":1,\"active\":" + String(gProfiles.active()) + ",";
    j += "\"wifi\":{\"ssid\":\"" + jsonEsc(gStaSsid) + "\",\"pass\":\"" + jsonEsc(gStaPass) +
         "\"},\"profiles\":[";
    bool firstP = true;
    for (int pid = 0; pid < ProfileManager::kMax; ++pid) {
        if (!gProfiles.used(pid)) continue;
        if (!firstP) j += ",";
        firstP = false;
        tmp->cfg.begin(pid);
        tmp->sig.begin(tmp->cfg.slots(), tmp->cfg.count(), pid);
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
        for (size_t i = 0; i < tmp->cfg.count(); ++i) {
            if (i) j += ",";
            DeviceSlot& s = tmp->cfg.slots()[i];
            j += "{\"name\":\"" + jsonEsc(s.name) + "\",\"type\":\"" + typeName(s.type) +
                 "\",\"key\":\"" + keyHex(s.key) + "\"}";
        }
        j += "],\"bindings\":[";
        bool firstB = true;
        for (size_t r = 0; r < sig::kRoleCount; ++r) {
            const sig::Binding& b = tmp->sig.binding(static_cast<sig::Role>(r));
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
    TmpCfg* tmp = &gTmp;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonArray profs = doc["profiles"].as<JsonArray>();
    if (profs.isNull()) return false;

    for (JsonObject pr : profs) {
        int pid = pr["id"] | -1;
        if (pid < 0 || pid >= ProfileManager::kMax) continue;
        gProfiles.setName(pid, pr["name"] | "Profile");
        wipeProfile(pid);

        tmp->cfg.begin(pid);
        tmp->cfg.clear();  // drop any seeded defaults; install exactly the backup
        for (JsonObject d : pr["devices"].as<JsonArray>()) {
            const char* dn = d["name"] | "";
            uint8_t k[16];
            if (dn[0] && DeviceConfig::parseHexKey(String((const char*)(d["key"] | "")), k))
                tmp->cfg.add(dn, parseType(String((const char*)(d["type"] | "battery"))), k);
        }
        tmp->cfg.save();

        tmp->sig.begin(tmp->cfg.slots(), tmp->cfg.count(), pid);
        for (size_t r = 0; r < sig::kRoleCount; ++r)
            tmp->sig.set(static_cast<sig::Role>(r), "", sig::Field::None);
        for (JsonObject bd : pr["bindings"].as<JsonArray>()) {
            const char* rk = bd["role"] | "";
            for (size_t r = 0; r < sig::kRoleCount; ++r)
                if (strcmp(sig::roleKey(static_cast<sig::Role>(r)), rk) == 0)
                    tmp->sig.set(static_cast<sig::Role>(r), bd["device"] | "",
                                static_cast<sig::Field>(bd["field"] | 0));
        }
        tmp->sig.save();

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

static void backupCard(HtmlOut& out) {
    out += F(
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

static void diagInto(OutSink& o) {
    uint32_t now = millis();
    o.put("[", 1);
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& sl = gConfig.slots()[i];
        uint32_t age = sl.everSeen ? (now - sl.lastSeenMs) / 1000 : 0;
        String nm = jsonEsc(sl.name), fields = diagFields(sl);   // small; the rest streams
        o.f("%s{\"name\":\"%s\",\"type\":\"%s\",\"mac\":\"%s\",\"model\":\"%s\",\"seen\":%s,\"stale\":%s,\"age\":%lu,\"raw\":\"",
            i ? "," : "", nm.c_str(), typeName(sl.type), sl.mac, modelHex(sl.modelId).c_str(),
            sl.everSeen ? "true" : "false", sl.stale(now) ? "true" : "false", (unsigned long)age);
        static const char* hx = "0123456789abcdef";
        for (size_t k = 0; k < sl.rawLen; ++k) { char b[2] = {hx[sl.raw[k] >> 4], hx[sl.raw[k] & 0xF]}; o.put(b, 2); }
        o.put("\",\"fields\":", 11); o.put(fields.c_str(), fields.length()); o.put("}", 1);
    }
    o.put("]", 1);
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

static void otaCard(HtmlOut& out) {
    out += F(
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
        // Two-step confirm rather than window.confirm(), for the same captive-portal
        // reason as the System card's buttons.
        "cfm(this,function(){"
        "m.textContent='Uploading\\u2026 do not close this page.';"
        "var fd=new FormData();fd.append('f',f);"
        "fetch('/api/ota',{method:'POST',body:fd}).then(function(x){return x.text();})"
        ".then(function(t){m.textContent=t.indexOf('OK')>=0?"
        "'Flashed \\u2014 rebooting, reload in ~10 s.':'Update failed (see serial log).';})"
        ".catch(function(){m.textContent='Upload sent; if it succeeded the device is rebooting.';});"
        "});});"
        "</script></div>");
}

// System / pairing card — the web equivalent of the on-screen Diag controls, so
// anything doable on the display is doable from the AP (pair a slave, toggle debug
// capture, switch role). Shown on the settings page for both roles.
static void systemCard(HtmlOut& out) {
    char idbuf[12];
    snprintf(idbuf, sizeof(idbuf), "%08X", gRole == ROLE_SLAVE ? gRx.pairedMaster() : gMasterId);
    // Two-step confirm for the destructive/rebooting actions below: the first tap
    // arms the button and relabels it, the second performs the action, and it
    // disarms itself after 4 s. Deliberately not window.confirm() — see the note on
    // the buttons. Defined once here and reused by the OTA card on the same page.
    out += "<script>function cfm(b,fn){"
               "if(b.dataset.arm){delete b.dataset.arm;b.textContent=b.dataset.l0;fn();return;}"
               "b.dataset.l0=b.textContent;b.dataset.arm='1';b.textContent='Tap again to confirm';"
               "setTimeout(function(){if(b.dataset.arm){delete b.dataset.arm;"
               "b.textContent=b.dataset.l0;}},4000);}</script>";
    out += "<div class=card><h3>System</h3>";
    out += "<p class=muted>Role: <b>" + String(gRole == ROLE_SLAVE ? "Slave" : "Master") + "</b> &middot; " +
         String(gRole == ROLE_SLAVE ? "paired master" : "id") + " " + String(idbuf) + "</p>";
#ifdef VICMON_HAS_M5CAPSULE
    if (capsulePresent())
        out += "<p class=muted>M5Capsule &middot; RTC " + String(capsuleRtcOk() ? "set" : "unset") +
             " &middot; SD " + String(capsuleSdOk() ? "logging" : "none") +
             " &middot; buzzer " + String(gBuzzerEnable ? "on" : "muted") + "</p>";
#endif
    // NB: these use an in-page two-step confirm (cfm), NOT window.confirm(). The
    // device runs a captive-portal DNS, so a phone joining its AP opens this page in
    // the OS captive-portal WebView (Android CaptivePortalLogin / iOS CNA) — and
    // those routinely suppress native dialogs, returning false with nothing shown.
    // Every confirm()-guarded action then silently did nothing, which is exactly how
    // "Switch to Slave" appeared broken while the unguarded Pair button worked.
    if (gRole == ROLE_SLAVE) {
        out += "<button type=button id=pairBtn data-lbl='Pair to a master' onclick=\"fetch('/api/pair',{method:'POST'});this.textContent='Opening\\u2026'\">Pair to a master</button> ";
        out += "<button type=button onclick=\"cfm(this,function(){fetch('/api/unpair',{method:'POST'})})\">Unpair</button> ";
        out += "<button type=button onclick=\"cfm(this,function(){fetch('/api/role',{method:'POST'})})\">Switch to Master</button>";
        out += "<p class=muted>Pair while a master's pairing window is open. Switching role reboots. "
             "Unpair and Switch ask for a second tap to confirm.</p>";
    } else {
        out += "<button type=button id=pairBtn data-lbl='Pair a slave' onclick=\"fetch('/api/pair',{method:'POST'});this.textContent='Opening\\u2026'\">Pair a slave</button> ";
        out += "<button type=button onclick=\"fetch('/api/debug',{method:'POST'}).then(()=>location.reload())\">Toggle debug capture</button> ";
        out += "<button type=button onclick=\"cfm(this,function(){fetch('/api/role',{method:'POST'})})\">Switch to Slave</button>";
        out += "<p class=muted>Pairing lets a slave display adopt this master (60 s window). Debug "
             "capture records raw bytes of unknown Victron devices. Switching role reboots "
             "(a second tap confirms).</p>";
    }
    // Firmware clone (OTA push): either role can push its running image to the
    // paired peer over ESP-NOW; the peer accepts only if it allows remote updates.
    // A dropped transfer is non-destructive (the target keeps its current firmware).
    out += "<p class=muted style='margin:.9em 0 .3em'>&mdash; Firmware clone (wireless) &mdash;</p>";
    out += "<p class=muted id=otaVer style='margin:.1em 0 .5em'>This unit: firmware " +
         String(kFwVersion) + " &middot; built " + String(gOta.builtStr()) + "</p>";
    out += "<label style='font-weight:normal;display:block;margin-bottom:.4em'>"
         "<input type=checkbox id=otaAllow onchange=\"fetch('/api/ota/allow?v='+(this.checked?1:0),{method:'POST'})\"> "
         "Allow this device to be updated remotely</label>";
    out += "<button type=button onclick=\"cfm(this,otaPush)\">Send my firmware to the paired device</button> "
         "<button type=button class=ghost onclick=\"cfm(this,otaPull)\">Update this device from the paired device</button> "
         "<span id=otaStat class=muted></span>";
    out += "<p class=muted>Push: the paired " + String(gRole == ROLE_SLAVE ? "master" : "slave") +
         " must have “allow remote update” on. Pull: asks the paired device to update <i>this</i> one, "
         "and it answers only if its firmware is newer. Either way the target reboots into the new "
         "firmware only if the whole image validates, so an interrupted transfer is harmless.</p>";
    out += "<script>"
         "function otaPoll(){if(document.hidden)return;fetch('/api/ota/status').then(r=>r.json()).then(s=>{"
         "var a=document.getElementById('otaAllow');if(a)a.checked=s.allow;"
         "var v=document.getElementById('otaVer');if(v&&s.version){var pt=s.peerKnown?(' \\u00b7 paired: '+s.peer+' ('+s.peerRel+')'):' \\u00b7 paired: not heard yet';v.textContent='This unit: firmware '+s.version+' \\u00b7 built '+s.built+pt;}"
         "var e=document.getElementById('otaStat');if(e)e.textContent=s.status+(s.busy?(' '+s.percent+'%'):'')+(s.mismatch?(' \u2014 '+s.mismatch):'');"
         "var pb=document.getElementById('pairBtn');if(pb){if(s.pairing){pb.textContent='Pairing\\u2026 '+s.pairSec+'s';pb.style.background='#22d3ee';pb.style.color='#001018';}else{pb.textContent=pb.dataset.lbl;pb.style.background='';pb.style.color='';}}"
         "}).catch(()=>{});}"
         "function otaPush(){"
         "fetch('/api/ota/push',{method:'POST'}).then(r=>r.text()).then(t=>{"
         "var e=document.getElementById('otaStat');if(e)e.textContent=t;});}"
         "function otaPull(){"
         "fetch('/api/ota/pull',{method:'POST'}).then(r=>r.text()).then(t=>{"
         "var e=document.getElementById('otaStat');if(e)e.textContent=t;});}"
         "otaPoll();setInterval(otaPoll,4000);"
         "</script>";
    out += "</div>";
    return;
}

void setupServer() {
    gServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Streams straight from flash; send(const char*) would copy it into a
        // heap String on every page load.
        // Cached client-side for a day: each page load then costs one connection
        // fewer, and connection concurrency is what drives the pool down.
        AsyncWebServerResponse* r = req->beginResponse_P(200, "text/css", (const uint8_t*)kStyle, strlen(kStyle));
        r->addHeader("Cache-Control", "max-age=86400");
        req->send(r);
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
    // Firmware clone (OTA push) — offer this device's running image to the paired
    // peer, toggle whether we accept an incoming push, and report transfer status.
    gServer.on("/api/ota/push", HTTP_POST, [](AsyncWebServerRequest* req) {
        bool ok = gOta.startPush();
        req->send(200, "text/plain", ok ? "offering firmware to the paired device…" : "busy");
    });
    gServer.on("/api/ota/pull", HTTP_POST, [](AsyncWebServerRequest* req) {
        bool ok = gOta.startPull();
        req->send(200, "text/plain", ok ? "requesting an update from the paired device…" : "busy");
    });
    gServer.on("/api/ota/allow", HTTP_POST, [](AsyncWebServerRequest* req) {
        bool v = req->hasParam("v") ? req->getParam("v")->value().toInt() != 0 : true;
        saveOtaAllow(v);
        req->send(200, "text/plain", v ? "on" : "off");
    });
    gServer.on("/api/ota/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        String j = "{\"allow\":";
        j += gOta.allowRemote() ? "true" : "false";
        j += ",\"busy\":";
        j += gOta.busy() ? "true" : "false";
        j += ",\"percent\":" + String(gOta.percent());
        j += ",\"version\":\"" + jsonEsc(String(gOta.localVersion())) + "\"";
        j += ",\"built\":\"" + jsonEsc(String(gOta.builtStr())) + "\"";
        j += ",\"peerKnown\":";
        j += gOta.peerKnown() ? "true" : "false";
        j += ",\"peer\":\"" + jsonEsc(String(gOta.peerVersion())) + "\"";
        j += ",\"peerRel\":\"" + jsonEsc(String(gOta.peerRel())) + "\"";
        bool pairing = gRole == ROLE_SLAVE ? gRx.isAdopting() : pairingActive();
        int pairSec = gRole == ROLE_SLAVE ? (int)gRx.adoptSecsLeft() : pairSecsLeft();
        j += ",\"pairing\":";
        j += pairing ? "true" : "false";
        j += ",\"pairSec\":" + String(pairSec);
        j += ",\"status\":\"" + jsonEsc(String(gOta.statusText())) + "\"";
        j += ",\"mismatch\":\"" + String(gLinkMismatch == 1 ? "paired master is on newer firmware — pulling it" : gLinkMismatch == 2 ? "paired master is on older firmware — send it this one" : "") + "\"}";
        req->send(200, "application/json", j);
    });
    gServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req, true)) return;
        serveStaticCached(req, 0);
    });
    gServer.on("/devices", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req, true)) return;
        if (!arenaTake()) { req->send(503, "text/plain", "busy"); return; }
        HtmlOut h;
        devicesPage(h);
        serveArena(req, "text/html");
    });
    gServer.on("/bindings", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req, true)) return;
        if (!arenaTake()) { req->send(503, "text/plain", "busy"); return; }
        HtmlOut h;
        bindingsPage(h);
        serveArena(req, "text/html");
    });
    gServer.on("/network", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req, true)) return;
        if (!arenaTake()) { req->send(503, "text/plain", "busy"); return; }
        HtmlOut h;
        networkPage(h);
        serveArena(req, "text/html");
    });
    gServer.on("/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req, true)) return;
        serveStaticCached(req, 1);
    });
    gServer.on("/diag", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req, true)) return;
        serveStaticCached(req, 2);
    });
    gServer.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req)) return;
        if (!gPageArena.take()) { req->send(503, "text/plain", "busy"); return; }
        HtmlOut h;
        diagInto(h);
        serveArena(req, "application/json");
    });
    gServer.on("/api/panel", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildPanelJson());
    });
    gServer.on("/api/time", HTTP_POST, handleTime);
    gServer.on("/api/time", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Device clock for the header display: local epoch (0 = no clock set yet).
        uint32_t le = currentLocalEpoch();
        String j = "{\"epoch\":" + String(le) + ",\"utc\":" + String(currentUtcEpoch()) +
                   ",\"clock\":" + (le ? "true" : "false") + "}";
        req->send(200, "application/json", j);
    });
    gServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req)) return;
        if (!gPageArena.take()) { req->send(503, "text/plain", "busy"); return; }
        HtmlOut h;
        statsInto(h);
        serveArena(req, "application/json");
    });
    gServer.on("/api/sys", HTTP_GET, [](AsyncWebServerRequest* req) {
        webSample();
        uint32_t wm = gWebMinFree == 0xFFFFFFFF ? 0 : gWebMinFree;
        if (req->hasParam("reset")) gWebMinFree = 0xFFFFFFFF;
        // Live memory + uptime for the Diagnostics page. maxblk = largest allocatable
        // block (fragmentation), minheap = lowest free heap ever (catches transient
        // pressure, e.g. serving a big page) — the numbers that matter on no-PSRAM.
        String j = "{\"heap\":" + String(ESP.getFreeHeap()) +
                   ",\"maxblk\":" + String(ESP.getMaxAllocHeap()) +
                   ",\"minheap\":" + String(ESP.getMinFreeHeap()) +
                   ",\"uptime\":" + String(millis() / 1000) +
                   ",\"psram\":" + String(ESP.getPsramSize()) +
                   ",\"arena_high\":" + String(gArenaHigh) + ",\"arena\":" + String(kArenaSize) +
                   ",\"head\":" + String(gStaticHeadLen) +
                   ",\"web_min\":" + String(wm) +
                   ",\"boots\":" + String(gBootCount) +
                   ",\"heap_restarts\":" + String(gHeapRestarts) +
                   ",\"reset\":\"" + String(gResetReason) + "\"}";
        req->send(200, "application/json", j);
    });
    gServer.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req)) return;
        if (!gPageArena.take()) { req->send(503, "text/plain", "busy"); return; }
        HtmlOut h;
        dataInto(h);
        serveArena(req, "application/json");
    });
    gServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (shedIfLow(req)) return;
        int mins = req->hasParam("mins") ? req->getParam("mins")->value().toInt() : 10;
        if (!arenaTake()) { req->send(503, "text/plain", "busy"); return; }
        HtmlOut h;
        buildHistoryInto(mins, h);
        serveArena(req, "application/json");
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
    // Browsers request this on every page load (and retry after a 404). It used
    // to fall through to onNotFound and get the ENTIRE mimic page back — a full
    // page render per click, on top of the page actually being loaded.
    gServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* r = req->beginResponse(204);
        r->addHeader("Cache-Control", "max-age=86400");
        req->send(r);
    });
    gServer.onNotFound([](AsyncWebServerRequest* req) {
        if (shedIfLow(req, true)) return;
        serveStaticCached(req, 0);
    });
    gServer.begin();
}
