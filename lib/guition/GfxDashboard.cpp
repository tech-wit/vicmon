#include "GfxDashboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"

namespace guition {

// Palette (RGB565).
static constexpr uint16_t kBg    = RGB565(0x0b, 0x12, 0x20);
static constexpr uint16_t kCard  = RGB565(0x18, 0x24, 0x38);
static constexpr uint16_t kText  = RGB565(0xe6, 0xed, 0xf3);
static constexpr uint16_t kMuted = RGB565(0x9a, 0xa5, 0xb1);
static constexpr uint16_t kGreen = RGB565(0x3f, 0xb9, 0x50);
static constexpr uint16_t kCyan  = RGB565(0x39, 0xd0, 0xd8);
static constexpr uint16_t kAmber = RGB565(0xd2, 0x99, 0x22);
static constexpr uint16_t kRed   = RGB565(0xf8, 0x51, 0x49);
static constexpr uint16_t kGold  = RGB565(0xf0, 0xb4, 0x29);
static constexpr uint16_t kBlue  = RGB565(0x58, 0xd0, 0xff);
static constexpr uint16_t kGrey  = RGB565(0x30, 0x36, 0x3d);
static constexpr uint16_t kBlack = RGB565(0, 0, 0);

// Trend series colours — matched to the AP chart palette so the LCD Graph and the
// web chart are consistent (batt cyan vs dc-dc violet are now clearly distinct).
static constexpr uint16_t kSerBatt  = RGB565(0x22, 0xd3, 0xee);  // #22d3ee cyan
static constexpr uint16_t kSerSolar = RGB565(0xfa, 0xcc, 0x15);  // #facc15 yellow
static constexpr uint16_t kSerChg   = RGB565(0x60, 0xa5, 0xfa);  // #60a5fa light blue
static constexpr uint16_t kSerDcdc  = RGB565(0xa7, 0x8b, 0xfa);  // #a78bfa violet
static constexpr uint16_t kSerLoad  = RGB565(0xf8, 0x71, 0x71);  // #f87171 red
static constexpr uint16_t kSerSoc   = RGB565(0xf1, 0xf5, 0xf9);  // #f1f5f9 near-white
// AP mimic flow colours.
static constexpr uint16_t kFlowChg  = RGB565(0x34, 0xd3, 0x99);  // #34d399 charging/source green
static constexpr uint16_t kFlowLoad = RGB565(0xfb, 0xbf, 0x24);  // #fbbf24 load yellow

static constexpr int W = 480, H = 320;
static constexpr int TAB_Y = 284, TAB_H = H - TAB_Y;   // bottom tab bar
static constexpr int TAB_W = W / PAGE_COUNT;

enum Align { L, R, C };

static void gtext(Arduino_GFX* c, const GFXfont* f, int x, int y, const char* s,
                  uint16_t color, Align a = L) {
  c->setFont(f);
  c->setTextColor(color);
  if (a != L) {
    int16_t bx, by; uint16_t bw, bh;
    c->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    x = (a == R) ? (x - bw) : (x - bw / 2);
  }
  c->setCursor(x, y);
  c->print(s);
}

static void numOr(char* buf, size_t n, bool valid, float v, int dp, const char* unit) {
  if (!valid) snprintf(buf, n, "--%s", unit);
  else        snprintf(buf, n, "%.*f%s", dp, v, unit);
}

static uint16_t modeColor(const DashData& d) {
  if (d.worst >= 2) return kRed;
  if (d.worst == 1) return kAmber;
  if (strcmp(d.mode, "Charging") == 0) return kGreen;
  if (strcmp(d.mode, "Discharging") == 0) return kRed;
  return kGrey;
}

// ---------------------------------------------------------------- tab bar ----
static void renderTabs(Arduino_GFX* c, Page page) {
  static const char* names[PAGE_COUNT] = {"Dash", "Flow", "Graph", "Week", "Set"};
  c->fillRect(0, TAB_Y, W, TAB_H, kBg);
  c->drawFastHLine(0, TAB_Y, W, kGrey);
  for (int i = 0; i < PAGE_COUNT; ++i) {
    int x = i * TAB_W;
    bool active = (i == page);
    if (active) {
      c->fillRect(x, TAB_Y + 1, TAB_W, TAB_H - 1, kCard);
      c->fillRect(x, TAB_Y, TAB_W, 3, kBlue);
    }
    gtext(c, &FreeSans9pt7b, x + TAB_W / 2, TAB_Y + TAB_H / 2 + 6, names[i],
          active ? kText : kMuted, C);
  }
}

int tabHitTest(int tx, int ty) {
  if (ty < TAB_Y) return -1;
  int i = tx / TAB_W;
  if (i < 0) i = 0;
  if (i >= PAGE_COUNT) i = PAGE_COUNT - 1;
  return i;
}

// ------------------------------------------------------------ dash page ----
static void tile(Arduino_GFX* c, int x, int y, int w, int h, const char* label,
                 uint16_t labelColor, const char* value) {
  c->fillRoundRect(x, y, w, h, 6, kCard);
  gtext(c, &FreeSans9pt7b, x + 10, y + 19, label, labelColor);
  gtext(c, &FreeSansBold12pt7b, x + w - 10, y + h - 11, value, kText, R);
}

static void renderDash(Arduino_GFX* c, const DashData& d) {
  char buf[24];
  // Mode banner
  c->fillRect(0, 0, W, 38, modeColor(d));
  gtext(c, &FreeSansBold18pt7b, W / 2, 27, d.mode, kBlack, C);

  // Battery card (left)
  const int bx = 8, by = 44, bw = 288, bh = 232;
  c->fillRoundRect(bx, by, bw, bh, 10, kCard);
  gtext(c, &FreeSans9pt7b, bx + 14, by + 22, "BATTERY", kMuted);

  if (d.battValid) snprintf(buf, sizeof(buf), "%.0f%%", d.soc);
  else             snprintf(buf, sizeof(buf), "--%%");
  uint16_t socColor = !d.battValid ? kMuted : (d.soc >= 50 ? kGreen : (d.soc >= 20 ? kAmber : kRed));
  gtext(c, &FreeSansBold24pt7b, bx + bw / 2, by + 86, buf, socColor, C);

  const int barx = bx + 16, bary = by + 104, barw = bw - 32, barh = 16;
  c->fillRoundRect(barx, bary, barw, barh, 5, kGrey);
  if (d.battValid) {
    int soc = (int)lroundf(d.soc); if (soc < 0) soc = 0; if (soc > 100) soc = 100;
    int fillw = (barw - 4) * soc / 100;
    if (fillw > 0) c->fillRoundRect(barx + 2, bary + 2, fillw, barh - 4, 4, socColor);
  }

  numOr(buf, sizeof(buf), d.battValid, d.v, 2, "V");
  gtext(c, &FreeSansBold18pt7b, bx + 16, by + 162, buf, kText);
  numOr(buf, sizeof(buf), d.battValid, d.a, 1, "A");
  gtext(c, &FreeSansBold18pt7b, bx + bw - 16, by + 162, buf, d.a >= 0 ? kGreen : kCyan, R);

  if (d.ttgValid && d.ttg > 0) {
    float hh = d.ttg / 60.0f;
    if (hh >= 1) snprintf(buf, sizeof(buf), "TTG %.1fh", hh);
    else         snprintf(buf, sizeof(buf), "TTG %.0fm", d.ttg);
  } else snprintf(buf, sizeof(buf), "TTG --");
  gtext(c, &FreeSans9pt7b, bx + 16, by + bh - 14, buf, kMuted);
  if (d.starterValid) snprintf(buf, sizeof(buf), "Start %.1fV", d.starterV);
  else                snprintf(buf, sizeof(buf), "Start --");
  gtext(c, &FreeSans9pt7b, bx + bw - 16, by + bh - 14, buf, kMuted, R);

  // Source tiles (right)
  const int tx = 304, tw = 168, th = 52, gap = 6;
  int ty = 44;
  char v[28];
  if (d.solarValid) snprintf(v, sizeof(v), "%.0fW %.1fA", d.solarW, d.solarA);
  else              snprintf(v, sizeof(v), "--");
  tile(c, tx, ty, tw, th, "Solar", kGold, v); ty += th + gap;
  numOr(v, sizeof(v), d.chargerValid, d.chargerA, 1, "A");
  tile(c, tx, ty, tw, th, "Charger", kGreen, v); ty += th + gap;
  if (d.dcdcValid && d.dcdcInVValid) snprintf(v, sizeof(v), "%.1fA %.0fV", d.dcdcOutA, d.dcdcInV);
  else if (d.dcdcValid)              snprintf(v, sizeof(v), "%.1fA", d.dcdcOutA);
  else                               snprintf(v, sizeof(v), "--");
  tile(c, tx, ty, tw, th, "DC-DC", kBlue, v); ty += th + gap;
  numOr(v, sizeof(v), d.loadValid, d.loadA, 1, "A");
  tile(c, tx, ty, tw, th, d.loadDerived ? "Load*" : "Load", kRed, v);
}

// ------------------------------------------------------------ flow page ----
// A small node (rounded rect) with a title and a value.
static void node(Arduino_GFX* c, int x, int y, int w, int h, uint16_t accent,
                 const char* title, const char* value, bool on) {
  c->fillRoundRect(x, y, w, h, 8, kCard);
  c->drawRoundRect(x, y, w, h, 8, on ? accent : kGrey);
  gtext(c, &FreeSans9pt7b, x + w / 2, y + 20, title, on ? accent : kMuted, C);
  gtext(c, &FreeSansBold12pt7b, x + w / 2, y + h - 12, value, on ? kText : kMuted, C);
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

static void renderFlow(Arduino_GFX* c, const DashData& d) {
  char v[28];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Energy flow", kText);
  gtext(c, &FreeSansBold18pt7b, W - 12, 28, d.mode, modeColor(d), R);

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

  // Source nodes.
  if (d.solarValid) snprintf(v, sizeof(v), "%.0fW", d.solarW); else snprintf(v, sizeof(v), "--");
  node(c, sx, sy[0], sw, sh, kGold, "Solar", v, solarOn);
  numOr(v, sizeof(v), d.chargerValid, d.chargerA, 1, "A");
  node(c, sx, sy[1], sw, sh, kGreen, "Charger", v, chgOn);
  numOr(v, sizeof(v), d.dcdcValid, d.dcdcOutA, 1, "A");
  node(c, sx, sy[2], sw, sh, kBlue, "DC-DC", v, dcOn);

  // Battery node (bigger).
  c->fillRoundRect(batx, baty, batw, bath, 10, kCard);
  c->drawRoundRect(batx, baty, batw, bath, 10, modeColor(d));
  if (d.battValid) snprintf(v, sizeof(v), "%.0f%%", d.soc); else snprintf(v, sizeof(v), "--%%");
  uint16_t socColor = !d.battValid ? kMuted : (d.soc >= 50 ? kGreen : (d.soc >= 20 ? kAmber : kRed));
  gtext(c, &FreeSansBold18pt7b, batx + batw / 2, baty + 34, v, socColor, C);
  numOr(v, sizeof(v), d.battValid, d.v, 2, "V");
  gtext(c, &FreeSans9pt7b, batx + batw / 2, baty + 60, v, kText, C);
  numOr(v, sizeof(v), d.battValid, d.a, 1, "A");
  gtext(c, &FreeSans9pt7b, batx + batw / 2, baty + 82, v, d.a >= 0 ? kGreen : kCyan, C);

  // Load node.
  numOr(v, sizeof(v), d.loadValid, d.loadA, 1, "A");
  node(c, lx, ly, lw, lh, kRed, d.loadDerived ? "Load*" : "Load", v, loadOn);
}

// ------------------------------------------------------------ graph page ----
// Single-panel trend that mirrors the web AP chart: the five measured currents
// as lines on an auto-scaled Amps axis (left) plus SoC as a dashed line on a
// fixed 0..100 % axis (right), a row of zoom pills across the top, and time
// labels along the bottom.
static const int kGWins[5] = {1, 10, 60, 720, 1440};  // 1m 10m 1h 12h 24h

static void gWinLabel(char* buf, size_t n, int mins) {
  if (mins >= 60) snprintf(buf, n, "%dh", mins / 60);
  else            snprintf(buf, n, "%dm", mins);
}

// Zoom-pill row geometry (top of the page).
static constexpr int GZ_Y = 6, GZ_H = 28, GZ_X0 = 8, GZ_GAP = 6;
static constexpr int GZ_W = (W - 2 * GZ_X0 - 4 * GZ_GAP) / 5;

int graphHitTest(int x, int y) {
  if (y < GZ_Y - 4 || y > GZ_Y + GZ_H + 6) return -1;
  for (int i = 0; i < 5; ++i) {
    int px = GZ_X0 + i * (GZ_W + GZ_GAP);
    if (x >= px && x < px + GZ_W) return kGWins[i];
  }
  return -1;
}

static void renderZoomRow(Arduino_GFX* c, int activeMin) {
  char buf[8];
  for (int i = 0; i < 5; ++i) {
    int px = GZ_X0 + i * (GZ_W + GZ_GAP);
    bool active = (kGWins[i] == activeMin);
    c->fillRoundRect(px, GZ_Y, GZ_W, GZ_H, 7, active ? kBlue : kGrey);
    gWinLabel(buf, sizeof(buf), kGWins[i]);
    gtext(c, &FreeSansBold12pt7b, px + GZ_W / 2, GZ_Y + 20, buf, active ? kBg : kText, C);
  }
}

// Line plot of `vals` (deci-units, -32768 = n/a) into a data rect, mapping
// [vmin,vmax] to the height. Interpolates per screen column so gaps appear over
// missing samples; `dashed` renders as a broken line (used for the SoC overlay).
static void linePlot(Arduino_GFX* c, int px0, int py0, int pw, int ph,
                     const int16_t* vals, int n, float vmin, float vmax,
                     uint16_t col, bool dashed) {
  if (n < 2 || pw < 2 || vmax <= vmin) return;
  auto yOf = [&](float v) -> int {
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    float f = (v - vmin) / (vmax - vmin);
    return py0 + ph - 1 - (int)lroundf(f * (ph - 1));
  };
  int prevY = -1;
  for (int px = 0; px < pw; ++px) {
    if (dashed && ((px >> 2) & 1)) { prevY = -1; continue; }  // ~4px dash / 4px gap
    float fi = (float)px * (n - 1) / (pw - 1);
    int i0 = (int)fi, i1 = i0 + 1;
    if (i1 >= n) i1 = n - 1;
    int16_t a = vals[i0], b = vals[i1];
    if (a == -32768 || b == -32768) { prevY = -1; continue; }
    float frac = fi - i0;
    float v = (a / 10.0f) * (1 - frac) + (b / 10.0f) * frac;
    int y = yOf(v), x = px0 + px;
    if (prevY >= 0) c->drawLine(x - 1, prevY, x, y, col);
    else            c->drawPixel(x, y, col);
    prevY = y;
  }
}

// Graph legend row: 6 fixed-width slots so a tap maps cleanly to a series.
struct Leg { const char* t; uint16_t col; };
static constexpr int GL_Y = 262, GL_X0 = 8;
static constexpr int GL_SLOT = (W - 2 * GL_X0) / 6;

static void renderGraph(Arduino_GFX* c, const DashData& d) {
  char buf[16];
  renderZoomRow(c, d.histWinMin);

  if (d.histCount < 2) {
    c->fillRoundRect(8, 44, W - 16, TAB_Y - 52, 10, kCard);
    gtext(c, &FreeSansBold18pt7b, W / 2, 150, "Collecting data...", kMuted, C);
    gtext(c, &FreeSans9pt7b, W / 2, 178, "trend appears after a minute", kMuted, C);
    return;
  }
  const int n = d.histCount;

  // Plot card + interior data area (left gutter = Amps labels, right = % labels,
  // bottom band = time labels).
  const int LX = 30, RX = 28, BAND = 16;
  const int cardX = LX, cardY = 42, cardW = W - 8 - LX, cardH = 202;
  const int dX = cardX + 4, dW = cardW - 4 - RX;
  const int dY = cardY + 4, dH = cardH - 4 - BAND;
  c->fillRoundRect(cardX, cardY, cardW, cardH, 8, kCard);

  // Auto-scale the Amps axis across all five current series; always span zero.
  float mn = 0, mx = 0;
  auto scan = [&](const int16_t* a) {
    for (int i = 0; i < n; ++i) {
      if (a[i] == -32768) continue;
      float v = a[i] / 10.0f;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
  };
  // Autoscale ignores hidden series (bit i of graphHidden), matching the web.
  if (!(d.graphHidden & (1 << 0))) scan(d.histBatt);
  if (!(d.graphHidden & (1 << 1))) scan(d.histSolar);
  if (!(d.graphHidden & (1 << 2))) scan(d.histCharger);
  if (!(d.graphHidden & (1 << 3))) scan(d.histDcdc);
  if (!(d.graphHidden & (1 << 4))) scan(d.histLoad);
  if (mx - mn < 2) mx = mn + 2;
  auto yA = [&](float v) { return dY + dH - 1 - (int)lroundf((v - mn) / (mx - mn) * (dH - 1)); };
  auto yP = [&](float p) { return dY + dH - 1 - (int)lroundf(p / 100.0f * (dH - 1)); };

  // Horizontal gridlines + Amps labels (left) and % labels (right).
  int dec = (mx - mn) >= 10 ? 0 : 1;
  for (int gi = 0; gi <= 4; ++gi) {
    float val = mn + (mx - mn) * gi / 4;
    int y = yA(val);
    c->drawFastHLine(dX, y, dW, kGrey);
    snprintf(buf, sizeof(buf), "%.*f", dec, val);
    gtext(c, &FreeSans9pt7b, LX - 3, y + 4, buf, kMuted, R);
  }
  if (mn < 0 && mx > 0) c->drawFastHLine(dX, yA(0), dW, kMuted);  // brighter zero line
  for (int p = 0; p <= 100; p += 50) {
    snprintf(buf, sizeof(buf), "%d", p);
    gtext(c, &FreeSans9pt7b, dX + dW + 3, yP(p) + 4, buf, kMuted, L);
  }

  // Vertical time gridlines + interval labels (matches the web's stepMin scheme).
  int win = d.histWinMin;
  int stepMin = win <= 10 ? 1 : (win <= 60 ? 10 : 180);
  int labelY = cardY + cardH - 5;
  for (int t = stepMin; t < win; t += stepMin) {
    float frac = 1.0f - (float)t / win;
    int x = dX + (int)lroundf(frac * (dW - 1));
    c->drawFastVLine(x, dY, dH, kGrey);
    if ((float)dW * stepMin / win >= 34) {
      if (t < 60) snprintf(buf, sizeof(buf), "%dm", t);
      else        snprintf(buf, sizeof(buf), "%dh", t / 60);
      gtext(c, &FreeSans9pt7b, x, labelY, buf, kMuted, C);
    }
  }
  // End labels: oldest (left) .. now (right).
  if (win >= 60) snprintf(buf, sizeof(buf), "-%dh", win / 60);
  else           snprintf(buf, sizeof(buf), "-%dm", win);
  gtext(c, &FreeSans9pt7b, dX + 2, labelY, buf, kMuted, L);
  gtext(c, &FreeSans9pt7b, dX + dW - 2, labelY, "now", kMuted, R);

  // Series lines: five currents on the Amps axis, SoC dashed on the % axis. A
  // hidden series (tapped off in the legend) is skipped.
  uint8_t hid = d.graphHidden;
  if (!(hid & (1 << 0))) linePlot(c, dX, dY, dW, dH, d.histBatt,    n, mn, mx, kSerBatt,  false);
  if (!(hid & (1 << 1))) linePlot(c, dX, dY, dW, dH, d.histSolar,   n, mn, mx, kSerSolar, false);
  if (!(hid & (1 << 2))) linePlot(c, dX, dY, dW, dH, d.histCharger, n, mn, mx, kSerChg,   false);
  if (!(hid & (1 << 3))) linePlot(c, dX, dY, dW, dH, d.histDcdc,    n, mn, mx, kSerDcdc,  false);
  if (!(hid & (1 << 4))) linePlot(c, dX, dY, dW, dH, d.histLoad,    n, mn, mx, kSerLoad,  false);
  if (!(hid & (1 << 5))) linePlot(c, dX, dY, dW, dH, d.histSoc,     n, 0, 100, kSerSoc,   true);

  // Legend in 6 fixed slots (tap a slot to toggle that series on/off).
  static const Leg legs[6] = {{"Batt", kSerBatt}, {"Solar", kSerSolar}, {"Chg", kSerChg},
                              {"DCDC", kSerDcdc}, {"Load", kSerLoad}, {"SoC", kSerSoc}};
  for (int i = 0; i < 6; ++i) {
    int sx = GL_X0 + i * GL_SLOT;
    bool off = hid & (1 << i);
    c->fillRect(sx, GL_Y - 10, 12, 12, off ? kGrey : legs[i].col);
    gtext(c, &FreeSans9pt7b, sx + 16, GL_Y, legs[i].t, off ? kMuted : kText, L);
  }
}

// Hit-test the Graph legend row: returns the series index 0..5 (tap toggles it),
// or -1. Slots must match renderGraph's layout.
int graphLegendHit(int x, int y) {
  if (y < GL_Y - 12 || y > GL_Y + 8) return -1;
  int i = (x - GL_X0) / GL_SLOT;
  return (i >= 0 && i < 6) ? i : -1;
}

// ------------------------------------------------------------- week page ----
// Last-7-days energy: stacked Wh in (solar+dcdc+charger) vs Wh out (load), one
// pair of bars per day. Mirrors the web app's "Last 7 days" chart.
static void renderDays(Arduino_GFX* c, const DashData& d) {
  char buf[40];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Last 7 days", kText);
  snprintf(buf, sizeof(buf), "today  in %.0f  out %.0f Wh",
           d.todaySolarWh + d.todayDcdcWh + d.todayChargerWh, d.todayLoadWh);
  gtext(c, &FreeSans9pt7b, W - 12, 26, buf, kMuted, R);

  const int px = 40, py = 54, pw = (W - 12) - 40, ph = 170;  // plot rect
  c->fillRoundRect(8, 44, W - 16, 210, 10, kCard);           // card 44..254

  if (d.dayCount < 1) {
    gtext(c, &FreeSansBold18pt7b, W / 2, 140, "No completed days yet", kMuted, C);
    gtext(c, &FreeSans9pt7b, W / 2, 168,
          d.clockOk ? "check back after midnight" : "needs WiFi/NTP clock", kMuted, C);
  } else {
    // Scale to the tallest of (stacked-in, out) across the shown days.
    float mx = 1;
    for (int i = 0; i < d.dayCount; ++i) {
      float in = d.daySolarWh[i] + d.dayDcdcWh[i] + d.dayChargerWh[i];
      if (in > mx) mx = in;
      if (d.dayLoadWh[i] > mx) mx = d.dayLoadWh[i];
    }
    auto yOf = [&](float v) { return py + (int)(ph * (1.0f - v / mx)); };
    for (int g = 0; g <= 2; ++g) {                       // gridlines mx, mx/2, 0
      float gv = mx * (2 - g) / 2;
      int gy = yOf(gv);
      c->drawFastHLine(px, gy, pw, kGrey);
      snprintf(buf, sizeof(buf), "%.0f", gv);
      gtext(c, &FreeSans9pt7b, px - 4, gy + 4, buf, kMuted, R);
    }
    int n = d.dayCount;
    float slot = (float)pw / n;
    int bw = (int)(slot * 0.30f); if (bw < 4) bw = 4;
    int base = yOf(0);
    for (int i = 0; i < n; ++i) {
      int cx = px + (int)(slot * (i + 0.5f));
      int xi = cx - bw - 1, xo = cx + 1;
      // stacked IN
      float acc = 0;
      const float vals[3] = {d.daySolarWh[i], d.dayDcdcWh[i], d.dayChargerWh[i]};
      const uint16_t cols[3] = {kGold, kBlue, kGreen};
      for (int s = 0; s < 3; ++s) {
        float v = vals[s]; if (v <= 0) continue;
        int y0 = yOf(acc), y1 = yOf(acc + v);
        c->fillRect(xi, y1, bw, y0 - y1, cols[s]);
        acc += v;
      }
      // OUT (load)
      if (d.dayLoadWh[i] > 0) {
        int yo = yOf(d.dayLoadWh[i]);
        c->fillRect(xo, yo, bw, base - yo, kRed);
      }
      // date label MM/DD
      uint32_t ymd = d.dayStamp[i];
      snprintf(buf, sizeof(buf), "%02u/%02u", (ymd / 100) % 100, ymd % 100);
      gtext(c, &FreeSans9pt7b, cx, py + ph + 14, buf, kMuted, C);
    }
  }
  // Legend (below the card, above the tab bar).
  int lx = 14, ly = 270;
  gtext(c, &FreeSans9pt7b, lx, ly, "In: Solar", kGold);      lx += 82;
  gtext(c, &FreeSans9pt7b, lx, ly, "DC-DC", kBlue);          lx += 60;
  gtext(c, &FreeSans9pt7b, lx, ly, "Charger", kGreen);       lx += 76;
  gtext(c, &FreeSans9pt7b, lx, ly, "Out: Load", kRed);
}

// --------------------------------------------------------- settings page ----
// Layout constants shared by renderSettings() and settingsHit().
static constexpr int PROF_X = 8,   PROF_Y = 66, PROF_W = 200, PROF_RH = 24;  // profile rows
static constexpr int TUN_X = 216,  TUN_Y = 68, TUN_W = W - 8 - 216, TUN_RH = 20;  // tunable rows
static constexpr int ADJ_Y = 232, ADJ_H = 40;                               // -/+ buttons
static constexpr int ADJ_W = (TUN_W - 8) / 2;
static constexpr int PAIR_X = 14, PAIR_Y = 240, PAIR_W = 188, PAIR_H = 30;   // Pair action (Tune)
// Tunables a slave shows on Tune (display-relevant only; the rest are master alert
// settings). render + hit-test share this ordering.
static const uint8_t kSlaveTun[2] = {TUN_BRIGHT, TUN_TZ};

struct TunInfo { const char* label; const char* unit; };
static const TunInfo kTun[TUNABLE_N] = {
  {"Brightness", "%"}, {"Battery cap", "Ah"}, {"Deadband", "A"}, {"Timezone", "h"},
  {"SoC warn", "%"},   {"SoC crit", "%"},     {"Volt low", "V"}, {"Volt high", "V"},
};

static float tunValue(const DashData& d, int i) {
  switch (i) {
    case TUN_BRIGHT:   return d.brightness;
    case TUN_BATTCAP:  return d.battCapAh;
    case TUN_DEADBAND: return d.deadbandA;
    case TUN_TZ:       return d.tzMin / 60.0f;
    case TUN_SOCWARN:  return d.socWarn;
    case TUN_SOCCRIT:  return d.socCrit;
    case TUN_VLOW:     return d.vLow;
    case TUN_VHIGH:    return d.vHigh;
  }
  return 0;
}

static void tunText(char* buf, size_t n, int i, float v) {
  switch (i) {
    case TUN_BRIGHT:  snprintf(buf, n, "%.0f%%", v); break;
    case TUN_BATTCAP: (v <= 0) ? snprintf(buf, n, "auto") : snprintf(buf, n, "%.0f Ah", v); break;
    case TUN_DEADBAND:snprintf(buf, n, "%.2f A", v); break;
    case TUN_TZ:      snprintf(buf, n, "%+.1f h", v); break;
    case TUN_SOCWARN:
    case TUN_SOCCRIT: snprintf(buf, n, "%.0f%%", v); break;
    default:          snprintf(buf, n, "%.1f V", v); break;
  }
}

static void button(Arduino_GFX* c, int x, int y, int w, int h, const char* label,
                   uint16_t accent) {
  c->fillRoundRect(x, y, w, h, 8, kGrey);
  c->drawRoundRect(x, y, w, h, 8, accent);
  gtext(c, &FreeSansBold18pt7b, x + w / 2, y + h / 2 + 8, label, kText, C);
}

// --- Settings sub-view toggle (Tune | Bind) + signal-bindings editor --------
// The Bindings view mirrors the web app's "Panel signals" section: each logical
// role shows its current source; tapping a row asks the firmware to cycle it to
// the next available device+field (the firmware owns the option list + NVS).
static constexpr int SV_W = 74, SV_H = 34, SV_Y = 6;
static constexpr int SV_DIAG_X = W - 8 - SV_W;
static constexpr int SV_BIND_X = SV_DIAG_X - 6 - SV_W;
static constexpr int SV_TUNE_X = SV_BIND_X - 6 - SV_W;

// Short role labels (order matches sig::Role / the firmware's bindLabel[]).
static const char* kRoleNames[ROLE_N] = {
  "Battery SoC", "Battery V",  "Battery A",   "Consumed Ah", "Starter V",   "Time to go",
  "Solar A",     "Solar W",    "Charger A",   "DC-DC in A",  "DC-DC out A", "Load A",
};

// Big, touch-friendly rows; the list + picker paginate BIND_PERPAGE per page.
static constexpr int BIND_TOP = 44, BIND_ROWH = 38;

// Page nav = up/down arrow buttons in a reserved column down the right edge
// (up = previous page, down = next). Rows shrink to `rowRight()` to clear it.
static constexpr int NAVCOL_W = 46;
static constexpr int NAVCOL_X = W - 8 - NAVCOL_W;
static constexpr int NAV_UP_Y = 44, NAV_UP_H = 112;
static constexpr int NAV_DN_Y = 164, NAV_DN_H = 112;

static void renderViewToggle(Arduino_GFX* c, int view, int role) {
  const char* names[3] = {"Tune", "Bind", "Diag"};
  int xs[3] = {SV_TUNE_X, SV_BIND_X, SV_DIAG_X};
  for (int i = 0; i < 3; ++i) {
    if (role == 1 && i == 1) continue;  // slave has no Bind — hide the pill
    bool active = (i == view);
    c->fillRoundRect(xs[i], SV_Y, SV_W, SV_H, 7, active ? kBlue : kGrey);
    gtext(c, &FreeSansBold12pt7b, xs[i] + SV_W / 2, SV_Y + 23, names[i], active ? kBg : kText, C);
  }
}

// Right edge of the list/picker rows: full width, or shy of the nav column when
// the content paginates.
static int rowRight(int total) { return total > 1 ? NAVCOL_X - 6 : W - 6; }

// Up/down page buttons + a small "cur/total" label (drawn only when >1 page).
static void renderNav(Arduino_GFX* c, int cur, int total) {
  if (total <= 1) return;
  bool hasUp = cur > 0, hasDn = cur < total - 1;
  int cx = NAVCOL_X + NAVCOL_W / 2;
  c->fillRoundRect(NAVCOL_X, NAV_UP_Y, NAVCOL_W, NAV_UP_H, 8, hasUp ? kGrey : kCard);
  uint16_t uc = hasUp ? kText : kMuted;
  int uy = NAV_UP_Y + NAV_UP_H / 2;
  c->fillTriangle(cx, uy - 13, cx - 15, uy + 9, cx + 15, uy + 9, uc);
  c->fillRoundRect(NAVCOL_X, NAV_DN_Y, NAVCOL_W, NAV_DN_H, 8, hasDn ? kGrey : kCard);
  uint16_t dc = hasDn ? kText : kMuted;
  int dy = NAV_DN_Y + NAV_DN_H / 2;
  c->fillTriangle(cx, dy + 13, cx - 15, dy - 9, cx + 15, dy - 9, dc);
  char b[8];
  snprintf(b, sizeof(b), "%d/%d", cur + 1, total);
  gtext(c, &FreeSans9pt7b, cx, NAV_UP_Y + NAV_UP_H + 12, b, kMuted, C);
}

static int navHit(int x, int y) {
  if (x < NAVCOL_X || x >= NAVCOL_X + NAVCOL_W) return -1;
  if (y >= NAV_UP_Y && y < NAV_UP_Y + NAV_UP_H) return 0;  // up = prev page
  if (y >= NAV_DN_Y && y < NAV_DN_Y + NAV_DN_H) return 1;  // down = next page
  return -1;
}

int bindListPages() { return (ROLE_N + BIND_PERPAGE - 1) / BIND_PERPAGE; }

static void renderBindings(Arduino_GFX* c, const DashData& d) {
  int total = bindListPages();
  renderNav(c, d.bindPage, total);
  int rowR = rowRight(total);
  int base = d.bindPage * BIND_PERPAGE;
  for (int s = 0; s < BIND_PERPAGE; ++s) {
    int role = base + s;
    if (role >= ROLE_N) break;
    int y = BIND_TOP + s * BIND_ROWH;
    c->fillRoundRect(6, y, rowR - 6, BIND_ROWH - 6, 6, kCard);
    gtext(c, &FreeSansBold12pt7b, 16, y + 25, kRoleNames[role], kText, L);
    const char* v = d.bindLabel[role];
    bool none = (v[0] == '\0' || strncmp(v, "-- none", 7) == 0);
    gtext(c, &FreeSans9pt7b, rowR - 10, y + 25, none ? "-- none --" : v, none ? kMuted : kBlue, R);
  }
}

int settingsViewHit(int x, int y) {
  if (y < SV_Y || y >= SV_Y + SV_H) return -1;
  if (x >= SV_TUNE_X && x < SV_TUNE_X + SV_W) return 0;
  if (x >= SV_BIND_X && x < SV_BIND_X + SV_W) return 1;
  if (x >= SV_DIAG_X && x < SV_DIAG_X + SV_W) return 2;
  return -1;
}

int bindingHit(int x, int y) {
  int nv = navHit(x, y);
  if (nv == 0) return -2;  // prev page
  if (nv == 1) return -3;  // next page
  if (x < 6 || x >= W - 6 || y < BIND_TOP) return -1;
  int s = (y - BIND_TOP) / BIND_ROWH;
  return (s >= 0 && s < BIND_PERPAGE) ? s : -1;
}

// --- Source-picker menu (opened by tapping a signal in the Bindings list) ----
// The two "derived" sources (shared indices 1 and 2) only apply to the current-
// flow roles, mirroring the web dropdown.
static bool isCurrentRole(int r) {
  return r == 6 || r == 8 || r == 9 || r == 10 || r == 11;  // Solar/Charger/DcDcIn/DcDcOut/Load A
}

int bindVisible(int role, int srcCount, int* out, int max) {
  int n = 0;
  auto add = [&](int s) { if (n < max) out[n++] = s; };
  add(0);                                   // -- none --
  if (isCurrentRole(role)) { add(1); add(2); }  // derived charge / load
  for (int s = 3; s < srcCount; ++s) add(s);    // device + field sources
  return n;
}

static void renderBindMenu(Arduino_GFX* c, const DashData& d) {
  int role = d.menuRole;
  gtext(c, &FreeSansBold18pt7b, 12, 28, kRoleNames[role], kText);
  // Back pill (top-right, where the view toggle sits).
  c->fillRoundRect(SV_BIND_X, SV_Y, SV_W, SV_H, 7, kGrey);
  gtext(c, &FreeSansBold12pt7b, SV_BIND_X + SV_W / 2, SV_Y + 23, "Back", kText, C);

  int vis[BIND_MAXSRC];
  int vc = bindVisible(role, d.srcCount, vis, BIND_MAXSRC);
  int pages = (vc + BIND_PERPAGE - 1) / BIND_PERPAGE;
  if (pages < 1) pages = 1;
  int cur = d.menuPage < pages ? d.menuPage : pages - 1;
  renderNav(c, cur, pages);

  int rowR = rowRight(pages);
  int curShared = (role >= 0 && role < ROLE_N) ? d.bindIdx[role] : -1;
  int base = cur * BIND_PERPAGE;
  for (int s = 0; s < BIND_PERPAGE; ++s) {
    int idx = base + s;
    if (idx >= vc) break;
    int shared = vis[idx];
    int y = BIND_TOP + s * BIND_ROWH;
    bool sel = (shared == curShared);
    c->fillRoundRect(6, y, rowR - 6, BIND_ROWH - 6, 6, sel ? kBlue : kCard);
    gtext(c, &FreeSansBold12pt7b, 16, y + 25, d.srcLabels[shared], sel ? kBg : kText, L);
  }
}

int bindMenuHit(int x, int y) {
  if (x >= SV_BIND_X && x < SV_BIND_X + SV_W && y >= SV_Y && y < SV_Y + SV_H) return -2;  // Back
  int nv = navHit(x, y);
  if (nv == 0) return -3;  // prev page
  if (nv == 1) return -4;  // next page
  if (x < 6 || x >= W - 6 || y < BIND_TOP) return -1;
  int s = (y - BIND_TOP) / BIND_ROWH;
  return (s >= 0 && s < BIND_PERPAGE) ? s : -1;
}

// --- Settings > Diagnostics sub-view (ESP-NOW / pairing + devices + role) ----
// The Diag sub-view is a small menu (screen 0) that opens one of several screens.
// Menu rows, a shared bottom Back button, and an in-screen control button.
static constexpr int DM_TOP = 52, DM_RH = 42, DM_GAP = 8, DM_X = 8, DM_W = W - 16;
static constexpr int DBACK_H = 34, DBACK_Y = TAB_Y - DBACK_H - 6, DBACK_X = 8, DBACK_W = W - 16;
static constexpr int DCTL_Y = 52, DCTL_H = 42, DCTL_X = 8, DCTL_W = W - 16;
static constexpr int DROLE_BTN_Y = 116, DROLE_BTN_H = 46;
static constexpr int DLINK_UNPAIR_Y = 196;  // slave Link screen: Unpair button

static void diagBtn(Arduino_GFX* c, int x, int y, int w, int h, const char* label,
                    uint16_t bg, uint16_t fg) {
  c->fillRoundRect(x, y, w, h, 7, bg);
  gtext(c, &FreeSansBold12pt7b, x + w / 2, y + h / 2 + 7, label, fg, C);
}

// Menu structure — role-dependent. Master: Monitored/Discovered/Debug/Role.
// Slave (no BLE): Link/Role. render + hit-test share this ordering.
static int diagMenuCount(int role) { return role == 1 ? 2 : 4; }
static DiagAction diagMenuAction(int role, int i) {
  if (role == 1) return i == 0 ? DIAG_OPEN_LINK : DIAG_OPEN_ROLE;
  switch (i) {
    case 0:  return DIAG_OPEN_MON;
    case 1:  return DIAG_OPEN_DISC;
    case 2:  return DIAG_OPEN_DEBUG;
    default: return DIAG_OPEN_ROLE;
  }
}
static void diagMenuLabel(const DashData& d, int i, char* out, size_t n) {
  if (d.role == 1) { snprintf(out, n, i == 0 ? "Link status" : "Switch to Master"); return; }
  switch (i) {
    case 0:  snprintf(out, n, "Monitored (%d)", d.monCount); break;
    case 1:  snprintf(out, n, "Discovered (%d)", d.discCount); break;
    case 2:  snprintf(out, n, d.debugCapture ? "Debug capture: ON" : "Debug capture: OFF"); break;
    default: snprintf(out, n, "Switch to Slave"); break;
  }
}

static void diagBack(Arduino_GFX* c) {
  diagBtn(c, DBACK_X, DBACK_Y, DBACK_W, DBACK_H, "< Back", kGrey, kText);
}

static void renderDiagMenu(Arduino_GFX* c, const DashData& d) {
  char buf[32];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Diagnostics", kText);
  int cnt = diagMenuCount(d.role);
  for (int i = 0; i < cnt; ++i) {
    int y = DM_TOP + i * (DM_RH + DM_GAP);
    c->fillRoundRect(DM_X, y, DM_W, DM_RH, 8, kCard);
    diagMenuLabel(d, i, buf, sizeof(buf));
    gtext(c, &FreeSansBold12pt7b, DM_X + 18, y + DM_RH / 2 + 7, buf, kText, L);
    gtext(c, &FreeSansBold18pt7b, DM_X + DM_W - 18, y + DM_RH / 2 + 9, ">", kMuted, R);
  }
}

static void renderDiagMon(Arduino_GFX* c, const DashData& d) {
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Monitored", kText);
  int y = 46;
  for (int i = 0; i < d.monCount && i < DashData::MON_N; ++i) {
    if (y + 30 > DBACK_Y - 4) break;
    c->fillRoundRect(8, y, W - 16, 28, 6, kCard);
    c->fillCircle(23, y + 14, 5, d.monLive[i] ? kGreen : kRed);
    gtext(c, &FreeSansBold12pt7b, 38, y + 19, d.monName[i], kText);
    gtext(c, &FreeSans9pt7b, 200, y + 19, d.monType[i], kMuted);
    gtext(c, &FreeSans9pt7b, W - 14, y + 19, d.monVal[i], kText, R);
    y += 32;
  }
  if (d.monCount == 0) gtext(c, &FreeSans9pt7b, 12, 64, "No configured devices.", kMuted);
  diagBack(c);
}

static void renderDiagDisc(Arduino_GFX* c, const DashData& d) {
  char buf[48];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Discovered", kText);
  if (d.debugCapture) gtext(c, &FreeSans9pt7b, W - 12, 24, "debug on", kBlue, R);
  int y = 46;
  for (int i = 0; i < d.discCount && i < DashData::DISC_N; ++i) {
    bool raw = d.debugCapture && d.discRaw[i][0];
    int rh = raw ? 38 : 28;
    if (y + rh > DBACK_Y - 4) break;
    c->fillRoundRect(8, y, W - 16, rh, 6, kCard);
    gtext(c, &FreeSansBold12pt7b, 16, y + 19, d.discName[i][0] ? d.discName[i] : "(unnamed)", kText);
    snprintf(buf, sizeof(buf), "%s 0x%04X %ddBm", d.discMac[i], d.discModel[i], d.discRssi[i]);
    gtext(c, &FreeSans9pt7b, W - 14, y + 19, buf, kMuted, R);
    if (raw) { snprintf(buf, sizeof(buf), "raw %s", d.discRaw[i]); gtext(c, &FreeSans9pt7b, 16, y + 33, buf, kBlue); }
    y += rh + 4;
  }
  if (d.discCount == 0) gtext(c, &FreeSans9pt7b, 12, 64, "No unknown devices nearby.", kMuted);
  diagBack(c);
}

static void renderDiagDebug(Arduino_GFX* c, const DashData& d) {
  char buf[48];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Debug", kText);
  diagBtn(c, DCTL_X, DCTL_Y, DCTL_W, DCTL_H,
          d.debugCapture ? "Debug capture: ON" : "Debug capture: OFF",
          d.debugCapture ? kGreen : kGrey, d.debugCapture ? kBg : kText);
  int y = DCTL_Y + DCTL_H + 24;
  gtext(c, &FreeSans9pt7b, 12, y, "Captures raw bytes of unknown Victron adverts", kMuted); y += 20;
  gtext(c, &FreeSans9pt7b, 12, y, "(shown on the Discovered screen).", kMuted); y += 28;
  snprintf(buf, sizeof(buf), "Devices set / seen:  %d / %d", d.devPaired, d.devSeen);
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText); y += 20;
  snprintf(buf, sizeof(buf), "Free heap:  %u KB", d.freeHeapKb);
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText); y += 20;
  snprintf(buf, sizeof(buf), "Firmware:  %s", d.version[0] ? d.version : "--");
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText); y += 20;
  snprintf(buf, sizeof(buf), "Master id %08X  seq %u", d.masterId, d.snapSeq);
  gtext(c, &FreeSans9pt7b, 12, y, buf, kMuted);
  diagBack(c);
}

