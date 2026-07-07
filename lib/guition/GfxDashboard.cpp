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
  static const char* names[PAGE_COUNT] = {"Dash", "Flow", "Graph", "Set"};
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

// ------------------------------------------------ graph / settings pages ----
static void placeholder(Arduino_GFX* c, const char* title, const char* line1,
                        const char* line2) {
  gtext(c, &FreeSansBold18pt7b, 12, 30, title, kText);
  c->fillRoundRect(8, 44, W - 16, TAB_Y - 52, 10, kCard);
  gtext(c, &FreeSansBold18pt7b, W / 2, 150, line1, kMuted, C);
  if (line2) gtext(c, &FreeSans9pt7b, W / 2, 180, line2, kMuted, C);
}

// -------------------------------------------------------------- dispatch ----
void renderPage(Arduino_GFX* c, Page page, const DashData& d) {
  c->fillScreen(kBg);
  switch (page) {
    case PAGE_FLOW:     renderFlow(c, d); break;
    case PAGE_GRAPH:    placeholder(c, "Graph", "Trend chart", "coming soon"); break;
    case PAGE_SETTINGS: placeholder(c, "Settings", "Settings", "use the web app for now"); break;
    case PAGE_DASH:
    default:            renderDash(c, d); break;
  }
  renderTabs(c, page);
  c->setFont(nullptr);
}

void renderDashboard(Arduino_GFX* c, const DashData& d) { renderPage(c, PAGE_DASH, d); }

}  // namespace guition
