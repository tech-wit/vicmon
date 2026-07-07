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
  int16_t  histSoc[HIST_POINTS];   // deci-percent 0..1000, -32768 = n/a
  int16_t  histBatt[HIST_POINTS];  // deci-amps (signed +charging), -32768 = n/a
  int      histCount = 0;          // valid points (<= HIST_POINTS)
  uint16_t histSpanSec = 0;        // time span the points cover

  // Settings page: read-only status + one control (brightness).
  char     profileName[20] = "";
  int      profileId = 0;
  int      profileCount = 0;       // profiles in use
  char     apSsid[24] = "";
  char     ipStr[20] = "";
  int      devPaired = 0;          // configured signal-source devices
  int      devSeen = 0;            // BLE devices currently discovered
  uint32_t uptimeSec = 0;
  uint32_t freeHeapKb = 0;
  char     version[16] = "";
  uint8_t  brightness = 100;       // 0..100 %, owned by the display task
};

// Pages selectable via the bottom tab bar.
enum Page : uint8_t { PAGE_DASH = 0, PAGE_FLOW, PAGE_GRAPH, PAGE_SETTINGS, PAGE_COUNT };

// Touchable controls on the Settings page.
enum SettingsHit : uint8_t { SET_NONE = 0, SET_BRIGHT_DN, SET_BRIGHT_UP, SET_PROFILE_NEXT };

// Draw the given page (content + tab bar) into the landscape 480x320 canvas.
// Does not flush.
void renderPage(Arduino_GFX* c, Page page, const DashData& d);

// If (tx,ty) (landscape coords) is inside the tab bar, return the tapped page
// (>=0); otherwise return -1.
int tabHitTest(int tx, int ty);

// Hit-test the Settings-page controls (call only when the Settings page is up).
SettingsHit settingsHitTest(int tx, int ty);

// Back-compat: renders PAGE_DASH.
void renderDashboard(Arduino_GFX* c, const DashData& d);

}  // namespace guition
