// Graph page: trend chart + zoom/legend hit-tests (split from GfxDashboard.cpp, P4).
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

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

void renderZoomRow(Arduino_GFX* c, int activeMin) {
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

void renderGraph(Arduino_GFX* c, const DashData& d) {
  char buf[16];
  renderZoomRow(c, d.histWinMin);
  // Drawn last (on top of the plot), a little below the zoom pills, while the
  // history is streaming in from the master.
  auto drawSync = [&]() {
    if (!d.graphSyncing) return;
    char s[40];
    snprintf(s, sizeof(s), "syncing with master  %u%%", d.graphSyncPct);
    c->fillRoundRect(W / 2 - 98, 54, 196, 18, 5, kBg);
    gtext(c, &FreeSans9pt7b, W / 2, 67, s, kAccent, C);
  };

  const int n = d.histCount;

  // Plot card + interior data area (left gutter = Amps labels, right = % labels,
  // bottom band = time labels). Always drawn — even empty — so the graph just
  // populates as points arrive rather than showing a "collecting" placeholder.
  const int LX = 30, RX = 28, BAND = 16;
  const int cardX = LX, cardY = 42, cardW = W - 8 - LX, cardH = 202;
  const int dX = cardX + 4, dW = cardW - 4 - RX;
  const int dY = cardY + 4, dH = cardH - 4 - BAND;
  c->fillRoundRect(cardX, cardY, cardW, cardH, 8, kCard);

  if (n < 2) {  // empty frame — bare gridlines, no message; fills in as data arrives
    for (int gi = 0; gi <= 4; ++gi) {
      int y = dY + dH - 1 - (int)lroundf((float)gi / 4 * (dH - 1));
      c->drawFastHLine(dX, y, dW, kGrey);
    }
    gtext(c, &FreeSans9pt7b, dX + dW - 2, cardY + cardH - 5, "now", kMuted, R);
    drawSync();
    return;
  }

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
  drawSync();  // overlay on top of the plotted series
}

// Hit-test the Graph legend row: returns the series index 0..5 (tap toggles it),
// or -1. Slots must match renderGraph's layout.
int graphLegendHit(int x, int y) {
  if (y < GL_Y - 12 || y > GL_Y + 8) return -1;
  int i = (x - GL_X0) / GL_SLOT;
  return (i >= 0 && i < 6) ? i : -1;
}

}  // namespace guition
