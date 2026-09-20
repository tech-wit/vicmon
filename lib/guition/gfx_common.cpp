// Shared drawing primitives used across the dashboard pages (split from
// GfxDashboard.cpp, P4). Declared in gfx_internal.h.
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

void gtext(Arduino_GFX* c, const GFXfont* f, int x, int y, const char* s,
                  uint16_t color, Align a) {
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

int textW(Arduino_GFX* c, const GFXfont* f, const char* s) {
  int16_t bx, by; uint16_t bw, bh;
  c->setFont(f);
  c->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  return bw;
}

void numOr(char* buf, size_t n, bool valid, float v, int dp, const char* unit) {
  if (!valid) snprintf(buf, n, "--%s", unit);
  else        snprintf(buf, n, "%.*f%s", dp, v, unit);
}

uint16_t modeColor(const DashData& d) {
  if (d.worst >= 2) return kRed;
  if (d.worst == 1) return kAmber;
  if (strcmp(d.mode, "Charging") == 0) return kGreen;
  if (strcmp(d.mode, "Discharging") == 0) return kRed;
  return kGrey;
}

// Format a duration in minutes as days/hours/minutes (matches the web ttgStr):
// >=1d -> "2d 4h", >=1h -> "3h 20m", else "45m".
static void fmtDuration(char* buf, size_t n, float mins) {
  if (mins >= 1440.0f) {
    int d = (int)(mins / 1440.0f);
    int h = (int)lroundf(fmodf(mins, 1440.0f) / 60.0f);
    if (h) snprintf(buf, n, "%dd %dh", d, h); else snprintf(buf, n, "%dd", d);
  } else if (mins >= 60.0f) {
    int h = (int)(mins / 60.0f);
    int m = (int)lroundf(fmodf(mins, 60.0f));
    if (m) snprintf(buf, n, "%dh %dm", h, m); else snprintf(buf, n, "%dh", h);
  } else {
    snprintf(buf, n, "%dm", (int)lroundf(mins));
  }
}

void ttgLabel(char* buf, size_t n, const DashData& d) {
  char t[16];
  // Instantaneous estimate (settles in seconds; uncapped) when we know the
  // battery capacity and have a live current — charging gives time-to-full,
  // discharging time-to-empty.
  if (d.battCapAh > 0 && d.battValid && fabsf(d.a) > 0.05f) {
    if (d.a > 0) {  // charging into the battery
      fmtDuration(t, sizeof(t), d.battCapAh * (1.0f - d.soc / 100.0f) / d.a * 60.0f);
      snprintf(buf, n, "Full %s", t);
    } else {        // discharging
      fmtDuration(t, sizeof(t), d.battCapAh * (d.soc / 100.0f) / fabsf(d.a) * 60.0f);
      snprintf(buf, n, "TTG %s", t);
    }
  } else if (d.ttgValid && d.ttg > 0) {  // fall back to the BMV's filtered TTG
    fmtDuration(t, sizeof(t), d.ttg);
    snprintf(buf, n, "TTG %s", t);
  } else {
    snprintf(buf, n, "TTG --");
  }
}

}  // namespace guition
