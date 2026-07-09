// Shared drawing primitives used across the dashboard pages (split from
// GfxDashboard.cpp, P4). Declared in gfx_internal.h.
#include "gfx_internal.h"

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

}  // namespace guition