static void renderDiagRole(Arduino_GFX* c, const DashData& d) {
  char buf[48];
  bool slave = (d.role == 1);
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Role", kText);
  snprintf(buf, sizeof(buf), "Currently: %s", slave ? "SLAVE" : "MASTER");
  gtext(c, &FreeSansBold12pt7b, 12, 76, buf, slave ? kBlue : kGreen);
  gtext(c, &FreeSans9pt7b, 12, 100, "The device reboots into the other role.", kMuted);
  diagBtn(c, DCTL_X, DROLE_BTN_Y, DCTL_W, DROLE_BTN_H,
          slave ? "Switch to Master" : "Switch to Slave", kBlue, kBg);
  diagBack(c);
}

static void renderDiagLink(Arduino_GFX* c, const DashData& d) {  // slave role
  char buf[48];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Link", kText);
  int y = 72;
  if (!d.masterId) {
    gtext(c, &FreeSansBold12pt7b, 12, y, "Not paired", kText); y += 28;
    gtext(c, &FreeSans9pt7b, 12, y, "Pair from the Tune screen (Pair button).", kMuted); y += 22;
    gtext(c, &FreeSans9pt7b, 12, y,
          d.heardInvite ? "A master is inviting pairing now." : "Listening for a master...",
          d.heardInvite ? kGreen : kMuted);
  } else {
    snprintf(buf, sizeof(buf), "Master %08X", d.masterId);
    gtext(c, &FreeSansBold12pt7b, 12, y, buf, kText); y += 28;
    gtext(c, &FreeSans9pt7b, 12, y, d.linkLive ? "Receiving live" : "Stale / out of range",
          d.linkLive ? kGreen : kAmber); y += 22;
    snprintf(buf, sizeof(buf), "Channel %u    dropped %lu", d.linkChannel, (unsigned long)d.linkDrops);
    gtext(c, &FreeSans9pt7b, 12, y, buf, kMuted);
    diagBtn(c, DCTL_X, DLINK_UNPAIR_Y, DCTL_W, DBACK_H, "Unpair", kGrey, kText);
  }
  diagBack(c);
}

