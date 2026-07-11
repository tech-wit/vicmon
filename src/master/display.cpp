// Display: the Guition dashboard glue. Owns the display+touch task, collects the
// registry/panel/history into a DashData snapshot for the renderer, and services
// Settings-page requests (tunables, bindings, pairing, profile switch) on the
// loop task. Split out of main.cpp (P2). Compiled only when a display board is
// selected (VICMON_DISPLAY); the whole file is guarded so headless builds skip it.

#include "app.h"

#ifdef VICMON_DISPLAY

#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstring>

// ---- display state (task-owned unless noted) -------------------------------
static guition::Display  gDisplay;
static guition::Touch    gTouch;
bool                     gDisplayOk = false;  // read by main loop (extern in app.h)
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
static volatile int gStatResetReq = -1;  // request: reset stats scope (0 today/1 trip/2 total)
static int gSetSel = 0;                  // display-local: selected tunable row
static int gSetView = 0;                 // display-local: Settings sub-view (0 tune, 1 bind)
static int gBindMenuRole = -1;           // display-local: open source-picker role (-1 = list)
static int gBindPage = 0;                // display-local: bindings-list page
static int gMenuPage = 0;                // display-local: source-picker page
static int gDiagScreen = 0;              // display-local: Diag sub-screen (guition::DiagScreen)
static bool gDisplayFlip = false;        // display-local: panel rotated 180° (NVS-persisted)
static volatile bool gFlipSaveReq = false;  // request: persist gDisplayFlip on the loop task
static uint8_t flipRotation();           // 1 normal / 3 flipped (defined near bringUpDisplay)
static void saveDisplayFlip();           // defined near bringUpDisplay (NVS ns "vicdisp")

static_assert(guition::ROLE_N == static_cast<int>(sig::kRoleCount),
              "display ROLE_N must match sig::kRoleCount");
