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
  gtext(c, &FreeSans9pt7b, W - 8, 24, "hold = reset", kMuted, R);  // hint above the cards
  // Current time (HH:MM, local) centred in the top bar when a clock is set.
  if (d.nowEpoch) {
    uint32_t secs = d.nowEpoch % 86400;
    snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(secs / 3600), (unsigned)((secs % 3600) / 60));
    gtext(c, &FreeSansBold12pt7b, W / 2, 25, b, kText, C);
  }

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

  // Always draw a fixed 7-"day" frame: the rightmost column is "now" (the current
  // partial day) and the six to its left are the most recent completed days,
  // left-padded with empty slots before any history has accrued.
  const int nd = d.dayCount;
  const int NSLOT = 7;
  const int capShown = nd < NSLOT - 1 ? nd : NSLOT - 1;  // completed days shown (<= 6)
  const int capFirst = (NSLOT - 1) - capShown;           // first slot holding a real day
  const int dBase = nd - capShown;                       // day-array index of that first day

  // In = the larger of NET charge and the attributed sources (the sources stack,
  // any unattributed remainder draws in grey); out = the larger of NET discharge
  // and load. Same rule as the web chart. A system whose charge source is not a
  // monitored device used to show empty bars against a meter reading +44 Ah.
  auto srcIn = [&](int di) { return d.daySolarAh[di] + d.dayDcdcAh[di] + d.dayChargerAh[di]; };
  auto dayIn = [&](int di) { float s = srcIn(di); return d.dayChargedAh[di] > s ? d.dayChargedAh[di] : s; };
  auto dayOut = [&](int di) { return d.dayDischargedAh[di] > d.dayLoadAh[di] ? d.dayDischargedAh[di] : d.dayLoadAh[di]; };
  float mx = 1;
  for (int j = 0; j < capShown; ++j) {
    int di = dBase + j;
    if (dayIn(di) > mx) mx = dayIn(di);
    if (dayOut(di) > mx) mx = dayOut(di);
  }
  float nowSrc = d.statToday.solarAh + d.statToday.dcdcAh + d.statToday.chargerAh;
  float nowIn = d.statToday.inAh > nowSrc ? d.statToday.inAh : nowSrc;
  float nowOut = d.statToday.outAh > d.statToday.loadAh ? d.statToday.outAh : d.statToday.loadAh;
  if (nowIn > mx) mx = nowIn;
  if (nowOut > mx) mx = nowOut;

  auto yOf = [&](float v) { return py + (int)(ph * (1.0f - v / mx)); };
  for (int g = 0; g <= 2; ++g) {  // gridlines mx, mx/2, 0
    float gv = mx * (2 - g) / 2;
    int gy = yOf(gv);
    c->drawFastHLine(px, gy, pw, kGrey);
    snprintf(b, sizeof(b), "%.0f", gv);
    gtext(c, &FreeSans9pt7b, px - 4, gy + 4, b, kMuted, R);
  }

  float slot = (float)pw / NSLOT;
  int bw = (int)(slot * 0.30f); if (bw < 3) bw = 3;
  for (int col = 0; col < NSLOT; ++col) {
    bool now = (col == NSLOT - 1);
    bool empty = !now && col < capFirst;
    int ccx = px + (int)(slot * (col + 0.5f));
    int xi = ccx - bw - 1, xo = ccx + 1;
    int di = dBase + (col - capFirst);  // valid only when !empty && !now
    if (!empty) {
      float sol = now ? d.statToday.solarAh   : d.daySolarAh[di];
      float dcd = now ? d.statToday.dcdcAh    : d.dayDcdcAh[di];
      float chg = now ? d.statToday.chargerAh : d.dayChargerAh[di];
      float out = now ? nowOut : dayOut(di);
      float tot = now ? nowIn : dayIn(di);
      float acc = 0;
      const float vals[3] = {sol, dcd, chg};
      const uint16_t cols[3] = {kSerSolar, kSerDcdc, kSerChg};
      for (int sIdx = 0; sIdx < 3; ++sIdx) {
        float v = vals[sIdx]; if (v <= 0) continue;
        int y0 = yOf(acc), y1 = yOf(acc + v);
        c->fillRect(xi, y1, bw, y0 - y1, cols[sIdx]);
        acc += v;
      }
      if (tot - acc > 0.05f) {  // unattributed net charge, in grey
        int y0 = yOf(acc), y1 = yOf(tot);
        c->fillRect(xi, y1, bw, y0 - y1, kMuted);
      }
      if (out > 0) { int yo = yOf(out); c->fillRect(xo, yo, bw, base - yo, kSerLoad); }
    }
    // Bottom axis label: "now"; a date MM/DD (clocked) or the run-day index for a
    // real day; and for empty run-day slots, the projected index (dayNow counts
    // back one per slot) so the axis reads as a run of days even before history.
    if (now) snprintf(b, sizeof(b), "now");
    else if (!empty && d.clockOk && d.dayStamp[di] >= kYmdMin)
      snprintf(b, sizeof(b), "%u/%u", (d.dayStamp[di] / 100) % 100, d.dayStamp[di] % 100);
    else if (!empty) snprintf(b, sizeof(b), "%u", d.dayStamp[di]);
    else {  // empty slot
      int idx = (int)d.dayNow - (NSLOT - 1 - col);
      if (!d.clockOk && d.dayNow < kYmdMin && idx >= 1) snprintf(b, sizeof(b), "%d", idx);
      else b[0] = '\0';
    }
    if (b[0]) gtext(c, &FreeSans9pt7b, ccx, base + 16, b, now ? kText : kMuted, C);
  }
}

}  // namespace guition