static void renderDiag(Arduino_GFX* c, const DashData& d) {
  switch (d.diagScreen) {
    case DS_MON:   renderDiagMon(c, d); break;
    case DS_DISC:  renderDiagDisc(c, d); break;
    case DS_DEBUG: renderDiagDebug(c, d); break;
    case DS_ROLE:  renderDiagRole(c, d); break;
    case DS_LINK:  renderDiagLink(c, d); break;
    default:       renderDiagMenu(c, d); break;
  }
}

int diagHit(int x, int y, int role, int screen) {
  if (screen == DS_MENU) {
    for (int i = 0; i < diagMenuCount(role); ++i) {
      int ry = DM_TOP + i * (DM_RH + DM_GAP);
      if (y >= ry && y < ry + DM_RH && x >= DM_X && x < DM_X + DM_W) return diagMenuAction(role, i);
    }
    return DIAG_NONE;
  }
  if (y >= DBACK_Y && y < DBACK_Y + DBACK_H && x >= DBACK_X && x < DBACK_X + DBACK_W) return DIAG_BACK;
  if (screen == DS_DEBUG && y >= DCTL_Y && y < DCTL_Y + DCTL_H && x >= DCTL_X && x < DCTL_X + DCTL_W)
    return DIAG_DEBUG_TOGGLE;
  if (screen == DS_ROLE && y >= DROLE_BTN_Y && y < DROLE_BTN_Y + DROLE_BTN_H &&
      x >= DCTL_X && x < DCTL_X + DCTL_W)
    return DIAG_ROLE_TOGGLE;
  if (screen == DS_LINK && y >= DLINK_UNPAIR_Y && y < DLINK_UNPAIR_Y + DBACK_H &&
      x >= DCTL_X && x < DCTL_X + DCTL_W)
    return DIAG_UNPAIR;
  return DIAG_NONE;
}

