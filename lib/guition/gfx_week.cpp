// Week page: last-7-days energy bars (split from GfxDashboard.cpp, P4).
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

// ------------------------------------------------------------- week page ----
// Last-7-days energy: stacked Wh in (solar+dcdc+charger) vs Wh out (load), one
// pair of bars per day. Mirrors the web app's "Last 7 days" chart.
void renderDays(Arduino_GFX* c, const DashData& d) {
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

}  // namespace guition
