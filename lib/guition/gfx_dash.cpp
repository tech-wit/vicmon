// Dash page: battery + source tiles (split from GfxDashboard.cpp, P4).
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

// ------------------------------------------------------------ dash page ----
static void tile(Arduino_GFX* c, int x, int y, int w, int h, const char* label,
                 uint16_t labelColor, const char* value) {
  c->fillRoundRect(x, y, w, h, 6, kCard);
  gtext(c, &FreeSans9pt7b, x + 10, y + 19, label, labelColor);
  gtext(c, &FreeSansBold12pt7b, x + w - 10, y + h - 11, value, kText, R);
}

void renderDash(Arduino_GFX* c, const DashData& d) {
  char buf[24];
  // Top bar in two sections: a dark VICMON brand block on the left, and the
  // charge-status banner (mode colour, black text) filling the rest.
  const int kBrandW = 156;
  c->fillRect(0, 0, kBrandW, 38, kBlack);
  c->fillRect(kBrandW, 0, W - kBrandW, 38, d.linkStale ? kGrey : modeColor(d));
  gtext(c, &FreeSansBold12pt7b, kBrandW / 2, 26, "VICMON", kAccent, C);
  gtext(c, &FreeSansBold18pt7b, kBrandW + (W - kBrandW) / 2, 27, d.mode, d.linkStale ? kMuted : kBlack, C);
  if (d.linkStale) gtext(c, &FreeSansBold12pt7b, W - 10, 26, "STALE", kAmber, R);

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
  // Battery power (V x A, signed) between the V and A readouts.
  if (d.battValid) snprintf(buf, sizeof(buf), "%.0fW", d.v * d.a);
  else             snprintf(buf, sizeof(buf), "--W");
  gtext(c, &FreeSansBold12pt7b, bx + bw / 2, by + 161, buf,
        d.battValid ? (d.a >= 0 ? kGreen : kCyan) : kMuted, C);

  // Remaining / capacity Ah (rem = capacity x SoC), matching the AP mimic.
  if (d.battCapAh > 0 && d.battValid) {
    snprintf(buf, sizeof(buf), "%.0f/%.0f Ah", d.battCapAh * d.soc / 100.0f, d.battCapAh);
    gtext(c, &FreeSansBold12pt7b, bx + bw / 2, by + 194, buf, kText, C);
  }

  ttgLabel(buf, sizeof(buf), d);  // "TTG 2d 4h" / "Full 1d 3h" / "TTG 45m"
  gtext(c, &FreeSans9pt7b, bx + 16, by + bh - 14, buf, kMuted);
  if (d.starterValid) snprintf(buf, sizeof(buf), "Starter %.1fV", d.starterV);
  else                snprintf(buf, sizeof(buf), "Starter --");
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

}  // namespace guition
