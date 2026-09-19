// Environment page: cabin temperature / humidity / pressure / gas from the Unit
// ENV Pro (BME688) on the master's Grove port.
//
// Two stacked dual-axis charts rather than one panel, because these four channels
// have four unrelated units. The Graph page can put five currents on one shared
// Amps axis; degC, %RH, hPa and kOhm have no common scale, and overlaying all four
// would need four axes to be readable and would still invite misreading. So they
// are paired — temperature with humidity, pressure with gas — each pair sharing a
// chart but NOT an axis: the first series is scaled and labelled on the left, the
// second (dashed) on the right. The pairing, colours and dashed-right convention
// match the web Environment card exactly, so the two read the same.
//
// Both axes auto-scale to their own data rather than a nominal full range: the
// interesting movement is small (indoor humidity lives in a few percent, pressure
// in a few hPa), which a 0..100 % or 300..1100 hPa axis would flatten into a
// straight line. A per-channel minimum span keeps a genuinely flat trace as a flat
// line in a sensible band instead of magnifying sensor noise to full scale.
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

// One plotted channel: its history, the divisor that turns the stored fixed-point
// value into real units, and how it is drawn and labelled.
struct EnvSeries {
  const int16_t* vals;
  float scale;        // stored / scale = real units (10 for deci-*, 1 for whole kOhm)
  const char* name;
  const char* unit;
  int dp;             // decimals on the axis + live value
  uint16_t col;
  float minSpan;      // smallest axis range to show (stops a flat trace becoming noise)
  bool nowValid;
  float now;
};

// Card / plot geometry. Two equal charts stacked between the zoom pills and the
// tab bar, with the time labels under the lower one only (both share the axis).
// Left gutter must fit the widest LEFT-axis label, which is pressure ("1019.3",
// ~45px at FreeSans9pt7b) — not temperature. The right gutter only ever carries
// humidity ("54.1") or gas ("110"), so it can be narrower.
static constexpr int EC_LX = 52;                  // left gutter: axis labels
static constexpr int EC_RX = 38;                  // right gutter: axis labels
static constexpr int EC_TOP = 40;                 // below the zoom pill row
static constexpr int EC_H = 114;                  // per-card height
static constexpr int EC_GAP = 8;

// Auto-scaled [lo,hi] for one series over `n` points, folding in the live value so
// the current reading is never off-scale. Returns false when there is nothing valid.
static bool envRange(const EnvSeries& s, int n, float& lo, float& hi) {
  bool any = false;
  float mn = 0, mx = 0;
  for (int i = 0; i < n; ++i) {
    if (s.vals[i] == -32768) continue;
    float v = s.vals[i] / s.scale;
    if (!any) { mn = mx = v; any = true; }
    else { if (v < mn) mn = v; if (v > mx) mx = v; }
  }
  if (s.nowValid) {
    if (!any) { mn = mx = s.now; any = true; }
    else { if (s.now < mn) mn = s.now; if (s.now > mx) mx = s.now; }
  }
  if (!any) return false;
  float span = mx - mn;
  if (span < s.minSpan) {
    float mid = (mn + mx) / 2;
    lo = mid - s.minSpan / 2;
    hi = mid + s.minSpan / 2;
  } else {
    lo = mn - span * 0.1f;
    hi = mx + span * 0.1f;
  }
  return true;
}

// Line plot of `s` into the data rect, mapping [lo,hi] to the height. Interpolates
// per screen column so gaps appear over missing samples; `dashed` marks the
// right-axis series (same convention as the Graph page's SoC overlay).
static void envPlot(Arduino_GFX* c, int px0, int py0, int pw, int ph,
                    const EnvSeries& s, int n, float lo, float hi, bool dashed) {
  if (n < 2 || pw < 2 || hi <= lo) return;
  auto yOf = [&](float v) -> int {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return py0 + ph - 1 - (int)lroundf((v - lo) / (hi - lo) * (ph - 1));
  };
  int prevY = -1;
  for (int px = 0; px < pw; ++px) {
    if (dashed && ((px >> 2) & 1)) { prevY = -1; continue; }  // ~4px dash / 4px gap
    float fi = (float)px * (n - 1) / (pw - 1);
    int i0 = (int)fi, i1 = i0 + 1;
    if (i1 >= n) i1 = n - 1;
    int16_t a = s.vals[i0], b = s.vals[i1];
    if (a == -32768 || b == -32768) { prevY = -1; continue; }
    float frac = fi - i0;
    float v = (a / s.scale) * (1 - frac) + (b / s.scale) * frac;
    int y = yOf(v), x = px0 + px;
    if (prevY >= 0) c->drawLine(x - 1, prevY, x, y, s.col);
    else            c->drawPixel(x, y, s.col);
    prevY = y;
  }
}

