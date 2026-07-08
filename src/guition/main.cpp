// Guition JC3248W535 — standalone dashboard demo (no WiFi, no LVGL).
// Renders the Arduino_GFX dashboard with animated synthetic data so the display
// can be exercised on the bench. The same lib/guition modules run in the master
// firmware. Touch is polled and printed to serial.

#include <Arduino.h>
#include <math.h>

#include <GuitionDisplay.h>
#include <GuitionTouch.h>
#include <GfxDashboard.h>

static guition::Display display;
static guition::Touch   touch;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Guition dashboard demo (Arduino_GFX) ===");

  if (!display.begin(1 /*landscape 480x320*/)) {
    Serial.println("FATAL: display.begin() failed");
    while (true) delay(1000);
  }
  Serial.printf("Display OK: %dx%d\n", display.width(), display.height());
  touch.begin(1);
}

static guition::Page page = guition::PAGE_DASH;
static int graphWin = 10;      // zoom window (min) for the Graph page
static uint8_t graphHidden = 0; // Graph legend show/hide bitfield
static int setView = 0;        // Settings sub-view: 0 tunables, 1 bindings
static int menuRole = -1;      // open source-picker role, -1 = list
static int bindPage = 0;       // bindings-list page
static int menuPage = 0;       // source-picker page

// Sample shared source list + per-role selection for the bindings demo.
static const char* kSrc[] = {
  "-- none --", "derived: charge", "derived: load",
  "BMV SoC", "BMV V", "BMV A", "BMV Ah", "BMV start V", "BMV TTG",
  "MPPT bat A", "MPPT PV W", "Orion in A", "Orion out A",
};
static const int kSrcCount = sizeof(kSrc) / sizeof(kSrc[0]);
static int bindSel[guition::ROLE_N] = {3, 4, 5, 6, 7, 8, 9, 10, 0, 11, 12, 2};

// Diagnostics demo state (so the Diag sub-view + Pair/Debug/Role are exercisable
// on the bench). Role toggle just flips a local preview (no reboot here).
static int diagRole = 0;              // 0 master preview, 1 slave preview
static bool diagDebug = false;
static uint32_t diagPairUntil = 0;    // pairing-window end (ms)
static int diagScreen = 0;            // Diag sub-screen (guition::DiagScreen)

static void fillDiag(guition::DashData& d, uint32_t now) {
  d.role = (uint8_t)diagRole;
  d.diagScreen = (uint8_t)diagScreen;
  d.espNowOk = true;
  d.masterId = 0xA1B2C3D4;
  d.snapSeq = (uint16_t)(now / 1000);
  d.pairing = diagPairUntil > now;
  d.pairSecLeft = d.pairing ? (int)((diagPairUntil - now) / 1000) : 0;
  d.debugCapture = diagDebug;
  d.linkLive = true; d.linkDrops = 3; d.linkChannel = 6; d.heardInvite = false;
  d.devPaired = 2; d.devSeen = 1; d.freeHeapKb = 176;
  strncpy(d.version, "demo", sizeof(d.version) - 1);
  // Monitored (configured) devices.
  const char* mn[2] = {"BMV", "Orion XS"};
  const char* mt[2] = {"battery", "dcdc"};
  const char* mv[2] = {"13.14V 56%", "12.8A 13.2V"};
  for (int i = 0; i < 2; ++i) {
    strncpy(d.monName[i], mn[i], sizeof(d.monName[i]) - 1); d.monName[i][sizeof(d.monName[i]) - 1] = 0;
    strncpy(d.monType[i], mt[i], sizeof(d.monType[i]) - 1); d.monType[i][sizeof(d.monType[i]) - 1] = 0;
    d.monLive[i] = true;
    strncpy(d.monVal[i], mv[i], sizeof(d.monVal[i]) - 1); d.monVal[i][sizeof(d.monVal[i]) - 1] = 0;
  }
  d.monCount = 2;
  // Discovered (unknown) device; raw hex only when debug capture is on.
  strncpy(d.discName[0], "Batt Sense", sizeof(d.discName[0]) - 1);
  strncpy(d.discMac[0], "e5:1a:7c:22:0f:9b", sizeof(d.discMac[0]) - 1);
  d.discModel[0] = 0xA3A4; d.discRssi[0] = -72;
  if (diagDebug) strncpy(d.discRaw[0], "100089a302b1c4d5e6f708", sizeof(d.discRaw[0]) - 1);
  else d.discRaw[0][0] = 0;
  d.discCount = 1;
}

