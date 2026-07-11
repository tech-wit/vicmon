// Vicmon master dashboard rendered directly with Arduino_GFX primitives (no
// LVGL). LVGL's flush folds on this AXS15231B canvas for reasons that survived
// exhaustive debugging, while Arduino_GFX fillRect/text/blit render flawlessly.
// The firmware fills a DashData from its resolved signals and calls render().
#pragma once

#include <Arduino_GFX_Library.h>

namespace guition {

// Number of trend columns the Graph page plots (downsampled from the firmware's
// history ring). Kept a bit under the plot width in px.
static constexpr int HIST_POINTS = 116;

// Logical panel signals shown on the Settings > Bindings sub-view. Must match
// sig::kRoleCount in the firmware (static_assert'd there).
static constexpr int ROLE_N = 12;

// Max entries in the shared binding-source list (none + 2 derived + device
// fields). The firmware publishes the whole list every snapshot so the picker
// menu opens with no lag; extra device fields beyond this are dropped.
static constexpr int BIND_MAXSRC = 32;

// Flat snapshot of everything the dashboard shows (mirrors buildPanelJson()).
// Must stay trivially copyable — it's snapshotted by value under a mutex.
struct DashData {
  const char* mode = "--";  // "Charging" / "Discharging" / "Idle" / "--"
  int worst = 0;            // worst alert severity: 0 none, 1 warn, 2 crit

  bool  battValid = false;
  float soc = 0, v = 0, a = 0;   // % , V , A (signed +charging)
  bool  ttgValid = false;
  float ttg = 0;                 // minutes
  bool  starterValid = false;
  float starterV = 0;

  bool  solarValid = false;
  float solarW = 0, solarA = 0;
  bool  chargerValid = false;
  float chargerA = 0;
  bool  dcdcValid = false;
  float dcdcOutA = 0;
  bool  dcdcInVValid = false;
  float dcdcInV = 0;
  bool  loadValid = false;
  float loadA = 0;
  bool  loadDerived = false;

  // Graph page: recent history, chronological (index [histCount-1] = newest).
  // Mirrors the web chart's series (all deci-units, -32768 = n/a).
  int16_t  histSoc[HIST_POINTS];      // deci-percent 0..1000
  int16_t  histBatt[HIST_POINTS];     // deci-amps (signed +charging)
  int16_t  histSolar[HIST_POINTS];    // deci-amps
  int16_t  histCharger[HIST_POINTS];  // deci-amps
  int16_t  histDcdc[HIST_POINTS];     // deci-amps (DC-DC output)
  int16_t  histLoad[HIST_POINTS];     // deci-amps
  int      histCount = 0;          // valid points (<= HIST_POINTS)
  uint16_t histWinMin = 60;        // selected window (minutes): 1/10/60/720/1440
  uint8_t  graphHidden = 0;        // bitfield: series hidden via the legend (bit 0=batt..5=soc)
  bool     graphSyncing = false;   // slave: pulling the trend history from the master

  // Week page: last-7-"day" energy in Ah (a "day" = a calendar day when a clock
  // is set, else 24h of run-time) + three resettable meters. Filled from gStats
  // (master) or the ESP-NOW StatsFrame (slave).
  static const int DAYS_N = 7;
  uint32_t dayStamp[DAYS_N];       // yyyymmdd (clocked) or run-day index (no clock)
  float    daySolarAh[DAYS_N], dayDcdcAh[DAYS_N], dayChargerAh[DAYS_N], dayLoadAh[DAYS_N];
  int      dayCount = 0;
  uint32_t dayNow = 0;             // current day key (run-index or yyyymmdd) for axis labels
  bool     clockOk = false;        // a real/manual clock is set (date labels vs "Day N")

  // Net-in / net-out (Ah) meters. inAh = charged into battery, outAh = discharged.
  struct StatMeter { float inAh, outAh, solarAh, dcdcAh, chargerAh, loadAh; uint32_t durSecs; };
  StatMeter statToday, statTrip, statTotal;

  // Week long-press-to-reset feedback (display-owned): which card (0..2) is held
  // and how far through the ~2s hold (0..1); -1 = none.
  int      weekHold = -1;
  float    weekHoldFrac = 0;