// One dual-axis chart: card, both axes, gridlines, the two traces, and a header
// row carrying each channel's live value in its own colour (so the axis a line
// belongs to needs no legend lookup).
static void envChart(Arduino_GFX* c, const DashData& d, int cardY,
                     const EnvSeries& left, const EnvSeries& right, bool timeLabels) {
  char buf[20];
  const int n = d.histCount;
  const int BAND = timeLabels ? 15 : 4;
  const int cardX = EC_LX, cardW = W - 6 - EC_LX - EC_RX;
  const int dX = cardX + 3, dW = cardW - 6;
  const int dY = cardY + 20, dH = EC_H - 20 - BAND;

  c->fillRoundRect(cardX, cardY, cardW, EC_H, 8, kCard);

  // Header: "Temperature 24.5C" left, "Humidity 54.1%RH" right, each in its colour.
  snprintf(buf, sizeof(buf), "%s ", left.name);
  gtext(c, &FreeSans9pt7b, cardX + 6, cardY + 15, buf, left.col, L);
  numOr(buf, sizeof(buf), left.nowValid, left.now, left.dp, left.unit);
  gtext(c, &FreeSans9pt7b, cardX + 6 + 90, cardY + 15, buf, left.col, L);
  numOr(buf, sizeof(buf), right.nowValid, right.now, right.dp, right.unit);
  gtext(c, &FreeSans9pt7b, cardX + cardW - 6, cardY + 15, buf, right.col, R);
  snprintf(buf, sizeof(buf), "%s", right.name);
  gtext(c, &FreeSans9pt7b, cardX + cardW - 6 - 74, cardY + 15, buf, right.col, R);

  float lLo, lHi, rLo, rHi;
  bool haveL = envRange(left, n, lLo, lHi);
  bool haveR = envRange(right, n, rLo, rHi);

  // Gridlines, labelled twice: left in the left series' colour and unit, right in
  // the right series'. One set of lines, two readings.
  for (int gi = 0; gi <= 3; ++gi) {
    int y = dY + dH - 1 - (int)lroundf((float)gi / 3 * (dH - 1));
    c->drawFastHLine(dX, y, dW, kGrey);
    if (haveL) {
      snprintf(buf, sizeof(buf), "%.*f", left.dp, lLo + (lHi - lLo) * gi / 3);
      gtext(c, &FreeSans9pt7b, cardX - 3, y + 4, buf, left.col, R);
    }
    if (haveR) {
      snprintf(buf, sizeof(buf), "%.*f", right.dp, rLo + (rHi - rLo) * gi / 3);
      gtext(c, &FreeSans9pt7b, cardX + cardW + 3, y + 4, buf, right.col, L);
    }
  }

  // Vertical time gridlines, on the same scheme as the Graph page.
  int win = d.histWinMin;
  int stepMin = win <= 10 ? 1 : (win <= 60 ? 10 : 180);
  for (int t = stepMin; t < win; t += stepMin) {
    int x = dX + (int)lroundf((1.0f - (float)t / win) * (dW - 1));
    c->drawFastVLine(x, dY, dH, kGrey);
    if (timeLabels && (float)dW * stepMin / win >= 34) {
      if (t < 60) snprintf(buf, sizeof(buf), "%dm", t);
      else        snprintf(buf, sizeof(buf), "%dh", t / 60);
      gtext(c, &FreeSans9pt7b, x, cardY + EC_H - 4, buf, kMuted, C);
    }
  }
  if (timeLabels) {
    if (win >= 60) snprintf(buf, sizeof(buf), "-%dh", win / 60);
    else           snprintf(buf, sizeof(buf), "-%dm", win);
    gtext(c, &FreeSans9pt7b, dX + 2, cardY + EC_H - 4, buf, kMuted, L);
    gtext(c, &FreeSans9pt7b, dX + dW - 2, cardY + EC_H - 4, "now", kMuted, R);
  }

  if (haveL) envPlot(c, dX, dY, dW, dH, left, n, lLo, lHi, false);
  if (haveR) envPlot(c, dX, dY, dW, dH, right, n, rLo, rHi, true);
}

void renderEnv(Arduino_GFX* c, const DashData& d) {
  renderZoomRow(c, d.histWinMin);

  // No sensor fitted (and none ever seen): say so plainly rather than drawing two
  // empty frames that look like a fault.
  if (!d.envPresent) {
    gtext(c, &FreeSansBold12pt7b, W / 2, 140, "No environment sensor", kMuted, C);
    gtext(c, &FreeSans9pt7b, W / 2, 168,
          "Connect a Unit ENV Pro to the master's Port A", kMuted, C);
    return;
  }

  const EnvSeries temp  = {d.histEnvT, 10.0f, "Temperature", "C",   1, kSerTemp,
                           1.0f,  d.envValid,    d.envTempC};
  const EnvSeries humid = {d.histEnvH, 10.0f, "Humidity",    "%RH", 1, kSerHum,
                           1.0f,  d.envValid,    d.envHumidity};
  const EnvSeries press = {d.histEnvP, 10.0f, "Pressure",    "hPa", 1, kSerPress,
                           2.0f,  d.envValid,    d.envPressureHpa};
  const EnvSeries gas   = {d.histEnvG, 1.0f,  "Gas",         "k",   0, kSerGas,
                           10.0f, d.envGasValid, d.envGasKohm};

  envChart(c, d, EC_TOP, temp, humid, false);
  envChart(c, d, EC_TOP + EC_H + EC_GAP, press, gas, true);
}

}  // namespace guition