// Synthesize the full trend history so the multi-line Graph page is exercisable
// on the bench without BLE.
static void fillHistory(guition::DashData& d, uint32_t now) {
  d.histWinMin = (uint16_t)graphWin;
  d.graphHidden = graphHidden;
  d.histCount = guition::HIST_POINTS;
  for (int i = 0; i < guition::HIST_POINTS; ++i) {
    float ph = (float)i / (guition::HIST_POINTS - 1);        // 0 (old) .. 1 (now)
    float t = ph * 4.0f * PI + now / 2500.0f;                // scrolls left over time
    float solar = fmaxf(0.0f, sinf(t) * 16.0f);
    float drive = fmodf(ph + now / 9000.0f, 1.0f);
    float dcdc = (drive > 0.55f && drive < 0.8f) ? 26.0f : 0.0f;
    float load = 4.0f + 2.0f * fabsf(sinf(t * 1.7f));
    float batt = solar + dcdc - load;
    float soc = 55.0f + 30.0f * sinf(t * 0.5f);
    d.histSolar[i]   = (int16_t)lroundf(solar * 10);
    d.histDcdc[i]    = (int16_t)lroundf(dcdc * 10);
    d.histLoad[i]    = (int16_t)lroundf(load * 10);
    d.histCharger[i] = -32768;                                // n/a (no AC charger)
    d.histBatt[i]    = (int16_t)lroundf(batt * 10);
    d.histSoc[i]     = (int16_t)lroundf(soc * 10);
  }
}

static void fillBindings(guition::DashData& d) {
  d.setView = (uint8_t)setView;
  d.menuRole = menuRole;
  d.bindPage = (uint8_t)bindPage;
  d.menuPage = (uint8_t)menuPage;
  strncpy(d.apSsid, "Vicmon-Master", sizeof(d.apSsid) - 1);
  strncpy(d.apPass, "vicmon1234", sizeof(d.apPass) - 1);
  d.srcCount = kSrcCount;
  for (int i = 0; i < kSrcCount && i < guition::BIND_MAXSRC; ++i) {
    strncpy(d.srcLabels[i], kSrc[i], sizeof(d.srcLabels[i]) - 1);
    d.srcLabels[i][sizeof(d.srcLabels[i]) - 1] = '\0';
  }
  for (int i = 0; i < guition::ROLE_N; ++i) {
    d.bindIdx[i] = bindSel[i];
    strncpy(d.bindLabel[i], kSrc[bindSel[i]], sizeof(d.bindLabel[i]) - 1);
    d.bindLabel[i][sizeof(d.bindLabel[i]) - 1] = '\0';
  }
}

