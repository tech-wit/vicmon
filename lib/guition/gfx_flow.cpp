// Flow page: animated energy-flow mimic (split from GfxDashboard.cpp, P4).
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

// ------------------------------------------------------------ flow page ----
// A small node (rounded rect) with a title and a value.
static void node(Arduino_GFX* c, int x, int y, int w, int h, uint16_t accent,
                 const char* title, const char* value, bool on) {
  c->fillRoundRect(x, y, w, h, 8, kCard);
  c->drawRoundRect(x, y, w, h, 8, on ? accent : kGrey);
  gtext(c, &FreeSans9pt7b, x + w / 2, y + 20, title, on ? accent : kMuted, C);
  gtext(c, &FreeSans9pt7b, x + w / 2, y + h - 11, value, on ? kText : kMuted, C);
}

// A 3px-thick dot, thickness perpendicular to the segment direction.
static void plotDot(Arduino_GFX* c, int x, int y, bool segHoriz, uint16_t col) {
  if (segHoriz) { c->drawPixel(x, y - 1, col); c->drawPixel(x, y, col); c->drawPixel(x, y + 1, col); }
  else          { c->drawPixel(x - 1, y, col); c->drawPixel(x, y, col); c->drawPixel(x + 1, y, col); }
}

// Orthogonal (right-angle) energy-flow path through pts[0..n-1]. When `on`,
// animated dashes crawl toward the last point — the AP mimic's moving-dash
// "pulse"; off = a static grey line. Every segment must be horizontal or vertical.
static void orthoFlow(Arduino_GFX* c, const int16_t pts[][2], int n, bool on, uint16_t onCol) {
  uint16_t col = on ? onCol : kGrey;
  const int dash = 9, period = 18;
  int phase = on ? (int)(millis() / 45) : 0;  // crawl toward the end while flowing
  int dist = 0;
  for (int i = 1; i < n; ++i) {
    int x0 = pts[i - 1][0], y0 = pts[i - 1][1], x1 = pts[i][0], y1 = pts[i][1];
    bool horiz = (y0 == y1);
    int len = horiz ? abs(x1 - x0) : abs(y1 - y0);
    int sgn = horiz ? (x1 >= x0 ? 1 : -1) : (y1 >= y0 ? 1 : -1);
    for (int k = 0; k <= len; ++k) {
      bool ink = on ? ((((dist + k - phase) % period) + period) % period < dash) : true;
      if (ink) plotDot(c, horiz ? x0 + sgn * k : x0, horiz ? y0 : y0 + sgn * k, horiz, col);
    }
    dist += len;
  }
}