// Placeholder for pages that need data the ESP-NOW frame doesn't carry (slave).
static void renderNA(Arduino_GFX* c, const char* what) {
  gtext(c, &FreeSansBold18pt7b, W / 2, 140, "Master only", kMuted, C);
  gtext(c, &FreeSans9pt7b, W / 2, 172, what, kMuted, C);
}

static void renderSettings(Arduino_GFX* c, const DashData& d) {
  int view = d.setView;
  if (d.role == 1 && view == 1) view = 0;  // slave has no signal bindings -> Tune
  // Bindings view has a full-screen source picker when a signal is selected.
  if (view == 1 && d.menuRole >= 0 && d.menuRole < ROLE_N) {
    renderBindMenu(c, d);
    return;
  }
  renderViewToggle(c, view, d.role);
  if (view == 2) { renderDiag(c, d); return; }  // Diag draws its own per-screen header
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Settings", kText);
  if (view == 1) { renderBindings(c, d); return; }

  char buf[48];
  bool slave = (d.role == 1);
  int ntun = slave ? 2 : TUNABLE_N;

  // --- Left column: profiles (master) or link status (slave) + shared status ---
  c->fillRoundRect(8, 44, 200, TAB_Y - 52, 10, kCard);
  int sy;
  if (!slave) {
    gtext(c, &FreeSans9pt7b, 18, 60, "PROFILES", kMuted);
    for (int i = 0; i < 4; ++i) {
      int ry = PROF_Y + i * PROF_RH;
      if (!d.profUsed[i]) continue;
      bool active = (i == d.profileId);
      if (active) c->fillRoundRect(PROF_X + 6, ry, PROF_W - 12, PROF_RH - 2, 5, kGrey);
      c->fillCircle(PROF_X + 16, ry + (PROF_RH - 2) / 2, 4, active ? kGreen : kMuted);
      gtext(c, &FreeSans9pt7b, PROF_X + 28, ry + 15, d.profNames[i], active ? kText : kMuted);
    }
    sy = PROF_Y + 4 * PROF_RH + 12;
  } else {
    gtext(c, &FreeSans9pt7b, 18, 60, "LINK", kMuted);
    if (d.masterId) {
      snprintf(buf, sizeof(buf), "Master %08X", d.masterId);
      gtext(c, &FreeSans9pt7b, 18, 82, buf, kText);
      gtext(c, &FreeSans9pt7b, 18, 102, d.linkLive ? "receiving live" : "stale / no signal",
            d.linkLive ? kGreen : kAmber);
      snprintf(buf, sizeof(buf), "ch %u   drops %lu", d.linkChannel, (unsigned long)d.linkDrops);
      gtext(c, &FreeSans9pt7b, 18, 122, buf, kMuted);
    } else {
      gtext(c, &FreeSans9pt7b, 18, 82, "Not paired", kText);
      gtext(c, &FreeSans9pt7b, 18, 102, "tap Pair below", kMuted);
    }
    sy = 148;
  }
  // Shared status lines.
  const int dy = 15;
  auto stat = [&](const char* label, const char* val) {
    gtext(c, &FreeSans9pt7b, 18, sy, label, kMuted);
    gtext(c, &FreeSans9pt7b, 200, sy, val, kText, R);
    sy += dy;
  };
  stat("AP", d.apSsid);
  stat("Pass", d.apPass);
  stat("IP", d.ipStr);
  uint32_t up = d.uptimeSec;
  if (up >= 86400) snprintf(buf, sizeof(buf), "%ud %uh", up / 86400, (up % 86400) / 3600);
  else if (up >= 3600) snprintf(buf, sizeof(buf), "%uh %um", up / 3600, (up % 3600) / 60);
  else snprintf(buf, sizeof(buf), "%um", up / 60);
  stat("Uptime", buf);  // dev/heap/firmware moved to Diag > Debug
  // Pair action: master opens the pairing window, slave adopts.
  char pb[24];
  if (d.pairing) snprintf(pb, sizeof(pb), "%s %ds", slave ? "Adopting" : "Pairing", d.pairSecLeft);
  else           snprintf(pb, sizeof(pb), "%s", slave ? "Pair" : "Pair slave");
  diagBtn(c, PAIR_X, PAIR_Y, PAIR_W, PAIR_H, pb, d.pairing ? kAmber : kBlue, kBg);

  // --- Right column: tunables (slave shows only display-relevant ones) ---
  c->fillRoundRect(TUN_X, 44, TUN_W, TAB_Y - 52, 10, kCard);
  gtext(c, &FreeSans9pt7b, TUN_X + 12, 60, "ADJUST", kMuted);
  for (int r = 0; r < ntun; ++r) {
    int i = slave ? kSlaveTun[r] : r;
    int ry = TUN_Y + r * TUN_RH;
    bool sel = (i == d.setSel);
    if (sel) c->fillRoundRect(TUN_X + 6, ry, TUN_W - 12, TUN_RH - 2, 4, kGrey);
    gtext(c, &FreeSans9pt7b, TUN_X + 14, ry + 15, kTun[i].label, sel ? kText : kMuted);
    tunText(buf, sizeof(buf), i, tunValue(d, i));
    gtext(c, &FreeSansBold12pt7b, TUN_X + TUN_W - 14, ry + 16, buf, sel ? kBlue : kText, R);
  }
  button(c, TUN_X, ADJ_Y, ADJ_W, ADJ_H, "-", kBlue);
  button(c, TUN_X + ADJ_W + 8, ADJ_Y, ADJ_W, ADJ_H, "+", kBlue);
}