void loop() {
  static uint32_t last = 0;
  static bool wasDown = false;
  uint32_t now = millis();

  guition::TouchPoint tp;
  bool down = touch.read(tp);
  bool redraw = false;
  if (down && !wasDown) {
    int t = guition::tabHitTest(tp.x, tp.y);
    if (t >= 0) { page = (guition::Page)t; menuRole = -1; redraw = true; }
    else if (page == guition::PAGE_GRAPH) {
      int win = guition::graphHitTest(tp.x, tp.y);
      if (win > 0) { graphWin = win; redraw = true; }
      else { int s = guition::graphLegendHit(tp.x, tp.y); if (s >= 0) { graphHidden ^= (1 << s); redraw = true; } }
    } else if (page == guition::PAGE_SETTINGS) {
      if (setView == 1 && menuRole >= 0) {
        int vis[guition::BIND_MAXSRC];
        int vc = guition::bindVisible(menuRole, kSrcCount, vis, guition::BIND_MAXSRC);
        int pages = (vc + guition::BIND_PERPAGE - 1) / guition::BIND_PERPAGE;
        int k = guition::bindMenuHit(tp.x, tp.y);
        if (k == -2) { menuRole = -1; redraw = true; }
        else if (k == -3) { if (menuPage > 0) { menuPage--; redraw = true; } }
        else if (k == -4) { if (menuPage < pages - 1) { menuPage++; redraw = true; } }
        else if (k >= 0) {
          int idx = menuPage * guition::BIND_PERPAGE + k;
          if (idx < vc) { bindSel[menuRole] = vis[idx]; menuRole = -1; redraw = true; }
        }
      } else {
        int effView = setView;
        if (diagRole == 1 && effView == 1) effView = 0;
        int v = guition::settingsViewHit(tp.x, tp.y);
        if (v >= 0 && !(diagRole == 1 && v == 1)) { setView = v; menuRole = -1; diagScreen = 0; redraw = true; }
        else if (effView == 2) {
          switch (guition::diagHit(tp.x, tp.y, diagRole, diagScreen)) {
            case guition::DIAG_OPEN_MON:   diagScreen = guition::DS_MON;   redraw = true; break;
            case guition::DIAG_OPEN_DISC:  diagScreen = guition::DS_DISC;  redraw = true; break;
            case guition::DIAG_OPEN_DEBUG: diagScreen = guition::DS_DEBUG; redraw = true; break;
            case guition::DIAG_OPEN_ROLE:  diagScreen = guition::DS_ROLE;  redraw = true; break;
            case guition::DIAG_OPEN_LINK:  diagScreen = guition::DS_LINK;  redraw = true; break;
            case guition::DIAG_BACK:       diagScreen = guition::DS_MENU;  redraw = true; break;
            case guition::DIAG_DEBUG_TOGGLE: diagDebug = !diagDebug; redraw = true; break;
            case guition::DIAG_ROLE_TOGGLE: diagRole ^= 1; setView = 0; diagScreen = 0; redraw = true; break;
            default: break;
          }
        }
        else if (effView == 0) {
          if (guition::settingsHit(tp.x, tp.y, diagRole).action == guition::SA_PAIR) { diagPairUntil = now + 60000; redraw = true; }
        }
        else if (effView == 1) {
          int h = guition::bindingHit(tp.x, tp.y);
          if (h == -2) { if (bindPage > 0) { bindPage--; redraw = true; } }
          else if (h == -3) { if (bindPage < guition::bindListPages() - 1) { bindPage++; redraw = true; } }
          else if (h >= 0) {
            int role = bindPage * guition::BIND_PERPAGE + h;
            if (role < guition::ROLE_N) { menuRole = role; menuPage = 0; redraw = true; }
          }
        }
      }
    }
  }
  wasDown = down;

  if (redraw || now - last >= 500) {
    last = now;
    // Synthetic "day": SoC swings, solar follows a sine, a drive window pushes DC-DC.
    float phase = fmodf(now / 1000.0f, 60.0f) / 60.0f;   // 60s day
    float sun = sinf(phase * 2.0f * PI);
    bool engine = (phase > 0.6f && phase < 0.8f);

    guition::DashData d;
    d.battValid = true;
    d.soc = 50 + 45 * sinf(phase * 2 * PI);
    d.v = 13.2f + 0.3f * sun;
    float solarA = sun > 0 ? sun * 18 : 0;
    float dcdcA = engine ? 28 : 0;
    float loadA = 5;
    d.a = solarA + dcdcA - loadA;
    d.mode = d.a > 0.5f ? "Charging" : (d.a < -0.5f ? "Discharging" : "Idle");
    d.ttgValid = true; d.ttg = 320;
    d.starterValid = true; d.starterV = 12.6f;
    d.solarValid = true; d.solarW = solarA * 13; d.solarA = solarA;
    d.chargerValid = false;
    d.dcdcValid = engine; d.dcdcOutA = dcdcA; d.dcdcInVValid = engine; d.dcdcInV = 13.8f;
    d.loadValid = true; d.loadA = loadA; d.loadDerived = true;
    fillHistory(d, now);
    fillBindings(d);
    fillDiag(d, now);

    guition::renderPage(display.canvas(), page, d);
    display.flush();
  }
  delay(5);
}
