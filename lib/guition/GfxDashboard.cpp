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

// Thick flow line between two points; green when energy is flowing.
static void flow(Arduino_GFX* c, int x0, int y0, int x1, int y1, bool on) {
  uint16_t col = on ? kGreen : kGrey;
  for (int o = -1; o <= 1; ++o) c->drawLine(x0, y0 + o, x1, y1 + o, col);
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

  // Flow lines first (under the nodes' reach).
  flow(c, sx + sw, sy[0] + sh / 2, batx, baty + 24, solarOn);
  flow(c, sx + sw, sy[1] + sh / 2, batx, baty + bath / 2, chgOn);
  flow(c, sx + sw, sy[2] + sh / 2, batx, baty + bath - 24, dcOn);
  flow(c, batx + batw, baty + bath / 2, lx, ly + lh / 2, loadOn);

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
// Dim fill variants for the area charts.
static constexpr uint16_t kFillGreen = RGB565(0x14, 0x3a, 0x22);
static constexpr uint16_t kFillRed   = RGB565(0x45, 0x1a, 0x1c);

struct Plot { int x, y, w, h; };

// Filled area plot of `vals` (deci-units, -32768 = n/a) into a rect. Values map
// linearly from [vmin,vmax] (real units) to the plot height; the fill runs to
// `baseline`, green where above it and red below. Interpolates per screen column
// so the fill is solid, and gaps appear across missing samples.
static void areaPlot(Arduino_GFX* c, const Plot& p, const int16_t* vals, int n,
                     float vmin, float vmax, float baseline,
                     uint16_t lineCol, uint16_t fillPos, uint16_t fillNeg) {
  if (n < 2 || vmax <= vmin) return;
  auto yOf = [&](float v) -> int {
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    float f = (v - vmin) / (vmax - vmin);
    return p.y + p.h - 1 - (int)lroundf(f * (p.h - 1));
  };
  int ybase = yOf(baseline);
  int prevTop = -1;
  for (int px = 0; px < p.w; ++px) {
    float fi = (float)px * (n - 1) / (p.w - 1);
    int i0 = (int)fi, i1 = i0 + 1;
    if (i1 >= n) i1 = n - 1;
    int16_t a = vals[i0], b = vals[i1];
    if (a == -32768 || b == -32768) { prevTop = -1; continue; }
    float frac = fi - i0;
    float v = (a / 10.0f) * (1 - frac) + (b / 10.0f) * frac;
    int top = yOf(v);
    int x = p.x + px;
    if (top <= ybase) c->drawFastVLine(x, top, ybase - top + 1, fillPos);
    else              c->drawFastVLine(x, ybase, top - ybase + 1, fillNeg);
    if (prevTop >= 0) c->drawLine(x - 1, prevTop, x, top, lineCol);
    else              c->drawPixel(x, top, lineCol);
    prevTop = top;
  }
}

// Window pill (top-right) — tap to cycle the zoom window. Hit area is generous.
static constexpr int GR_PILL_W = 62, GR_PILL_H = 28, GR_PILL_X = W - 8 - 62, GR_PILL_Y = 6;

static void winLabel(char* buf, size_t n, int mins) {
  if (mins >= 60) snprintf(buf, n, "%dh", mins / 60);
  else            snprintf(buf, n, "%dm", mins);
}

bool graphHitTest(int x, int y) {
  return x >= GR_PILL_X - 12 && y <= GR_PILL_Y + GR_PILL_H + 12;  // top-right corner
}

static void renderGraph(Arduino_GFX* c, const DashData& d) {
  char buf[28];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "History", kText);
  // Zoom-window pill (matches the web chart's windows).
  c->fillRoundRect(GR_PILL_X, GR_PILL_Y, GR_PILL_W, GR_PILL_H, 7, kGrey);
  c->drawRoundRect(GR_PILL_X, GR_PILL_Y, GR_PILL_W, GR_PILL_H, 7, kBlue);
  winLabel(buf, sizeof(buf), d.histWinMin);
  gtext(c, &FreeSansBold12pt7b, GR_PILL_X + GR_PILL_W / 2, GR_PILL_Y + 20, buf, kText, C);

  if (d.histCount < 2) {
    c->fillRoundRect(8, 44, W - 16, TAB_Y - 52, 10, kCard);
    gtext(c, &FreeSansBold18pt7b, W / 2, 150, "Collecting data...", kMuted, C);
    gtext(c, &FreeSans9pt7b, W / 2, 178, "trend appears after a minute", kMuted, C);
    return;
  }

  const int LX = 34;                       // left gutter for y labels
  const int PW = (W - 8) - LX;             // plot width
  Plot soc{LX, 46, PW, 108};
  Plot amp{LX, 172, PW, 104};

  // --- SoC chart (fixed 0..100 %) ---
  c->fillRoundRect(soc.x, soc.y, soc.w, soc.h, 6, kCard);
  for (int pct = 0; pct <= 100; pct += 50) {              // gridlines 0/50/100
    int gy = soc.y + soc.h - 1 - (soc.h - 1) * pct / 100;
    c->drawFastHLine(soc.x, gy, soc.w, kGrey);
    snprintf(buf, sizeof(buf), "%d", pct);
    gtext(c, &FreeSans9pt7b, LX - 4, gy + 5, buf, kMuted, R);
  }
  areaPlot(c, soc, d.histSoc, d.histCount, 0, 100, 0, kGreen, kFillGreen, kFillGreen);
  gtext(c, &FreeSans9pt7b, soc.x + 6, soc.y + 16, "Battery SoC %", kMuted);

  // --- Battery current chart (auto-scaled around zero) ---
  float lo = 0, hi = 0;
  bool any = false;
  for (int i = 0; i < d.histCount; ++i) {
    if (d.histBatt[i] == -32768) continue;
    float v = d.histBatt[i] / 10.0f;
    if (!any) { lo = hi = v; any = true; }
    else { if (v < lo) lo = v; if (v > hi) hi = v; }
  }
  if (!any) { lo = -1; hi = 1; }
  if (lo > 0) lo = 0;                    // always include zero
  if (hi < 0) hi = 0;
  float pad = (hi - lo) * 0.12f + 0.5f;
  lo -= pad; hi += pad;

  c->fillRoundRect(amp.x, amp.y, amp.w, amp.h, 6, kCard);
  int yzero = amp.y + amp.h - 1 - (int)lroundf((0 - lo) / (hi - lo) * (amp.h - 1));
  c->drawFastHLine(amp.x, yzero, amp.w, kMuted);        // zero line
  snprintf(buf, sizeof(buf), "%.0f", hi);
  gtext(c, &FreeSans9pt7b, LX - 4, amp.y + 12, buf, kMuted, R);
  snprintf(buf, sizeof(buf), "%.0f", lo);
  gtext(c, &FreeSans9pt7b, LX - 4, amp.y + amp.h - 3, buf, kMuted, R);
  areaPlot(c, amp, d.histBatt, d.histCount, lo, hi, 0, kCyan, kFillGreen, kFillRed);
  gtext(c, &FreeSans9pt7b, amp.x + 6, amp.y + 16, "Battery A (+charge)", kMuted);
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

static void renderSettings(Arduino_GFX* c, const DashData& d) {
  char buf[48];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Settings", kText);

  // --- Left column: profiles (tap to switch) + status ---
  c->fillRoundRect(8, 44, 200, TAB_Y - 52, 10, kCard);
  gtext(c, &FreeSans9pt7b, 18, 60, "PROFILES (tap)", kMuted);
  for (int i = 0; i < 4; ++i) {
    int ry = PROF_Y + i * PROF_RH;
    if (!d.profUsed[i]) continue;
    bool active = (i == d.profileId);
    if (active) c->fillRoundRect(PROF_X + 6, ry, PROF_W - 12, PROF_RH - 2, 5, kGrey);
    uint16_t dot = active ? kGreen : kMuted;
    c->fillCircle(PROF_X + 16, ry + (PROF_RH - 2) / 2, 4, dot);
    gtext(c, &FreeSans9pt7b, PROF_X + 28, ry + 15, d.profNames[i], active ? kText : kMuted);
  }
  // Status lines below the profile list.
  int sy = PROF_Y + 4 * PROF_RH + 12;
  const int dy = 17;
  auto stat = [&](const char* label, const char* val) {
    gtext(c, &FreeSans9pt7b, 18, sy, label, kMuted);
    gtext(c, &FreeSans9pt7b, 200, sy, val, kText, R);
    sy += dy;
  };
  stat("AP", d.apSsid);
  stat("IP", d.ipStr);
  snprintf(buf, sizeof(buf), "%d / %d", d.devPaired, d.devSeen);
  stat("Dev set/seen", buf);
  uint32_t up = d.uptimeSec;
  if (up >= 86400) snprintf(buf, sizeof(buf), "%ud %uh", up / 86400, (up % 86400) / 3600);
  else if (up >= 3600) snprintf(buf, sizeof(buf), "%uh %um", up / 3600, (up % 3600) / 60);
  else snprintf(buf, sizeof(buf), "%um", up / 60);
  stat("Uptime", buf);
  snprintf(buf, sizeof(buf), "%uKB", d.freeHeapKb);
  stat("Free heap", buf);
  stat("Firmware", d.version[0] ? d.version : "--");

  // --- Right column: tunables (tap a row, adjust with -/+) ---
  c->fillRoundRect(TUN_X, 44, TUN_W, TAB_Y - 52, 10, kCard);
  gtext(c, &FreeSans9pt7b, TUN_X + 12, 60, "ADJUST (tap a row)", kMuted);
  for (int i = 0; i < TUNABLE_N; ++i) {
    int ry = TUN_Y + i * TUN_RH;
    bool sel = (i == d.setSel);
    if (sel) c->fillRoundRect(TUN_X + 6, ry, TUN_W - 12, TUN_RH - 2, 4, kGrey);
    gtext(c, &FreeSans9pt7b, TUN_X + 14, ry + 15, kTun[i].label, sel ? kText : kMuted);
    tunText(buf, sizeof(buf), i, tunValue(d, i));
    gtext(c, &FreeSansBold12pt7b, TUN_X + TUN_W - 14, ry + 16, buf, sel ? kBlue : kText, R);
  }
  // Adjust buttons act on the highlighted row.
  button(c, TUN_X, ADJ_Y, ADJ_W, ADJ_H, "-", kBlue);
  button(c, TUN_X + ADJ_W + 8, ADJ_Y, ADJ_W, ADJ_H, "+", kBlue);
}

SettingsHitResult settingsHit(int x, int y) {
  auto in = [&](int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
  };
  // Adjust buttons.
  if (in(TUN_X, ADJ_Y, ADJ_W, ADJ_H)) return {SA_ADJ_DN, 0};
  if (in(TUN_X + ADJ_W + 8, ADJ_Y, ADJ_W, ADJ_H)) return {SA_ADJ_UP, 0};
  // Tunable rows.
  if (in(TUN_X, TUN_Y, TUN_W, TUNABLE_N * TUN_RH)) {
    int row = (y - TUN_Y) / TUN_RH;
    if (row >= 0 && row < TUNABLE_N) return {SA_SELECT_ROW, row};
  }
  // Profile rows.
  if (in(PROF_X, PROF_Y, PROF_W, 4 * PROF_RH)) {
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
    case PAGE_GRAPH:    renderGraph(c, d); break;
    case PAGE_DAYS:     renderDays(c, d); break;
    case PAGE_SETTINGS: renderSettings(c, d); break;
    case PAGE_DASH:
    default:            renderDash(c, d); break;
  }
  renderTabs(c, page);
  c->setFont(nullptr);
}

void renderDashboard(Arduino_GFX* c, const DashData& d) { renderPage(c, PAGE_DASH, d); }

}  // namespace guition