  // Settings page: status.
  int      profileId = 0;          // active profile
  int      profileCount = 0;       // profiles in use
  char     profNames[4][20];       // ProfileManager::kMax
  bool     profUsed[4];
  char     apSsid[24] = "";
  char     apPass[24] = "";
  char     ipStr[20] = "";
  int      devPaired = 0;          // configured signal-source devices
  int      devSeen = 0;            // BLE devices currently discovered
  uint32_t uptimeSec = 0;
  uint32_t freeHeapKb = 0;
  char     version[16] = "";

  // Settings page: tunables (mirrored from firmware globals). Brightness and the
  // selected row are display-owned (set by the display task before render).
  uint8_t  brightness = 100;       // 0..100 %
  float    battCapAh = 0;          // 0 = unknown/auto
  float    deadbandA = 0;
  int      tzMin = 0;              // timezone offset, minutes
  float    socWarn = 0, socCrit = 0, vLow = 0, vHigh = 0;
  uint8_t  setSel = 0;             // selected tunable row (0..TUNABLE_N-1)
  uint8_t  setView = 0;            // Settings sub-view: 0 = tunables, 1 = bindings, 2 = diagnostics
  int      menuRole = -1;          // open source-picker role (-1 = list), display-owned
  uint8_t  bindPage = 0;           // bindings-list page (display-owned)
  uint8_t  menuPage = 0;           // source-picker page (display-owned)
  uint8_t  diagScreen = 0;         // Diag sub-screen: 0 menu / MON / DISC / DEBUG / ROLE / LINK

  // Settings > Bindings. `srcLabels` is the shared list of selectable sources
  // (index 0 = none, 1 = derived charge, 2 = derived load, then device+field);
  // `bindIdx[r]` is the shared index each role is currently bound to; `bindLabel`
  // is that label pre-resolved for the list view. All filled by the firmware.
  int      srcCount = 0;
  char     srcLabels[BIND_MAXSRC][24];
  int      bindIdx[ROLE_N];
  char     bindLabel[ROLE_N][24];

  // Settings > Diagnostics sub-view + device role.
  uint8_t  role = 0;               // 0 = master, 1 = slave (this device's role)
  bool     debugCapture = false;   // master: capturing raw bytes of unknown adverts
  bool     espNowOk = false;       // ESP-NOW radio up (broadcaster or receiver)
  uint32_t masterId = 0;           // master: our id; slave: the paired master's id (0 = none)
  uint16_t snapSeq = 0;            // master: last broadcast sequence number
  bool     pairing = false;        // pairing window (master) / adopt window (slave) open
  int      pairSecLeft = 0;        // seconds left in that window
  bool     linkLive = false;       // slave: receiving frames from our master
  bool     linkStale = false;      // slave: link dropped but showing last-known values
  uint32_t linkDrops = 0;          // slave: sequence gaps observed
  uint8_t  linkChannel = 0;        // slave: current listen channel
  bool     heardInvite = false;    // slave: a master is inviting pairing right now

