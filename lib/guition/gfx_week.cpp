// Week page: runtime-day energy bars (left) + three resettable Net-in/Net-out
// Ah meters down the right rail (split from GfxDashboard.cpp, P4). No clock
// required — "days" come from the firmware's run-time odometer (or the calendar
// when a clock is set). Long-press a card to reset that meter.
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

// ------------------------------------------------------------- week page ----
static const int RAIL_W = 150;
static const int RAIL_X = W - 8 - RAIL_W;         // 322: meter-card column
static const int WK_TOP = 34;                     // below the compact title row
static const int WK_BOT = TAB_Y - 2;              // above the tab bar
static const uint32_t kYmdMin = 20000000;         // dayStamp >= this is a yyyymmdd date

// Card hit-boxes (also used by the long-press hit-test in the display task).
int weekCardAt(int px, int py) {
  if (px < RAIL_X || px >= W - 8) return -1;
  int gap = 8, ch = (WK_BOT - WK_TOP - 2 * gap) / 3;
  for (int i = 0; i < 3; ++i) {
    int cy = WK_TOP + i * (ch + gap);
    if (py >= cy && py < cy + ch) return i;
  }
  return -1;
}

// Ah value: "42", "1.2k" — keeps big lifetime totals inside the card width.
static void ahShort(char* b, size_t n, float ah) {
  if (ah >= 10000) snprintf(b, n, "%.1fk", ah / 1000.0f);
  else             snprintf(b, n, "%.0f", ah);
}

// Thin stacked source-split bar (solar / dc-dc / charger) under the meters.
static void splitBar(Arduino_GFX* c, int x, int y, int w, int h, const DashData::StatMeter& m) {
  float tot = m.solarAh + m.dcdcAh + m.chargerAh;
  c->fillRoundRect(x, y, w, h, h / 2, kBlack);
  if (tot <= 0) return;
  const float parts[3] = {m.solarAh, m.dcdcAh, m.chargerAh};
  const uint16_t cols[3] = {kSerSolar, kSerDcdc, kSerChg};
  int cx = x;
  for (int i = 0; i < 3; ++i) {
    int seg = (int)(w * parts[i] / tot + 0.5f);
    if (cx + seg > x + w) seg = x + w - cx;
    if (seg > 0) c->fillRect(cx, y, seg, h, cols[i]);
    cx += seg;
  }
}