void renderFlow(Arduino_GFX* c, const DashData& d) {
  char v[28];
  // Mode banner — same as the Dash: VICMON badge left, mode centred.
  c->fillRect(0, 0, W, 38, modeColor(d));
  c->fillRoundRect(5, 6, 90, 26, 5, kBlack);
  gtext(c, &FreeSansBold12pt7b, 13, 25, "VICMON", kAccent);
  gtext(c, &FreeSansBold18pt7b, W / 2, 27, d.mode, kBlack, C);

  // Battery in the centre.
  const int batx = 190, baty = 96, batw = 108, bath = 100;
  // Sources on the left.
  const int sx = 12, sw = 128, sh = 54;
  int sy[3] = {60, 124, 188};
  bool solarOn = d.solarValid && d.solarA > 0.1f;
  bool chgOn = d.chargerValid && d.chargerA > 0.1f;
  bool dcOn = d.dcdcValid && d.dcdcOutA > 0.1f;
  // Load on the right.
  const int lx = W - 12 - 120, lw = 120, lh = 70, ly = 116;
  bool loadOn = d.loadValid && d.loadA > 0.1f;

  // Orthogonal flow paths (drawn under the nodes). Sources feed the battery
  // (pulsing green when flowing); the battery feeds the load (pulsing yellow) —
  // matching the AP mimic's colour scheme.
  const int16_t sR = sx + sw, bMid = baty + bath / 2;
  const int16_t solarPts[][2] = {{sR, (int16_t)(sy[0] + sh / 2)}, {165, (int16_t)(sy[0] + sh / 2)}, {165, 120}, {batx, 120}};
  const int16_t chgPts[][2]   = {{sR, (int16_t)(sy[1] + sh / 2)}, {batx, (int16_t)(sy[1] + sh / 2)}};
  const int16_t dcPts[][2]    = {{sR, (int16_t)(sy[2] + sh / 2)}, {165, (int16_t)(sy[2] + sh / 2)}, {165, 172}, {batx, 172}};
  const int16_t loadPts[][2]  = {{(int16_t)(batx + batw), bMid}, {(int16_t)lx, bMid}};
  orthoFlow(c, solarPts, 4, solarOn, kFlowChg);
  orthoFlow(c, chgPts,   2, chgOn,   kFlowChg);
  orthoFlow(c, dcPts,    4, dcOn,    kFlowChg);
  orthoFlow(c, loadPts,  2, loadOn,  kFlowLoad);

  // Source nodes show amps + watts. Solar W is the real PV power from the
  // charger; charger/DC-DC watts are the branch current x battery voltage (no
  // per-branch voltage in the adverts). PV-array voltage isn't advertised, so
  // it's not shown.
  const bool bv = d.battValid;
  if (d.solarValid) snprintf(v, sizeof(v), "%.1fA %.0fW", d.solarA, d.solarW);
  else              snprintf(v, sizeof(v), "--");
  node(c, sx, sy[0], sw, sh, kGold, "Solar", v, solarOn);
  if (d.chargerValid && bv) snprintf(v, sizeof(v), "%.1fA %.0fW", d.chargerA, d.chargerA * d.v);
  else if (d.chargerValid)  snprintf(v, sizeof(v), "%.1fA", d.chargerA);
  else                      snprintf(v, sizeof(v), "--");
  node(c, sx, sy[1], sw, sh, kGreen, "Charger", v, chgOn);
  if (d.dcdcValid && bv) snprintf(v, sizeof(v), "%.1fA %.0fW", d.dcdcOutA, d.dcdcOutA * d.v);
  else if (d.dcdcValid)  snprintf(v, sizeof(v), "%.1fA", d.dcdcOutA);
  else                   snprintf(v, sizeof(v), "--");
  node(c, sx, sy[2], sw, sh, kBlue, "DC-DC", v, dcOn);

  // Battery node (bigger).
  c->fillRoundRect(batx, baty, batw, bath, 10, kCard);
  c->drawRoundRect(batx, baty, batw, bath, 10, modeColor(d));
  if (d.battValid) snprintf(v, sizeof(v), "%.0f%%", d.soc); else snprintf(v, sizeof(v), "--%%");
  uint16_t socColor = !d.battValid ? kMuted : (d.soc >= 50 ? kGreen : (d.soc >= 20 ? kAmber : kRed));
  gtext(c, &FreeSansBold18pt7b, batx + batw / 2, baty + 30, v, socColor, C);
  numOr(v, sizeof(v), d.battValid, d.v, 2, "V");
  gtext(c, &FreeSans9pt7b, batx + batw / 2, baty + 52, v, kText, C);
  numOr(v, sizeof(v), d.battValid, d.a, 1, "A");
  gtext(c, &FreeSans9pt7b, batx + batw / 2, baty + 70, v, d.a >= 0 ? kGreen : kCyan, C);
  if (d.battCapAh > 0 && d.battValid)  // remaining / capacity Ah
    snprintf(v, sizeof(v), "%.0f/%.0f Ah", d.battCapAh * d.soc / 100.0f, d.battCapAh);
  else snprintf(v, sizeof(v), "-- Ah");
  gtext(c, &FreeSans9pt7b, batx + batw / 2, baty + 90, v, kMuted, C);

  // Bottom row, same FreeSans9pt7b muted treatment as the Dash battery-card
  // footer: TTG on the bottom-left, starter voltage in the bottom-middle.
  ttgLabel(v, sizeof(v), d);
  gtext(c, &FreeSans9pt7b, 12, TAB_Y - 10, v, kMuted, L);
  if (d.starterValid) snprintf(v, sizeof(v), "Starter %.1fV", d.starterV);
  else                snprintf(v, sizeof(v), "Starter --");
  gtext(c, &FreeSans9pt7b, W / 2, TAB_Y - 10, v, kMuted, C);

  // Load node (amps + watts = load A x battery V).
  if (d.loadValid && d.battValid) snprintf(v, sizeof(v), "%.1fA %.0fW", d.loadA, d.loadA * d.v);
  else                            numOr(v, sizeof(v), d.loadValid, d.loadA, 1, "A");
  node(c, lx, ly, lw, lh, kRed, d.loadDerived ? "Load*" : "Load", v, loadOn);
}

}  // namespace guition