SettingsHitResult settingsHit(int x, int y, int role) {
  auto in = [&](int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
  };
  int ntun = role == 1 ? 2 : TUNABLE_N;
  // Pair action + adjust buttons.
  if (in(PAIR_X, PAIR_Y, PAIR_W, PAIR_H)) return {SA_PAIR, 0};
  if (in(TUN_X, ADJ_Y, ADJ_W, ADJ_H)) return {SA_ADJ_DN, 0};
  if (in(TUN_X + ADJ_W + 8, ADJ_Y, ADJ_W, ADJ_H)) return {SA_ADJ_UP, 0};
  // Tunable rows (slave list = brightness + timezone only).
  if (in(TUN_X, TUN_Y, TUN_W, ntun * TUN_RH)) {
    int r = (y - TUN_Y) / TUN_RH;
    if (r >= 0 && r < ntun) return {SA_SELECT_ROW, role == 1 ? (int)kSlaveTun[r] : r};
  }
  // Profile rows (master only).
  if (role == 0 && in(PROF_X, PROF_Y, PROF_W, 4 * PROF_RH)) {
    int row = (y - PROF_Y) / PROF_RH;
    if (row >= 0 && row < 4) return {SA_PROFILE, row};
  }
  return {SA_NONE, 0};
}

// -------------------------------------------------------------- dispatch ----
void renderPage(Arduino_GFX* c, Page page, const DashData& d) {
  c->fillScreen(kBg);
  switch (page) {
    case PAGE_FLOW:     renderFlow(c, d); break;
    case PAGE_GRAPH:    renderGraph(c, d); break;  // slave builds history from received frames
    case PAGE_DAYS:     if (d.role == 1) renderNA(c, "Week needs the master"); else renderDays(c, d); break;
    case PAGE_SETTINGS: renderSettings(c, d); break;
    case PAGE_DASH:
    default:            renderDash(c, d); break;
  }
  renderTabs(c, page);
  c->setFont(nullptr);
}

void renderDashboard(Arduino_GFX* c, const DashData& d) { renderPage(c, PAGE_DASH, d); }

}  // namespace guition