static void meterCard(Arduino_GFX* c, int x, int y, int w, int h, const char* title,
                      const DashData::StatMeter& m, bool held, float frac) {
  char b[20], v[28];
  c->fillRoundRect(x, y, w, h, 9, kCard);
  c->drawRoundRect(x, y, w, h, 9, held ? kAmber : kGrey);

  // Header: title + accumulating duration (d/h/m).
  gtext(c, &FreeSans9pt7b, x + 10, y + 16, title, kMuted);
  uint32_t s = m.durSecs;
  if (s >= 86400)      snprintf(b, sizeof(b), "%ud %uh", s / 86400, (s % 86400) / 3600);
  else if (s >= 3600)  snprintf(b, sizeof(b), "%uh %um", s / 3600, (s % 3600) / 60);
  else                 snprintf(b, sizeof(b), "%um", s / 60);
  gtext(c, &FreeSans9pt7b, x + w - 10, y + 16, b, kMuted, R);

  // Net in / net out (Ah) — the headline figures.
  ahShort(b, sizeof(b), m.inAh);  snprintf(v, sizeof(v), "+%s", b);
  gtext(c, &FreeSans9pt7b, x + 10, y + 39, "In", kGreen);
  gtext(c, &FreeSansBold12pt7b, x + w - 10, y + 40, v, kGreen, R);
  ahShort(b, sizeof(b), m.outAh); snprintf(v, sizeof(v), "-%s", b);
  gtext(c, &FreeSans9pt7b, x + 10, y + 59, "Out", kSerLoad);
  gtext(c, &FreeSansBold12pt7b, x + w - 10, y + 60, v, kSerLoad, R);

  splitBar(c, x + 10, y + h - 13, w - 20, 5, m);

  // Long-press-to-reset: a filling bar + label along the bottom edge.
  if (held) {
    int pw = (int)((w - 4) * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
    c->fillRect(x + 2, y + h - 4, pw, 3, frac >= 1 ? kGreen : kAmber);
    gtext(c, &FreeSans9pt7b, x + w / 2, y + h - 13, frac >= 1 ? "reset" : "hold to reset",
          frac >= 1 ? kGreen : kAmber, C);
  }
}

void renderDays(Arduino_GFX* c, const DashData& d) {
  char b[28];
  gtext(c, &FreeSansBold18pt7b, 12, 26, "Energy", kText);

  // ---- Right rail: three resettable meters ----
  const int gap = 8, ch = (WK_BOT - WK_TOP - 2 * gap) / 3;
  const char* names[3] = {"TODAY", "TRIP", "TOTAL"};
  const DashData::StatMeter* ms[3] = {&d.statToday, &d.statTrip, &d.statTotal};
  for (int i = 0; i < 3; ++i) {
    int cy = WK_TOP + i * (ch + gap);
    meterCard(c, RAIL_X, cy, RAIL_W, ch, names[i], *ms[i],
              d.weekHold == i, d.weekHoldFrac);
  }

  // ---- Left: runtime-day Ah bars (past days + a live "now" column) ----
  const int cx0 = 8, cw = RAIL_X - 8 - cx0;   // chart card
  c->fillRoundRect(cx0, WK_TOP, cw, WK_BOT - WK_TOP, 10, kCard);
  const char* unit = d.clockOk ? "days" : "run-days";
  snprintf(b, sizeof(b), "now  +%.0f / -%.0f Ah", d.statToday.inAh, d.statToday.outAh);
  gtext(c, &FreeSans9pt7b, cx0 + 14, WK_TOP + 18, unit, kMuted);
  gtext(c, &FreeSans9pt7b, cx0 + cw - 10, WK_TOP + 18, b, kMuted, R);

  const int px = cx0 + 34, py = WK_TOP + 32, pw = cw - 34 - 10, ph = (WK_BOT - WK_TOP) - 32 - 22;
  const int base = py + ph;

  // Columns = archived days + the current (partial) day.
  const int nd = d.dayCount;
  const int ncol = nd + 1;  // + "now"
  float mx = 1;
  auto dayIn = [&](int i) { return d.daySolarAh[i] + d.dayDcdcAh[i] + d.dayChargerAh[i]; };
  for (int i = 0; i < nd; ++i) { if (dayIn(i) > mx) mx = dayIn(i); if (d.dayLoadAh[i] > mx) mx = d.dayLoadAh[i]; }
  float nowIn = d.statToday.solarAh + d.statToday.dcdcAh + d.statToday.chargerAh;
  if (nowIn > mx) mx = nowIn;
  if (d.statToday.outAh > mx) mx = d.statToday.outAh;

  auto yOf = [&](float v) { return py + (int)(ph * (1.0f - v / mx)); };
  for (int g = 0; g <= 2; ++g) {  // gridlines mx, mx/2, 0
    float gv = mx * (2 - g) / 2;
    int gy = yOf(gv);
    c->drawFastHLine(px, gy, pw, kGrey);
    snprintf(b, sizeof(b), "%.0f", gv);
    gtext(c, &FreeSans9pt7b, px - 4, gy + 4, b, kMuted, R);
  }

  float slot = (float)pw / (ncol < 1 ? 1 : ncol);
  int bw = (int)(slot * 0.30f); if (bw < 3) bw = 3;
  for (int i = 0; i < ncol; ++i) {
    bool now = (i == nd);
    int ccx = px + (int)(slot * (i + 0.5f));
    int xi = ccx - bw - 1, xo = ccx + 1;
    float sol = now ? d.statToday.solarAh   : d.daySolarAh[i];
    float dcd = now ? d.statToday.dcdcAh    : d.dayDcdcAh[i];
    float chg = now ? d.statToday.chargerAh : d.dayChargerAh[i];
    float out = now ? d.statToday.outAh     : d.dayLoadAh[i];
    // stacked IN
    float acc = 0;
    const float vals[3] = {sol, dcd, chg};
    const uint16_t cols[3] = {kSerSolar, kSerDcdc, kSerChg};
    for (int sIdx = 0; sIdx < 3; ++sIdx) {
      float v = vals[sIdx]; if (v <= 0) continue;
      int y0 = yOf(acc), y1 = yOf(acc + v);
      c->fillRect(xi, y1, bw, y0 - y1, cols[sIdx]);
      acc += v;
    }
    if (out > 0) { int yo = yOf(out); c->fillRect(xo, yo, bw, base - yo, kSerLoad); }
    // label: "now", a date MM/DD (clocked) or the run-day index
    uint32_t ds = d.dayStamp[i];
    if (now) snprintf(b, sizeof(b), "now");
    else if (d.clockOk && ds >= kYmdMin) snprintf(b, sizeof(b), "%u/%u", (ds / 100) % 100, ds % 100);
    else snprintf(b, sizeof(b), "%u", ds);
    gtext(c, &FreeSans9pt7b, ccx, base + 16, b, now ? kText : kMuted, C);
  }
  if (ncol == 1) {  // only the "now" column, no history yet
    gtext(c, &FreeSans9pt7b, px + pw / 2, py + ph / 2,
          d.clockOk ? "new day at midnight" : "day 1 accruing", kMuted, C);
  }
}

}  // namespace guition