  // Diagnostics: monitored (configured) devices.
  static const int MON_N = 6;
  int      monCount = 0;
  char     monName[MON_N][18];
  char     monType[MON_N][8];
  bool     monLive[MON_N];
  char     monVal[MON_N][20];
  // Diagnostics: discovered (unknown) devices; discRaw filled only in debug mode.
  static const int DISC_N = 6;
  int      discCount = 0;
  char     discName[DISC_N][18];
  char     discMac[DISC_N][20];
  uint16_t discModel[DISC_N];
  int      discRssi[DISC_N];
  char     discRaw[DISC_N][36];
};

// Pages selectable via the bottom tab bar.
enum Page : uint8_t { PAGE_DASH = 0, PAGE_FLOW, PAGE_GRAPH, PAGE_DAYS, PAGE_SETTINGS, PAGE_COUNT };

// Adjustable tunables on the Settings page (indices shared with the firmware's
// apply logic). Index 0 (brightness) is handled locally by the display task.
enum Tunable : uint8_t {
  TUN_BRIGHT = 0, TUN_BATTCAP, TUN_DEADBAND, TUN_TZ,
  TUN_SOCWARN, TUN_SOCCRIT, TUN_VLOW, TUN_VHIGH, TUNABLE_N
};

// Result of a tap on the Settings page (Tune sub-view). SA_PAIR = the Pair action
// row (opens the pairing window on a master / adopts on a slave).
enum SettingsAction : uint8_t { SA_NONE = 0, SA_PROFILE, SA_SELECT_ROW, SA_ADJ_DN, SA_ADJ_UP, SA_PAIR };
struct SettingsHitResult { SettingsAction action; int index; };  // index: profile id or row

// Draw the given page (content + tab bar) into the landscape 480x320 canvas.
// Does not flush.
void renderPage(Arduino_GFX* c, Page page, const DashData& d);

// If (tx,ty) (landscape coords) is inside the tab bar, return the tapped page
// (>=0); otherwise return -1.
int tabHitTest(int tx, int ty);

// Hit-test the Settings-page (Tune view) controls. `role` (0 master / 1 slave)
// selects the tunable list + whether the profile rows are active.
SettingsHitResult settingsHit(int tx, int ty, int role);

// Settings sub-view toggle: returns 0 (Tunables), 1 (Bindings) or 2 (Diagnostics)
// if a view pill was tapped, else -1. Call only when the Settings page is up.
int settingsViewHit(int tx, int ty);

// The Diagnostics sub-view is a small menu of screens. Screen ids (DashData.
// diagScreen) and the actions a tap can produce.
enum DiagScreen : uint8_t {
  DS_MENU = 0, DS_MON, DS_DISC, DS_DEBUG, DS_ROLE, DS_LINK
};
enum DiagAction : uint8_t {
  DIAG_NONE = 0, DIAG_BACK,
  DIAG_OPEN_MON, DIAG_OPEN_DISC, DIAG_OPEN_DEBUG, DIAG_OPEN_ROLE, DIAG_OPEN_LINK,
  DIAG_DEBUG_TOGGLE, DIAG_ROLE_TOGGLE, DIAG_UNPAIR
};

// Hit-test the Diagnostics sub-view given the current screen + role. Returns a
// DiagAction (DIAG_NONE on a miss). On the menu it returns which screen to open;
// on a sub-screen it returns Back or the screen's control action.
int diagHit(int tx, int ty, int role, int screen);

// Rows per page in the paginated Bindings list and source-picker.
static constexpr int BIND_PERPAGE = 6;

// Number of pages in the bindings list (ROLE_N signals, BIND_PERPAGE per page).
int bindListPages();

// Hit-test the Bindings list: returns the on-page row slot (0..BIND_PERPAGE-1),
// or -2 (prev page) / -3 (next page), or -1 for a miss. Add bindPage*BIND_PERPAGE
// to a slot to get the signal role.
int bindingHit(int tx, int ty);

// Fills `outShared` with the shared source indices visible in `role`'s picker
// (all sources; the two derived entries only for the current-flow roles) and
// returns the count. Lets the display map a menu cell back to a shared index.
int bindVisible(int role, int srcCount, int* outShared, int max);

// Hit-test the open source-picker: returns the on-page row slot
// (0..BIND_PERPAGE-1), or -2 (Back) / -3 (prev page) / -4 (next page), or -1 for
// a miss. Add menuPage*BIND_PERPAGE to a slot to index the visible list.
int bindMenuHit(int tx, int ty);

// If (tx,ty) hit a Graph-page zoom pill, return its window in minutes
// (1/10/60/720/1440); otherwise return -1. Call only when the Graph page is up.
int graphHitTest(int tx, int ty);

// If (tx,ty) hit a Graph legend slot, return the series index 0..5 (batt/solar/
// charger/dcdc/load/soc) to toggle on/off; otherwise -1.
int graphLegendHit(int tx, int ty);

// If (tx,ty) is inside one of the Week-page meter cards, return its index
// (0 Today / 1 Trip / 2 Total) for the long-press-to-reset; otherwise -1.
int weekCardAt(int tx, int ty);

// Back-compat: renders PAGE_DASH.
void renderDashboard(Arduino_GFX* c, const DashData& d);

}  // namespace guition
