// Internal shared header for the split dashboard renderer (P4). The public API is
// in GfxDashboard.h; this holds the palette, layout constants, shared drawing
// primitives and the per-page render entry points that the page-descriptor
// registry in GfxDashboard.cpp dispatches to. Not part of the public interface.
#pragma once

#include "GfxDashboard.h"  // public API + DashData + Arduino_GFX (RGB565)

// Fonts are `const ... PROGMEM` (internal linkage), so including them here is
// safe across the several page TUs — only the fonts a TU actually references are
// emitted into it.
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

// Trend series colours — matched to the AP chart palette so the LCD Graph and the
// web chart are consistent (batt cyan vs dc-dc violet are now clearly distinct).
static constexpr uint16_t kSerBatt  = RGB565(0x22, 0xd3, 0xee);  // #22d3ee cyan
static constexpr uint16_t kSerSolar = RGB565(0xfa, 0xcc, 0x15);  // #facc15 yellow
static constexpr uint16_t kSerChg   = RGB565(0x60, 0xa5, 0xfa);  // #60a5fa light blue
static constexpr uint16_t kSerDcdc  = RGB565(0xa7, 0x8b, 0xfa);  // #a78bfa violet
static constexpr uint16_t kSerLoad  = RGB565(0xf8, 0x71, 0x71);  // #f87171 red
static constexpr uint16_t kSerSoc   = RGB565(0xf1, 0xf5, 0xf9);  // #f1f5f9 near-white
// AP mimic flow colours.
static constexpr uint16_t kFlowChg  = RGB565(0x34, 0xd3, 0x99);  // #34d399 charging/source green
static constexpr uint16_t kFlowLoad = RGB565(0xfb, 0xbf, 0x24);  // #fbbf24 load yellow

static constexpr int W = 480, H = 320;
static constexpr int TAB_Y = 284, TAB_H = H - TAB_Y;   // bottom tab bar
static constexpr int TAB_W = W / PAGE_COUNT;

enum Align { L, R, C };

// Shared drawing primitives (defined in gfx_common.cpp).
void gtext(Arduino_GFX* c, const GFXfont* f, int x, int y, const char* s,
           uint16_t color, Align a = L);
void numOr(char* buf, size_t n, bool valid, float v, int dp, const char* unit);
uint16_t modeColor(const DashData& d);

// Per-page renderers (one per file), dispatched by the registry in
// GfxDashboard.cpp. The heterogeneous per-page hit-tests stay in GfxDashboard.h.
void renderDash(Arduino_GFX* c, const DashData& d);
void renderFlow(Arduino_GFX* c, const DashData& d);
void renderGraph(Arduino_GFX* c, const DashData& d);
void renderDays(Arduino_GFX* c, const DashData& d);
void renderSettings(Arduino_GFX* c, const DashData& d);

}  // namespace guition