// Graph-page zoom window (minutes), set by the display task (graphHitTest owns
// the pill list), read by collectHistory on the loop task.
static volatile int gGraphWinMin = 60;
static volatile uint8_t gGraphHidden = 0;  // Graph legend: series toggled off (display-owned)

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

    // Week page: resettable meters + last-7 "day" Ah bars (a "day" = a calendar
    // day when clocked, else 24h of run-time).
    auto fillM = [](guition::DashData::StatMeter& m, const stats::Bucket& b) {
        m.inAh = b.chargedAh;    m.outAh = b.dischargedAh;
        m.solarAh = b.solarAh;   m.dcdcAh = b.dcdcAh;
        m.chargerAh = b.chargerAh; m.loadAh = b.loadAh;
        m.durSecs = b.durationSecs;
    };
    fillM(d.statToday, gStats.bucket(stats::TODAY));
    fillM(d.statTrip,  gStats.bucket(stats::TRIP));
    fillM(d.statTotal, gStats.bucket(stats::TOTAL));
    d.dayNow = gStats.bucket(stats::TODAY).dayStamp;  // current day key (axis labels)
    d.clockOk = (currentLocalEpoch() != 0);
    int dc = (int)gStats.dayCount();
    int start = dc > guition::DashData::DAYS_N ? dc - guition::DashData::DAYS_N : 0;
    int out = 0;
    for (int i = start; i < dc; ++i) {
        const stats::DayRecord& r = gStats.day(i);
        d.dayStamp[out]     = r.dayStamp;
        d.daySolarAh[out]   = r.solarAh;
        d.dayDcdcAh[out]    = r.dcdcAh;
        d.dayChargerAh[out] = r.chargerAh;
        d.dayLoadAh[out]    = r.loadAh;
        ++out;
    }
    d.dayCount = out;

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
void publishDash() {
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

    // Keep showing the last-known values when the link goes stale (flag them
    // stale) instead of blanking everything; only truly blank if we've never
    // heard this master at all.
    if (!gRx.haveSnapshot()) { d.mode = "--"; d.battValid = false; d.linkStale = false; return; }
    d.linkStale = !gRx.live();
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
    d.graphSyncing = gRx.histActive();  // show the "syncing" hint while pulling history
    d.graphSyncPct = gRx.histPercent();

    // Week page: from the low-rate stats frame (retain the last one when stale).
    if (gRx.everStats()) {
        const slavelink::StatsFrame& f = gRx.stats();
        d.clockOk = f.clockOk != 0;
        // Take the time from the master (the master owns + persists the clock). Adopt
        // it as our local clock base so this slave's own currentLocalEpoch()/AP page
        // agree; re-sync only on >5s drift so we don't reset the base every frame.
        if (f.utcNow > 1700000000u) {
            uint32_t mine = currentUtcEpoch();
            uint32_t diff = mine > f.utcNow ? mine - f.utcNow : f.utcNow - mine;
            if (!mine || diff > 5) { gManualEpoch = f.utcNow; gManualMillis = millis(); }
        }
        auto fillM = [](guition::DashData::StatMeter& m, const slavelink::StatMeterW& w) {
            m.inAh = w.inAh;       m.outAh = w.outAh;
            m.solarAh = w.solarAh; m.dcdcAh = w.dcdcAh;
            m.chargerAh = w.chargerAh; m.loadAh = w.loadAh;
            m.durSecs = w.durSecs;
        };
        fillM(d.statToday, f.today); fillM(d.statTrip, f.trip); fillM(d.statTotal, f.total);
        d.dayNow = f.dayNow;
        int n = f.dayCount > guition::DashData::DAYS_N ? guition::DashData::DAYS_N : f.dayCount;
        for (int i = 0; i < n; ++i) {
            d.dayStamp[i] = f.dayStamp[i];
            d.daySolarAh[i] = f.daySolarAh[i];
            d.dayDcdcAh[i] = f.dayDcdcAh[i];
            d.dayChargerAh[i] = f.dayChargerAh[i];
            d.dayLoadAh[i] = f.dayLoadAh[i];
        }
        d.dayCount = n;
    } else {
        d.dayCount = 0; d.clockOk = false; d.dayNow = 0;
        d.statToday = d.statTrip = d.statTotal = guition::DashData::StatMeter{};
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

void publishSlaveDash() {
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
void serviceDashRequests() {
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
    if (gFlipSaveReq) {
        gFlipSaveReq = false;
        saveDisplayFlip();  // gDisplayFlip already applied live by the display task
    }
    if (gStatResetReq >= 0) {
        int scope = gStatResetReq;
        gStatResetReq = -1;
        if (scope >= 0 && scope <= 2) {
            gStats.reset((stats::Scope)scope);   // 0 today / 1 trip / 2 total
            gStats.maybePersist(millis(), true);
            Serial.printf("[display] reset stats scope %d\n", scope);
        }
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
// TUN_FLIP (index 8) is display-owned/boolean, applied directly (not via the
// optimistic loop path); its step/clamp entries are placeholders for array parity.
static const float kTunStep[guition::TUNABLE_N] = {10, 5, 0.05f, 30, 5, 5, 0.1f, 0.1f, 1};
static const float kTunMin[guition::TUNABLE_N]  = {10, 0, 0, -720, 0, 0, 5, 5, 0};
static const float kTunMax[guition::TUNABLE_N]  = {100, 2000, 2, 840, 100, 100, 20, 20, 1};

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
        case guition::TUN_FLIP:     return d.displayFlip ? 1 : 0;
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
    int holdCard = -1;          // Week card being long-pressed (-1 = none)
    uint32_t holdStart = 0;
    bool holdFired = false;     // reset already fired for this hold
    for (;;) {
        bool redraw = false;
        int wkHold = -1; float wkFrac = 0;  // Week long-press feedback for this frame
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
                                } else if (gSetSel == guition::TUN_FLIP) {
                                    // Display-owned 180° flip: + = flipped, - = normal.
                                    // Applied live (canvas + touch), persisted by the loop.
                                    bool nf = (dir > 0);
                                    if (nf != gDisplayFlip) {
                                        gDisplayFlip = nf;
                                        gDisplay.setRotation(flipRotation());
                                        gTouch.setRotation(flipRotation());
                                        gFlipSaveReq = true;
                                    }
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
        // Week page: hold a meter card ~2s to reset it (progress fills; release
        // cancels). Tracked every poll, not just on the down-edge.
        if (gPage == guition::PAGE_DAYS && down) {
            int card = guition::weekCardAt(tp.x, tp.y);
            if (card < 0 || card != holdCard) { holdCard = card; holdStart = millis(); holdFired = false; }
            if (holdCard >= 0) {
                wkHold = holdCard;
                wkFrac = (millis() - holdStart) / 2000.0f;
                if (wkFrac > 1) wkFrac = 1;
                if (wkFrac >= 1 && !holdFired) { holdFired = true; gStatResetReq = holdCard; }
                redraw = true;  // animate the fill
            }
        } else {
            holdCard = -1;
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
            d.displayFlip = gDisplayFlip;           // screen 180° flip (display-owned)
            d.debugCapture = gDebugCapture;         // reflect the toggle instantly (display-owned)
            d.weekHold = wkHold; d.weekHoldFrac = wkFrac;  // Week long-press feedback (display-owned)
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

// Screen orientation (device-wide, NVS ns "vicdisp"). false = normal landscape
// (rotation 1), true = flipped 180° (rotation 3) for an upside-down mount.
static uint8_t flipRotation() { return gDisplayFlip ? 3 : 1; }
static void loadDisplayFlip() {
    Preferences p;
    p.begin("vicdisp", true);
    gDisplayFlip = p.getBool("flip", false);
    p.end();
}
static void saveDisplayFlip() {
    Preferences p;
    p.begin("vicdisp", false);
    p.putBool("flip", gDisplayFlip);
    p.end();
}

// Bring up the panel + touch + display task (shared by both roles). Seeds the
// first snapshot for the active role so the task has something to draw.
void bringUpDisplay() {
    loadDisplayFlip();  // restore a saved 180° flip before the panel comes up
    if (!gDisplay.begin(flipRotation() /*landscape 480x320, 1 or flipped 3*/)) {
        Serial.println("Display init FAILED (PSRAM/panel)");
        return;
    }
    Serial.printf("Display: %dx%d\n", gDisplay.width(), gDisplay.height());
    gTouch.begin(flipRotation());
    gDashMux = xSemaphoreCreateMutex();
    if (gRole == ROLE_SLAVE) publishSlaveDash(); else publishDash();  // seed
    // Priority 2 (above the Arduino loop's 1) so touch polling preempts the loop's
    // between-scan work; the task sleeps 8 ms between polls so it never starves it.
    xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 2, nullptr, 1);
    gDisplayOk = true;
}

#endif  // VICMON_DISPLAY
