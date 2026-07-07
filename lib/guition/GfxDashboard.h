// Vicmon master dashboard rendered directly with Arduino_GFX primitives (no
// LVGL). LVGL's flush folds on this AXS15231B canvas for reasons that survived
// exhaustive debugging, while Arduino_GFX fillRect/text/blit render flawlessly.
// The firmware fills a DashData from its resolved signals and calls render().
#pragma once

#include <Arduino_GFX_Library.h>

namespace guition {

// Flat snapshot of everything the dashboard shows (mirrors buildPanelJson()).
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
};

// Pages selectable via the bottom tab bar.
enum Page : uint8_t { PAGE_DASH = 0, PAGE_FLOW, PAGE_GRAPH, PAGE_SETTINGS, PAGE_COUNT };

// Draw the given page (content + tab bar) into the landscape 480x320 canvas.
// Does not flush.
void renderPage(Arduino_GFX* c, Page page, const DashData& d);

// If (tx,ty) (landscape coords) is inside the tab bar, return the tapped page
// (>=0); otherwise return -1.
int tabHitTest(int tx, int ty);

// Back-compat: renders PAGE_DASH.
void renderDashboard(Arduino_GFX* c, const DashData& d);

}  // namespace guition
