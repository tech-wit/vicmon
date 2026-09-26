// LilyGo T-Display-S3 display path — the runtime counterpart to the Guition glue
// in display.cpp, dispatched from the shared entry points (bringUpDisplay /
// publishDash / publishSlaveDash / serviceDashRequests) when detectBoard() picks
// HW_LILYGO. Compiled only when the LilyGo backend is in the image.
//
// A compact 320x170 dashboard with page parity to the Guition build — Dashboard,
// Power Flow, Graph, Week, Status — fed from the SAME per-role DashData assembly
// (collectDashForRole). Text uses the FreeSans/FreeSansBold GFX fonts (shared with
// the Guition renderer) for a clean look; frames render into the LilyGo's PSRAM
// canvas and flush as one image.
//
// Threading mirrors the Guition path: the loop task PUBLISHES a mutex-protected
// DashData snapshot (lilygoRender, registry-owner side); a dedicated display task
// polls the two buttons at ~60Hz and draws from that snapshot — so buttons stay
// crisp even while BLE scanning stalls the main loop.
//
// Buttons (A = top, B = bottom):
//   A short -> next page      A long -> previous page
//   B short -> page primary    B long -> page secondary   (footer shows B's action)

#include "app.h"

#ifdef VICMON_HAS_LILYGO
#if !defined(VICMON_HAS_GUITION)
#error "The LilyGo renderer reuses guition::DashData; build the Guition backend in too (the universal s3 image defines both)."
#endif

#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "LilygoDisplay.h"
#include <fonts/FreeSans9pt7b.h>
#include <fonts/FreeSansBold12pt7b.h>
#include <fonts/FreeSansBold18pt7b.h>
#include <fonts/FreeSansBold24pt7b.h>

using guition::DashData;

// Reuse the Guition renderer's TTG formatter so every view reads identically:
// capacity-aware "Full 1d 3h" / "TTG 2d 4h" / "TTG 45m" (fmtDuration day/hour).
namespace guition { void ttgLabel(char* buf, size_t n, const DashData& d); }

static const GFXfont* F_S  = &FreeSans9pt7b;        // small labels / footer
static const GFXfont* F_M  = &FreeSansBold12pt7b;   // values
static const GFXfont* F_L  = &FreeSansBold18pt7b;   // prominent values
static const GFXfont* F_XL = &FreeSansBold24pt7b;   // hero (SoC)

static lilygo::Display gLcd;

// ---- geometry & palette ----------------------------------------------------
static constexpr int16_t W = 320, H = 170;
static constexpr int16_t FOOT_Y = 152;  // footer strip 152..170

static const uint16_t C_BG    = BLACK;
static const uint16_t C_LBL   = RGB565(150, 150, 162);
static const uint16_t C_DIM   = RGB565(95, 95, 108);
static const uint16_t C_TEXT  = RGB565(232, 232, 238);
static const uint16_t C_GREEN = RGB565(60, 220, 110);
static const uint16_t C_AMBER = RGB565(245, 172, 45);
static const uint16_t C_RED   = RGB565(245, 72, 72);
static const uint16_t C_BLUE  = RGB565(95, 178, 255);
static const uint16_t C_SOLAR = RGB565(245, 212, 60);
static const uint16_t C_ALT   = RGB565(90, 212, 228);
static const uint16_t C_CHG   = RGB565(182, 152, 255);
static const uint16_t C_LOAD  = RGB565(255, 142, 72);
static const uint16_t C_SOC   = RGB565(235, 235, 240);
// Environment series — same hues as the web Environment card and the Guition's
// Env page, so the three UIs read alike.
static const uint16_t C_ENV_T = RGB565(251, 146, 60);   // #fb923c temperature
static const uint16_t C_ENV_H = RGB565(56, 189, 248);   // #38bdf8 humidity
static const uint16_t C_ENV_P = RGB565(163, 230, 53);   // #a3e635 pressure
static const uint16_t C_ENV_G = RGB565(192, 132, 252);  // #c084fc gas

// ---- pages & UI state ------------------------------------------------------
enum LPage : uint8_t { LP_DASH = 0, LP_FLOW, LP_GRAPH, LP_ENV, LP_WEEK, LP_INFO, LP_SET, LP_COUNT };
static const char* kPageName[LP_COUNT] = {"Dashboard", "Power Flow", "Graph", "Environment",
                                          "Week", "Status", "Settings"};
static const uint16_t kPageColor[LP_COUNT] = {C_GREEN, C_ALT, C_SOLAR, C_ENV_T, C_LOAD, C_BLUE,
                                              RGB565(190, 195, 210)};
// Environment page shows ONE pair at a time (B cycles): 0 = temp+humidity,
// 1 = pressure+gas. Four axes will not fit legibly on 320x170 — the Guition's
// 480x320 can stack both pairs, this cannot.
static volatile uint8_t gEnvPair = 0;
static volatile uint8_t gPage = LP_DASH;

// Settings page: a list cycled with B (short), the highlighted item activated with
// B (long / press-and-hold). Items act rather than edit — 2-button friendly.
enum SetItem : uint8_t { SET_BRIGHT = 0, SET_FLIP, SET_PAIR, SET_DIAG, SET_ROLE, SET_RESTART, SET_N };
static volatile uint8_t gSetSel = 0;
static volatile bool gFlip = false;      // 180° screen flip (rotation 3 normal / 1 flipped)

// Nested navigation: top-level pages, or a Diagnostics menu / detail screen reached
// by selecting "Diagnostics" in Settings. While in Diagnostics, A = back/up.
enum NavMode : uint8_t { NAV_PAGES = 0, NAV_DIAG, NAV_DETAIL };
static volatile uint8_t gNav = NAV_PAGES;
enum DiagItem : uint8_t { DG_MON = 0, DG_DISC, DG_LINK, DG_FW, DG_DEBUG, DG_BACK, DG_N };
static const char* kDiagName[DG_N] = {"Monitored devices", "Discovered Victron",
                                      "ESP-NOW link", "Firmware clone", "Debug capture", "< Back"};
static volatile uint8_t gDiagSel = 0;    // selected Diagnostics menu row
static volatile uint8_t gDetail = 0;     // open detail screen (a DiagItem)
static volatile uint8_t gFwSel = 0;      // Firmware detail: 0 = push, 1 = pull
static volatile bool gOtaPushReqL = false, gOtaPullReqL = false;  // processed on the loop

static const int kGraphWins[] = {1, 10, 60, 720, 1440};
static const char* kGraphWinName[] = {"1m", "10m", "1h", "12h", "24h"};
static volatile int gGraphWinIdx = 2;   // 1h
static volatile bool gGraphHideSoc = false;
static volatile uint8_t gWeekScope = 0; // 0 Today / 1 Trip / 2 Total
static const char* kScopeName[] = {"Today", "Trip", "Total"};
static volatile uint8_t gDashDetail = 0; // 0 TTG / 1 W / 2 starter
static volatile bool gFlowWatts = true;

static const uint8_t kBright[] = {100, 60, 25};
static volatile uint8_t gBrightIdx = 0;

static volatile bool gForce = true;      // request an immediate redraw

// Published snapshot (loop writes under mutex; display task reads), plus the two
// working copies either side of it.
//
// These are 4.9KB each and used to be BSS, which meant EVERY board in the
// universal image paid for all three — including the headless M5Capsule, which
// has no panel, no PSRAM and the tightest heap of the three boards. They are now
// allocated at panel bring-up, so a board with no screen never allocates them at
// all (~14.7KB of static RAM handed back to the Capsule's heap) while a board
// with a screen pays exactly the same total as before, just from the heap.
static DashData* gSnap = nullptr;        // published snapshot
static DashData* gTaskBuf = nullptr;     // display task's working copy
static DashData* gRenderBuf = nullptr;   // loop side's collection scratch
static SemaphoreHandle_t gSnapMux = nullptr;
static volatile bool gHaveSnap = false;

// ---- text helpers (GFX fonts, baseline-positioned) -------------------------
static Arduino_GFX* G() { return gLcd.gfx(); }
static int textW(const GFXfont* f, const char* s) {
    int16_t x1, y1; uint16_t w, h;
    G()->setFont(f);
    G()->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    return w;
}
static void T(int x, int y, const GFXfont* f, uint16_t col, const char* s) {
    G()->setFont(f); G()->setTextColor(col); G()->setCursor(x, y); G()->print(s);
}
static void Tf(int x, int y, const GFXfont* f, uint16_t col, const char* fmt, ...) {
    char b[48]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    T(x, y, f, col, b);
}
static void TR(int xr, int y, const GFXfont* f, uint16_t col, const char* fmt, ...) {
    char b[48]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    T(xr - textW(f, b), y, f, col, b);
}

// Classic built-in 5x7 GFX font for axis ticks: a fixed 6x8 cell, positioned by
// its TOP-left corner (the FreeSans fonts above position by baseline). Chart
// annotations are the one place the extra density beats the nicer glyphs — at
// 9pt a signed amp label such as "-100" eats 36 of the screen's 320 columns,
// which is gutter stolen from the plot itself.
static int textWs(const char* s) { return (int)strlen(s) * 6; }
static void Ts(int x, int yTop, uint16_t col, const char* fmt, ...) {
    char b[24]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    Arduino_GFX* g = G();
    g->setFont(nullptr); g->setTextSize(1);
    g->setTextColor(col); g->setCursor(x, yTop); g->print(b);
}
static void TsR(int xr, int yTop, uint16_t col, const char* fmt, ...) {
    char b[24]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    Ts(xr - textWs(b), yTop, col, "%s", b);
}

static uint16_t socColor(const DashData& d) {
    if (!d.battValid) return C_DIM;
    if (d.socCrit > 0 && d.soc <= d.socCrit) return C_RED;
    if (d.socWarn > 0 && d.soc <= d.socWarn) return C_AMBER;
    return C_GREEN;
}
// Battery current colour: charging green, discharging RED, idle grey.
static uint16_t ampColor(float a) { return a > 0.5f ? C_GREEN : a < -0.5f ? C_RED : C_LBL; }
// Charge-state colour from the resolved mode (respects the idle deadband).
static uint16_t stateColor(const DashData& d) {
    if (!d.mode) return C_LBL;
    if (!strcmp(d.mode, "Charging")) return C_GREEN;
    if (!strcmp(d.mode, "Discharging")) return C_RED;
    return C_LBL;
}
// SoC number/bar colour: RED while discharging, otherwise the level colour (so a
// low-SoC warning still shows when idle/charging).
static uint16_t socDisplayColor(const DashData& d) {
    if (d.battValid && d.mode && !strcmp(d.mode, "Discharging")) return C_RED;
    return socColor(d);
}

// ---- footer (page name · dots · B hint) ------------------------------------
static uint8_t gFooterNotice = 0;   // from the snapshot: firmware mismatch with the master
static void drawFooter(const char* bHint) {
    Arduino_GFX* g = G();
    g->fillRect(0, FOOT_Y, W, H - FOOT_Y, RGB565(22, 22, 28));
    g->drawFastHLine(0, FOOT_Y, W, RGB565(55, 55, 68));
    if (gFooterNotice) T(6, H - 5, F_S, C_AMBER, gFooterNotice == 1 ? "master newer: updating" : gFooterNotice == 2 ? "master older: push update" : "master newer: pull update");
    else T(6, H - 5, F_S, RGB565(205, 205, 216), kPageName[gPage]);
    int cx = W / 2 - (LP_COUNT - 1) * 7;
    for (int i = 0; i < LP_COUNT; ++i)
        g->fillCircle(cx + i * 14, FOOT_Y + 10, 3, i == gPage ? kPageColor[i] : RGB565(80, 80, 94));
    if (bHint && *bHint) TR(W - 6, H - 5, F_S, C_LBL, "%s", bHint);
}

// ---- Page: Dashboard -------------------------------------------------------
static void pageDash(const DashData& d) {
    Arduino_GFX* g = G();
    uint16_t sc = socDisplayColor(d);  // red while discharging
    // SoC number on a black background (coloured foreground).
    char num[8];
    if (d.battValid) snprintf(num, sizeof(num), "%d", (int)(d.soc + 0.5f));
    else strcpy(num, "--");
    T(6, 46, F_XL, d.battValid ? sc : C_DIM, num);
    T(6 + textW(F_XL, num) + 4, 46, F_L, d.battValid ? sc : C_DIM, "%");
    // SoC bar: coloured fill = charge remaining (red while discharging) on a dark track.
    int barx = 6, bary = 54, barw = 156, barh = 12;
    g->drawRect(barx, bary, barw, barh, C_DIM);
    if (d.battValid) {
        float s = d.soc < 0 ? 0 : d.soc > 100 ? 100 : d.soc;
        int fw = (int)((barw - 2) * s / 100.0f);
        g->fillRect(barx + 1, bary + 1, fw < 1 ? 1 : fw, barh - 2, sc);
    }
    // Mode: a square box filling the left column — state colour background, black text.
    const char* mode = d.mode ? d.mode : "--";
    uint16_t modeCol = d.worst >= 2 ? C_RED : d.worst == 1 ? C_AMBER : stateColor(d);
    g->fillRect(4, 72, 158, 26, modeCol);
    T(11, 72 + 18, F_M, BLACK, mode);

    // Right column (x 168..316, the widest that clears the mode box at 162): amps
    // is the hero — it changes fastest and decides charge/discharge — while the
    // voltage rides small on the heading row, since it barely moves. Below it the
    // Ah meter and the cycled detail line, both in F_M so they read at a glance.
    T(168, 14, F_S, C_LBL, "BATTERY");
    if (d.battValid) TR(316, 14, F_S, C_BLUE, "%.2fV", d.v);
    else             TR(316, 14, F_S, C_DIM, "%s", "--V");
    if (d.battValid) Tf(168, 46, F_L, ampColor(d.a), "%+.1fA", d.a);
    else             T(168, 46, F_L, C_DIM, "--A");
    // Ah meter: remaining / installed, mirroring the Guition battery card. With no
    // capacity configured, fall back to the BMV's consumed Ah like the web mimic.
    char ah[24];
    if (d.battCapAh > 0 && d.battValid)
        snprintf(ah, sizeof(ah), "%.0f/%.0f Ah", d.battCapAh * d.soc / 100.0f, d.battCapAh);
    else if (d.consumedValid) snprintf(ah, sizeof(ah), "%.0f Ah out", fabsf(d.consumedAh));
    else strcpy(ah, "-- Ah");
    T(168, 74, F_M, d.battValid ? C_TEXT : C_DIM, ah);
    char det[28];
    if (gDashDetail == 0) guition::ttgLabel(det, sizeof(det), d);
    else if (gDashDetail == 1) snprintf(det, sizeof(det), "Batt %.0fW", d.v * d.a);
    else if (d.starterValid) snprintf(det, sizeof(det), "Start %.1fV", d.starterV);
    else strcpy(det, "Start --");
    T(168, 98, F_M, C_TEXT, det);

    // Bottom chips: Solar / Alt / Charger / Load.
    const char* clbl[4] = {"SOLAR", "ALT", "CHG", "LOAD"};
    const uint16_t ccol[4] = {C_SOLAR, C_ALT, C_CHG, C_LOAD};
    const bool cok[4] = {d.solarValid, d.dcdcValid, d.chargerValid, d.loadValid};
    char cval[4][12];
    snprintf(cval[0], 12, "%.0fW", d.solarW);
    snprintf(cval[1], 12, "%.1fA", d.dcdcOutA);
    snprintf(cval[2], 12, "%.1fA", d.chargerA);
    snprintf(cval[3], 12, "%.1fA", d.loadA);
    for (int i = 0; i < 4; ++i) {
        int x = 5 + i * 78;
        g->drawFastHLine(x, 104, 72, cok[i] ? ccol[i] : C_DIM);
        T(x, 120, F_S, cok[i] ? ccol[i] : C_DIM, clbl[i]);
        T(x, 144, F_M, cok[i] ? C_TEXT : C_DIM, cok[i] ? cval[i] : "--");
    }
    drawFooter(gDashDetail == 0 ? "B: TTG" : gDashDetail == 1 ? "B: Watts" : "B: Starter");
}

// ---- Page: Power Flow ------------------------------------------------------
static void flowNode(int x, int y, int w, int h, uint16_t col, const char* lbl,
                     bool valid, float amps, float watts) {
    Arduino_GFX* g = G();
    g->drawRoundRect(x, y, w, h, 4, valid ? col : C_DIM);
    T(x + 7, y + 16, F_S, valid ? col : C_DIM, lbl);
    if (!valid) { T(x + 7, y + 37, F_M, C_DIM, "--"); return; }
    char b[16];
    if (gFlowWatts) snprintf(b, sizeof(b), "%.0fW", watts);
    else snprintf(b, sizeof(b), "%.1fA", amps);
    T(x + 7, y + 38, F_M, C_TEXT, b);
}
static void arrow(int x0, int x1, int y, uint16_t col, bool active) {
    Arduino_GFX* g = G();
    uint16_t c = active ? col : RGB565(48, 48, 58);
    g->drawFastHLine(x0 < x1 ? x0 : x1, y, abs(x1 - x0), c);
    int dir = x1 > x0 ? -1 : 1;
    g->fillTriangle(x1, y, x1 + dir * 6, y - 4, x1 + dir * 6, y + 4, c);
}
static void pageFlow(const DashData& d) {
    Arduino_GFX* g = G();
    float v = d.v;
    // Sources fill the full left column height; the battery spans the centre with
    // SoC / V / A / TTG; the load sits on the right — no wasted space up top.
    const int SY = 6, SH = 44, SGAP = 4;  // source boxes: 6..50, 54..98, 102..146
    flowNode(4, SY, 100, SH, C_SOLAR, "SOLAR", d.solarValid, d.solarA, d.solarW);
    flowNode(4, SY + SH + SGAP, 100, SH, C_ALT, "ALT", d.dcdcValid, d.dcdcOutA, d.dcdcOutA * v);
    flowNode(4, SY + 2 * (SH + SGAP), 100, SH, C_CHG, "CHARGER", d.chargerValid, d.chargerA, d.chargerA * v);

    // Battery box: black background, coloured readouts; the border carries the
    // charge state (red discharging / green charging / grey idle).
    int bx = 112, bw = 104, by = 6, bh = 140;
    g->fillRoundRect(bx, by, bw, bh, 7, BLACK);
    g->drawRoundRect(bx, by, bw, bh, 7, d.battValid ? stateColor(d) : C_DIM);
    if (d.battValid) {
        Tf(bx + 8, by + 40, F_XL, socDisplayColor(d), "%d", (int)(d.soc + 0.5f));
        T(bx + 8 + textW(F_XL, "88"), by + 34, F_S, socDisplayColor(d), "%");
        Tf(bx + 8, by + 74, F_M, C_BLUE, "%.2fV", d.v);
        Tf(bx + 8, by + 100, F_M, ampColor(d.a), "%+.1fA", d.a);
        char ttg[20];
        guition::ttgLabel(ttg, sizeof(ttg), d);
        T(bx + 8, by + 126, F_S, C_LBL, ttg);
    } else {
        T(bx + 8, by + 70, F_M, C_DIM, "BATT --");
    }

    // Load box on the right, aligned to the battery's vertical mid.
    flowNode(224, 52, 92, 48, C_LOAD, "LOAD", d.loadValid, d.loadA, d.loadA * v);

    // Arrows: each source into the battery at its own height; battery out to load.
    arrow(104, bx - 1, SY + SH / 2, C_SOLAR, d.solarValid && d.solarA > 0.1f);
    arrow(104, bx - 1, SY + SH + SGAP + SH / 2, C_ALT, d.dcdcValid && d.dcdcOutA > 0.1f);
    arrow(104, bx - 1, SY + 2 * (SH + SGAP) + SH / 2, C_CHG, d.chargerValid && d.chargerA > 0.1f);
    arrow(bx + bw + 1, 223, 76, C_LOAD, d.loadValid && d.loadA > 0.1f);
    drawFooter(gFlowWatts ? "B: Amps" : "B: Watts");
}

// ---- shared chart chrome (Graph / Environment / Week) -----------------------
// One geometry for every chart page, so the three read as the same instrument:
// a 2px-margin frame, 6x8 tick labels in gutters sized to the numbers actually
// drawn, a strip under the frame for the x-axis ends, and the legend on the row
// just above the footer. At 320x170 the plot IS the page — gutters and padding
// are the only things competing with it, so both are measured, never guessed.
struct Chart {
    int x0, y0, pw, ph;              // frame
    int plotX, plotW, plotY, plotH;  // interior
    int stripY;                      // top of the under-frame label strip
};
static Chart chartBegin(int gutL, int gutR, int frameH = 124) {
    Chart c;
    c.x0 = 2; c.y0 = 2; c.pw = W - 4; c.ph = frameH;
    G()->drawRect(c.x0, c.y0, c.pw, c.ph, RGB565(45, 45, 56));
    c.plotX = c.x0 + gutL;
    c.plotW = c.pw - gutL - gutR;
    c.plotY = c.y0 + 4;
    c.plotH = c.ph - 8;
    c.stripY = c.y0 + c.ph + 2;
    return c;
}
// Tick cells are 8px tall and drawn from their TOP, so -3 centres one on its
// gridline; the end ones are nudged back inside the frame.
static int chartTickY(const Chart& c, int y) {
    int ty = y - 3;
    if (ty < c.y0 + 2) ty = c.y0 + 2;
    if (ty > c.y0 + c.ph - 10) ty = c.y0 + c.ph - 10;
    return ty;
}
// Widest of a set of tick strings, as a gutter width.
static int chartGutter(const char* const* labels, int n, int pad) {
    int w = 0;
    for (int i = 0; i < n; ++i) { int t = textWs(labels[i]); if (t > w) w = t; }
    return w + pad;
}
struct Leg { const char* n; uint16_t c; bool on; };
static void chartLegend(const Leg* leg, int n) {
    Arduino_GFX* g = G();
    int lx = 4;
    for (int i = 0; i < n; ++i) {
        g->fillRect(lx, FOOT_Y - 15, 9, 9, leg[i].on ? leg[i].c : C_DIM);
        T(lx + 12, FOOT_Y - 6, F_S, leg[i].on ? C_TEXT : C_DIM, leg[i].n);
        lx += 12 + textW(F_S, leg[i].n) + 9;
    }
}
// Centred message for a chart with nothing to draw yet.
static void chartEmpty(const Chart& c, const char* msg) {
    T((W - textW(F_S, msg)) / 2, c.y0 + c.ph / 2 + 4, F_S, C_LBL, msg);
}

// ---- Page: Graph -----------------------------------------------------------
// Line plot of one deci-unit series across the plot rect, mapping [lo,hi] to the
// height. Deliberately the same math as gfx_graph.cpp's linePlot (and the web
// chart's Y()): value -> fraction of the axis range -> pixel, bottom-up. Gaps
// (-32768) break the line; `dashed` renders the SoC overlay broken, as there.
static void plotSeries(const int16_t* a, int n, float scale, int px0, int pw, int py0, int ph,
                       float lo, float hi, uint16_t col, bool dashed) {
    if (n < 2 || pw < 2 || ph < 2 || hi <= lo) return;
    Arduino_GFX* g = G();
    int prevX = -1, prevY = -1;
    for (int i = 0; i < n; ++i) {
        if (a[i] == -32768) { prevX = -1; continue; }
        float v = a[i] / scale;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        int x = px0 + (int)lroundf((float)i * (pw - 1) / (n - 1));
        int y = py0 + ph - 1 - (int)lroundf((v - lo) / (hi - lo) * (ph - 1));
        if (prevX >= 0) {
            if (!dashed || ((x >> 2) & 1)) g->drawLine(prevX, prevY, x, y, col);
        } else {
            g->drawPixel(x, y, col);
        }
        prevX = x; prevY = y;
    }
}
static void pageGraph(const DashData& d) {
    Arduino_GFX* g = G();
    const bool haveData = d.histCount >= 2;
    const bool showSoc = !gGraphHideSoc;
    const int16_t* series[5] = {d.histBatt, d.histSolar, d.histCharger, d.histDcdc, d.histLoad};

    // Auto-scale the Amps axis across the five current series exactly as
    // gfx_graph.cpp and the web chart do: track the real min and max, always
    // include zero, and never let the range close below 2A. A symmetric
    // +/-peak axis (what this page used to do) draws the SAME samples with a
    // different shape — an all-positive day lands entirely in the top half at
    // half amplitude — so the small screen disagreed with every other view.
    float mn = 0, mx = 0;
    if (haveData) {
        for (int s = 0; s < 5; ++s)
            for (int i = 0; i < d.histCount; ++i) {
                if (series[s][i] == -32768) continue;
                float v = series[s][i] / 10.0f;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        if (mx - mn < 2) mx = mn + 2;
    }

    const int dec = (mx - mn) >= 10 ? 0 : 1;
    char tick[3][12];
    const char* tp[3] = {tick[0], tick[1], tick[2]};
    for (int gi = 0; gi <= 2; ++gi)
        snprintf(tick[gi], sizeof(tick[gi]), "%.*f", dec, mn + (mx - mn) * gi / 2);
    const int gutL = haveData ? chartGutter(tp, 3, 5) : 4;
    const char* pct[3] = {"0", "50", "100"};
    const int gutR = (haveData && showSoc) ? chartGutter(pct, 3, 6) : 4;
    Chart c = chartBegin(gutL, gutR);

    if (!haveData) {
        chartEmpty(c, d.graphSyncing ? "syncing..." : "no data yet");
    } else {
        auto yA = [&](float v) {
            return c.plotY + c.plotH - 1 - (int)lroundf((v - mn) / (mx - mn) * (c.plotH - 1));
        };
        // Three gridlines (min / mid / max). Both axes span the full plot height,
        // so the 0/50/100 % ticks land on the same three lines.
        for (int gi = 0; gi <= 2; ++gi) {
            int y = yA(mn + (mx - mn) * gi / 2);
            g->drawFastHLine(c.plotX, y, c.plotW, RGB565(50, 50, 62));
            int ty = chartTickY(c, y);
            TsR(c.plotX - 3, ty, C_LBL, "%s", tick[gi]);
            if (showSoc) Ts(c.plotX + c.plotW + 4, ty, C_SOC, "%s", pct[gi]);
        }
        if (mn < 0 && mx > 0) g->drawFastHLine(c.plotX, yA(0), c.plotW, RGB565(95, 95, 112));

        const uint16_t col[5] = {C_GREEN, C_SOLAR, C_CHG, C_ALT, C_LOAD};
        for (int s = 0; s < 5; ++s)
            plotSeries(series[s], d.histCount, 10.0f, c.plotX, c.plotW, c.plotY, c.plotH,
                       mn, mx, col[s], false);
        if (showSoc)
            plotSeries(d.histSoc, d.histCount, 10.0f, c.plotX, c.plotW, c.plotY, c.plotH,
                       0.0f, 100.0f, C_SOC, true);
    }
    // Time axis in the strip under the frame: oldest .. now, as on the Guition
    // and the web. The left end doubles as the zoom readout (page colour), so
    // cycling the window with B still shows plainly which span is on screen.
    Ts(c.plotX, c.stripY, kPageColor[LP_GRAPH], "-%s", kGraphWinName[gGraphWinIdx]);
    TsR(c.plotX + c.plotW, c.stripY, C_DIM, "now");

    Leg leg[6] = {{"Bat", C_GREEN, true}, {"Sol", C_SOLAR, true}, {"Chg", C_CHG, true},
                  {"Alt", C_ALT, true}, {"Load", C_LOAD, true}, {"SoC", C_SOC, showSoc}};
    chartLegend(leg, 6);
    drawFooter("B:zoom  hold:SoC");
}

// ---- Page: Environment -----------------------------------------------------
// One pair of channels at a time (B cycles), each on its OWN auto-scaled axis:
// the first is solid and labelled down the left, the second dashed and labelled
// down the right, in the series colour. Pairs and colours match the web card and
// the Guition page. Only one pair is shown because four axis gutters will not fit
// legibly across 320 px.
struct EnvCh {
    const int16_t* vals;
    float scale;        // stored / scale = real units (10 = deci-*, 1 = whole kOhm)
    const char* name;
    const char* unit;
    int dp;
    uint16_t col;
    float minSpan;      // keeps a flat trace a flat line instead of amplified noise
    bool nowOk;
    float now;
};

// Auto-scaled [lo,hi] over n points plus the live value; false when nothing valid.
static bool envSpan(const EnvCh& c, int n, float& lo, float& hi) {
    bool any = false; float mn = 0, mx = 0;
    for (int i = 0; i < n; ++i) {
        if (c.vals[i] == -32768) continue;
        float v = c.vals[i] / c.scale;
        if (!any) { mn = mx = v; any = true; } else { if (v < mn) mn = v; if (v > mx) mx = v; }
    }
    if (c.nowOk) { if (!any) { mn = mx = c.now; any = true; }
                   else { if (c.now < mn) mn = c.now; if (c.now > mx) mx = c.now; } }
    if (!any) return false;
    float sp = mx - mn;
    if (sp < c.minSpan) { float mid = (mn + mx) / 2; lo = mid - c.minSpan / 2; hi = mid + c.minSpan / 2; }
    else { lo = mn - sp * 0.1f; hi = mx + sp * 0.1f; }
    return true;
}

static void plotEnv(const EnvCh& c, int n, int px0, int pw, int py0, int ph,
                    float lo, float hi, bool dashed) {
    if (n < 2 || hi <= lo) return;
    Arduino_GFX* g = G();
    int prevX = -1, prevY = -1;
    for (int i = 0; i < n; ++i) {
        if (c.vals[i] == -32768) { prevX = -1; continue; }
        float v = c.vals[i] / c.scale;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        int x = px0 + (n <= 1 ? 0 : i * pw / (n - 1));
        int y = py0 + ph - 1 - (int)((v - lo) / (hi - lo) * (ph - 1));
        if (prevX >= 0) {
            if (!dashed || ((x >> 2) & 1)) g->drawLine(prevX, prevY, x, y, c.col);
        } else {
            g->drawPixel(x, y, c.col);
        }
        prevX = x; prevY = y;
    }
}

static void pageEnv(const DashData& d) {
    Arduino_GFX* g = G();
    if (!d.envPresent) {
        Chart c = chartBegin(4, 4);
        chartEmpty(c, "no environment sensor");
        drawFooter("A:page");
        return;
    }

    EnvCh L, R;
    if (gEnvPair == 0) {
        L = {d.histEnvT, 10.0f, "Temp", "C",   1, C_ENV_T, 1.0f,  d.envValid,    d.envTempC};
        R = {d.histEnvH, 10.0f, "Hum",  "%",   1, C_ENV_H, 1.0f,  d.envValid,    d.envHumidity};
    } else {
        L = {d.histEnvP, 10.0f, "Press", "hPa", 1, C_ENV_P, 2.0f,  d.envValid,    d.envPressureHpa};
        R = {d.histEnvG, 1.0f,  "Gas",   "k",   0, C_ENV_G, 10.0f, d.envGasValid, d.envGasKohm};
    }

    float lLo = 0, lHi = 0, rLo = 0, rHi = 0;
    bool haveL = envSpan(L, d.histCount, lLo, lHi);
    bool haveR = envSpan(R, d.histCount, rLo, rHi);

    // Gutters sized to the ticks each channel actually needs. Pressure's
    // "1019.3" is the widest label any chart page can produce, and at 9pt it did
    // not fit the old fixed 34px gutter at all — the tick font plus a measured
    // gutter is what finally makes that pair legible.
    char lt[3][12], rt[3][12];
    const char* lp[3] = {lt[0], lt[1], lt[2]};
    const char* rp[3] = {rt[0], rt[1], rt[2]};
    for (int gi = 0; gi <= 2; ++gi) {
        snprintf(lt[gi], sizeof(lt[gi]), "%.*f", L.dp, lLo + (lHi - lLo) * gi / 2);
        snprintf(rt[gi], sizeof(rt[gi]), "%.*f", R.dp, rLo + (rHi - rLo) * gi / 2);
    }
    Chart c = chartBegin(haveL ? chartGutter(lp, 3, 5) : 4,
                         haveR ? chartGutter(rp, 3, 6) : 4);

    if (d.histCount < 2) {
        chartEmpty(c, d.graphSyncing ? "syncing..." : "no data yet");
    } else {
        for (int gi = 0; gi <= 2; ++gi) {  // 3 gridlines, labelled on both sides
            int y = c.plotY + c.plotH - 1 - gi * (c.plotH - 1) / 2;
            g->drawFastHLine(c.plotX, y, c.plotW, RGB565(50, 50, 62));
            int ty = chartTickY(c, y);
            // Axis labels stay in the series colour: with two unrelated units
            // sharing one frame, the colour is what says which axis is which.
            if (haveL) TsR(c.plotX - 3, ty, L.col, "%s", lt[gi]);
            if (haveR) Ts(c.plotX + c.plotW + 4, ty, R.col, "%s", rt[gi]);
        }
        if (haveL) plotEnv(L, d.histCount, c.plotX, c.plotW, c.plotY, c.plotH, lLo, lHi, false);
        if (haveR) plotEnv(R, d.histCount, c.plotX, c.plotW, c.plotY, c.plotH, rLo, rHi, true);
    }
    // Same strip as the Graph page — this page shares the Graph's zoom window.
    Ts(c.plotX, c.stripY, kPageColor[LP_ENV], "-%s", kGraphWinName[gGraphWinIdx]);
    TsR(c.plotX + c.plotW, c.stripY, C_DIM, "now");

    // The legend row doubles as the live readout for the pair on screen.
    char ln[26], rn[26];
    if (L.nowOk) snprintf(ln, sizeof(ln), "%s %.*f%s", L.name, L.dp, L.now, L.unit);
    else         snprintf(ln, sizeof(ln), "%s --", L.name);
    if (R.nowOk) snprintf(rn, sizeof(rn), "%s %.*f%s", R.name, R.dp, R.now, R.unit);
    else         snprintf(rn, sizeof(rn), "%s --", R.name);
    Leg leg[2] = {{ln, L.col, true}, {rn, R.col, true}};
    chartLegend(leg, 2);

    drawFooter("B:channel  hold:zoom");
}

// ---- Page: Week ------------------------------------------------------------
static const uint32_t kYmdMin = 20000000;  // dayStamp at/above this is a yyyymmdd date

static void pageWeek(const DashData& d) {
    Arduino_GFX* g = G();
    // Seven slots: the last six ARCHIVED days plus TODAY's live bucket on the
    // right, which is how the web chart and the Guition page build it. Plotting
    // only the archive — what this page did — meant the newest bar was
    // *yesterday*, wearing a "now" label, and nothing on the page moved until
    // midnight. That is why the week stopped appearing to trend.
    const int NS = 7;
    float inAh[NS] = {0}, outAh[NS] = {0}, sol[NS] = {0}, dcd[NS] = {0}, chg[NS] = {0}, net[NS] = {0};
    bool used[NS] = {false};
    char lbl[NS][8];
    for (int s = 0; s < NS; ++s) {
        lbl[s][0] = '\0';
        if (s == NS - 1) {
            sol[s] = d.statToday.solarAh; dcd[s] = d.statToday.dcdcAh;
            chg[s] = d.statToday.chargerAh; net[s] = d.statToday.inAh;
            outAh[s] = d.statToday.outAh > d.statToday.loadAh ? d.statToday.outAh : d.statToday.loadAh;
            used[s] = true;
            strcpy(lbl[s], "now");
        } else {
            int di = d.dayCount - (NS - 1 - s);
            if (di < 0 || di >= d.dayCount) continue;
            sol[s] = d.daySolarAh[di]; dcd[s] = d.dayDcdcAh[di];
            chg[s] = d.dayChargerAh[di]; net[s] = d.dayChargedAh[di];
            outAh[s] = d.dayDischargedAh[di] > d.dayLoadAh[di] ? d.dayDischargedAh[di] : d.dayLoadAh[di];
            used[s] = true;
            uint32_t st = d.dayStamp[di];
            if (d.clockOk && st >= kYmdMin)
                snprintf(lbl[s], sizeof(lbl[s]), "%u/%u", (unsigned)((st / 100) % 100),
                         (unsigned)(st % 100));
            else
                snprintf(lbl[s], sizeof(lbl[s]), "%u", (unsigned)st);
        }
        inAh[s] = sol[s] + dcd[s] + chg[s];
        if (net[s] > inAh[s]) inAh[s] = net[s];   // NET charge when no monitored source claimed it (same rule as the web)
    }
    float mx = 1.0f;
    for (int s = 0; s < NS; ++s) {
        if (inAh[s] > mx) mx = inAh[s];
        if (outAh[s] > mx) mx = outAh[s];
    }

    // Same chrome as the Graph and Environment pages, on a shorter frame so the
    // scope meter keeps the bottom third.
    char tick[12];
    snprintf(tick, sizeof(tick), "%.0f", mx);
    const char* tp[2] = {tick, "0"};
    Chart c = chartBegin(chartGutter(tp, 2, 5), 4, 80);

    const int midY = c.plotY + c.plotH / 2, halfH = c.plotH / 2 - 2;
    g->drawFastHLine(c.plotX, c.plotY, c.plotW, RGB565(38, 38, 48));
    g->drawFastHLine(c.plotX, c.plotY + c.plotH - 1, c.plotW, RGB565(38, 38, 48));
    g->drawFastHLine(c.plotX, midY, c.plotW, RGB565(70, 70, 84));
    // Coloured axis: the charge scale above the zero line, discharge below.
    TsR(c.plotX - 3, chartTickY(c, c.plotY), C_GREEN, "%s", tick);
    TsR(c.plotX - 3, chartTickY(c, c.plotY) + 9, C_DIM, "Ah");
    TsR(c.plotX - 3, chartTickY(c, midY), C_DIM, "0");
    TsR(c.plotX - 3, chartTickY(c, c.plotY + c.plotH - 1), C_LOAD, "%s", tick);

    const int slot = c.plotW / NS, bw = slot - 8;
    for (int s = 0; s < NS; ++s) {
        int x = c.plotX + s * slot + 4;
        if (used[s]) {
            // Stack the charge sources in their own colours, as the web chart and
            // the Guition page do, so a bar says WHERE the amp-hours came from.
            const float src[3] = {sol[s], dcd[s], chg[s]};
            const uint16_t sc[3] = {C_SOLAR, C_ALT, C_CHG};
            int acc = 0;
            for (int k = 0; k < 3; ++k) {
                int h = (int)lroundf(src[k] / mx * halfH);
                if (h <= 0) continue;
                g->fillRect(x, midY - acc - h, bw, h, sc[k]);
                acc += h;
            }
            int rest = (int)lroundf(inAh[s] / mx * halfH) - acc;   // unattributed remainder, grey
            if (rest > 0) g->fillRect(x, midY - acc - rest, bw, rest, C_LBL);
            int oh = (int)lroundf(outAh[s] / mx * halfH);
            if (oh > 0) g->fillRect(x, midY + 1, bw, oh, C_LOAD);
        }
        if (lbl[s][0])
            Ts(x + bw / 2 - textWs(lbl[s]) / 2, c.stripY, s == NS - 1 ? C_TEXT : C_DIM,
               "%s", lbl[s]);
    }

    // Scope meter under the chart (B cycles Today / Trip / Total).
    const DashData::StatMeter& m =
        gWeekScope == 0 ? d.statToday : gWeekScope == 1 ? d.statTrip : d.statTotal;
    g->drawFastHLine(6, 94, W - 12, RGB565(45, 45, 56));
    Tf(6, 112, F_M, kPageColor[LP_WEEK], "%s", kScopeName[gWeekScope]);
    int hh = (int)(m.durSecs / 3600), mm = (int)((m.durSecs % 3600) / 60);
    TR(W - 6, 110, F_S, C_LBL, "%dh%02dm", hh, mm);
    Tf(6, 129, F_S, C_GREEN, "In %.1f", m.inAh);
    Tf(6, 145, F_S, C_LOAD, "Out %.1f", m.outAh);
    Tf(118, 129, F_S, C_SOLAR, "Sol %.1f", m.solarAh);
    Tf(118, 145, F_S, C_ALT, "Alt %.1f", m.dcdcAh);
    Tf(224, 129, F_S, C_CHG, "Chg %.1f", m.chargerAh);
    Tf(224, 145, F_S, C_LOAD, "Ld %.1f", m.loadAh);
    drawFooter("B: scope");
}

// ---- Page: Status ----------------------------------------------------------
static void kv(int x, int y, const char* k, const char* v, uint16_t vc) {
    T(x, y, F_S, C_LBL, k);
    T(x + 74, y, F_S, vc, v);
}
static void pageInfo(const DashData& d) {
    bool slave = (gRole == ROLE_SLAVE);
    int x = 6, y = 18;
    T(x, y, F_M, kPageColor[LP_INFO], slave ? "SLAVE" : "MASTER");
    TR(W - 6, y, F_S, C_LBL, "fw %s", d.version);
    y = 42;
    int dy = 18;
    char b[28];
    int up = d.uptimeSec;
    snprintf(b, sizeof(b), "%dh%02dm", up / 3600, (up % 3600) / 60);
    kv(x, y, "Uptime", b, C_TEXT); y += dy;
    if (!slave) {
        kv(x, y, "AP", d.apSsid, C_TEXT); y += dy;
        kv(x, y, "IP", d.ipStr, C_BLUE); y += dy;
        snprintf(b, sizeof(b), "%d paired / %d seen", d.devPaired, d.devSeen);
        kv(x, y, "Devices", b, C_TEXT); y += dy;
        if (d.pairing) { snprintf(b, sizeof(b), "OPEN %ds", d.pairSecLeft); kv(x, y, "Pairing", b, C_GREEN); }
        else kv(x, y, "ESP-NOW", d.espNowOk ? "ready" : "down", d.espNowOk ? C_GREEN : C_AMBER);
        y += dy;
    } else {
        if (d.masterId) { snprintf(b, sizeof(b), "%08lX", (unsigned long)d.masterId); kv(x, y, "Master", b, C_TEXT); }
        else kv(x, y, "Master", "unpaired", C_AMBER);
        y += dy;
        if (d.pairing) snprintf(b, sizeof(b), "PAIRING %ds", d.pairSecLeft);
        else snprintf(b, sizeof(b), "%s ch%d drop%lu", d.linkLive ? "live" : d.linkStale ? "stale" : "--",
                      d.linkChannel, (unsigned long)d.linkDrops);
        kv(x, y, "Link", b, d.pairing || d.linkLive ? C_GREEN : C_AMBER); y += dy;
    }
    snprintf(b, sizeof(b), "%d%%  (RAM %luKB)", kBright[gBrightIdx], (unsigned long)d.freeHeapKb);
    kv(x, y, "Bright", b, C_TEXT);
    drawFooter("B:bright  hold:pair");
}

// ---- Page: Settings --------------------------------------------------------
static void pageSettings(const DashData& d) {
    Arduino_GFX* g = G();
    const char* names[SET_N] = {"Brightness", "Flip 180", "Pair", "Diagnostics", "Role", "Restart"};
    char val[SET_N][20];
    snprintf(val[SET_BRIGHT], 20, "%d%%", kBright[gBrightIdx]);
    snprintf(val[SET_FLIP], 20, "%s", gFlip ? "On" : "Off");
    if (d.pairing) snprintf(val[SET_PAIR], 20, "OPEN %ds", d.pairSecLeft);
    else snprintf(val[SET_PAIR], 20, "%s", gRole == ROLE_SLAVE ? "adopt" : "open");
    snprintf(val[SET_DIAG], 20, "%s", ">");
    snprintf(val[SET_ROLE], 20, "%s", gRole == ROLE_SLAVE ? "-> Master" : "-> Slave");
    snprintf(val[SET_RESTART], 20, "reboot");
    int y0 = 6, rh = 23;
    for (int i = 0; i < SET_N; ++i) {
        int ry = y0 + i * rh;
        bool sel = (i == gSetSel);
        if (sel) {
            g->fillRoundRect(4, ry, W - 8, rh - 3, 4, RGB565(38, 42, 54));
            g->drawRoundRect(4, ry, W - 8, rh - 3, 4, kPageColor[LP_SET]);
        }
        T(12, ry + 16, F_M, sel ? C_TEXT : C_LBL, names[i]);
        TR(W - 12, ry + 16, F_M, sel ? kPageColor[LP_SET] : C_LBL, "%s", val[i]);
    }
    drawFooter("B:next  hold:select");
}

// ---- Diagnostics: menu + detail screens (reached from Settings) ------------
static void drawBar(const char* left, const char* right) {
    Arduino_GFX* g = G();
    g->fillRect(0, FOOT_Y, W, H - FOOT_Y, RGB565(22, 22, 28));
    g->drawFastHLine(0, FOOT_Y, W, RGB565(55, 55, 68));
    T(6, H - 5, F_S, RGB565(205, 205, 216), left);
    if (right && *right) TR(W - 6, H - 5, F_S, C_LBL, "%s", right);
}
static void pageDiagMenu(const DashData& d) {
    Arduino_GFX* g = G();
    int y0 = 6, rh = 23;
    for (int i = 0; i < DG_N; ++i) {
        int ry = y0 + i * rh;
        bool sel = (i == gDiagSel);
        if (sel) {
            g->fillRoundRect(4, ry, W - 8, rh - 3, 4, RGB565(38, 42, 54));
            g->drawRoundRect(4, ry, W - 8, rh - 3, 4, kPageColor[LP_SET]);
        }
        T(12, ry + 16, F_M, sel ? C_TEXT : C_LBL, kDiagName[i]);
        if (i == DG_DEBUG)
            TR(W - 12, ry + 16, F_M, d.debugCapture ? C_GREEN : C_DIM, "%s", d.debugCapture ? "ON" : "off");
        else if (i == DG_DISC) TR(W - 12, ry + 16, F_M, C_LBL, "%d", d.discCount);
        else if (i == DG_MON) TR(W - 12, ry + 16, F_M, C_LBL, "%d", d.monCount);
    }
    drawBar("Diagnostics", "A:back  B:next  hold:open");
}
static void diagDetailMon(const DashData& d) {
    T(6, 15, F_M, C_TEXT, "Monitored devices");
    if (d.monCount == 0)
        T(6, 40, F_S, C_LBL, gRole == ROLE_SLAVE ? "(slave has no BLE devices)" : "none configured");
    for (int i = 0; i < d.monCount; ++i) {
        int y = 34 + i * 18;
        G()->fillCircle(9, y - 4, 3, d.monLive[i] ? C_GREEN : C_DIM);
        T(18, y, F_S, C_TEXT, d.monName[i]);
        TR(W - 6, y, F_S, C_LBL, "%s", d.monVal[i]);
    }
    drawBar("Monitored", "A:back");
}
static void diagDetailDisc(const DashData& d) {
    T(6, 15, F_M, C_TEXT, "Discovered Victron");
    if (d.discCount == 0)
        T(6, 40, F_S, C_LBL, gRole == ROLE_SLAVE ? "(slave has no BLE)" : "none in range");
    for (int i = 0; i < d.discCount; ++i) {
        int y = 34 + i * 18;
        const char* nm = d.discName[i][0] ? d.discName[i] : "(unnamed)";
        T(6, y, F_S, C_SOLAR, nm);
        T(150, y, F_S, C_DIM, d.discMac[i]);
        TR(W - 6, y, F_S, C_LBL, "%ddBm", d.discRssi[i]);
    }
    drawBar("Discovered", "A:back");
}
static void diagDetailLink(const DashData& d) {
    T(6, 15, F_M, C_TEXT, "ESP-NOW link");
    char b[28];
    int y = 36;
    kv(6, y, "Radio", d.espNowOk ? "up" : "down", d.espNowOk ? C_GREEN : C_AMBER); y += 18;
    if (gRole == ROLE_SLAVE) {
        snprintf(b, sizeof(b), "%08lX", (unsigned long)d.masterId);
        kv(6, y, "Master", d.masterId ? b : "unpaired", d.masterId ? C_TEXT : C_AMBER); y += 18;
        kv(6, y, "State", d.linkLive ? "live" : d.linkStale ? "stale" : "--",
           d.linkLive ? C_GREEN : C_AMBER); y += 18;
        snprintf(b, sizeof(b), "ch%d   drops %lu", d.linkChannel, (unsigned long)d.linkDrops);
        kv(6, y, "Signal", b, C_TEXT); y += 18;
    } else {
        snprintf(b, sizeof(b), "%08lX", (unsigned long)d.masterId);
        kv(6, y, "My ID", b, C_TEXT); y += 18;
        snprintf(b, sizeof(b), "ch%d   seq %u", d.linkChannel, d.snapSeq);
        kv(6, y, "Broadcast", b, C_TEXT); y += 18;
        kv(6, y, "Pairing", d.pairing ? "OPEN" : "idle", d.pairing ? C_GREEN : C_LBL); y += 18;
    }
    drawBar("Link", "A:back");
}
static void diagDetailFw(const DashData& d) {
    Arduino_GFX* g = G();
    T(6, 15, F_M, C_TEXT, "Firmware clone");
    char b[40];
    int y = 34;
    kv(6, y, "Local", d.version, C_TEXT); y += 17;
    if (d.otaPeerKnown) { snprintf(b, sizeof(b), "%s (%s)", d.otaPeerVer, d.otaPeerRel); kv(6, y, "Peer", b, C_BLUE); }
    else kv(6, y, "Peer", "(unknown)", C_DIM);
    y += 17;
    kv(6, y, "Status", d.otaStatus[0] ? d.otaStatus : "idle", C_LBL); y += 16;
    if (d.otaBusy) {
        g->drawRect(6, y, W - 12, 9, C_DIM);
        g->fillRect(7, y + 1, (W - 14) * d.otaPct / 100, 7, C_GREEN);
    }
    y = 118;
    // Push / Pull selector — B toggles, hold executes.
    const char* opt[2] = {"Push", "Pull"};
    for (int i = 0; i < 2; ++i) {
        int bx = 40 + i * 130, bw = 110;
        bool sel = (gFwSel == i);
        g->fillRoundRect(bx, y, bw, 24, 4, sel ? kPageColor[LP_SET] : RGB565(40, 40, 48));
        T(bx + bw / 2 - textW(F_M, opt[i]) / 2, y + 17, F_M, sel ? BLACK : C_LBL, opt[i]);
    }
    drawBar("Firmware", "A:back  B:sel  hold:go");
}
static void pageDiagDetail(const DashData& d) {
    switch (gDetail) {
        case DG_MON:  diagDetailMon(d);  break;
        case DG_DISC: diagDetailDisc(d); break;
        case DG_LINK: diagDetailLink(d); break;
        default:      diagDetailFw(d);   break;
    }
}

// ---- frame render (display task side) --------------------------------------
static void drawFrame(const DashData& d) {
    G()->fillScreen(C_BG);
    G()->drawFastHLine(0, 0, W, kPageColor[gNav == NAV_PAGES ? gPage : LP_SET]);
    if (gNav == NAV_DIAG) { pageDiagMenu(d); gLcd.flush(); return; }
    if (gNav == NAV_DETAIL) { pageDiagDetail(d); gLcd.flush(); return; }
    switch (gPage) {
        case LP_DASH:  pageDash(d);  break;
        case LP_FLOW:  pageFlow(d);  break;
        case LP_GRAPH: pageGraph(d); break;
        case LP_ENV:   pageEnv(d);   break;
        case LP_WEEK:  pageWeek(d);  break;
        case LP_SET:   pageSettings(d); break;
        default:       pageInfo(d);  break;
    }
    gLcd.flush();
}

// ---- buttons ---------------------------------------------------------------
static void applyBrightness() {
    gLcd.setBrightness(kBright[gBrightIdx]);
    Preferences p; p.begin("vicdisp", false); p.putUChar("bright", kBright[gBrightIdx]); p.end();
}
static void openPairWindow() {
    if (gRole == ROLE_SLAVE) { gRx.startAdopt(); Serial.println("[lilygo] slave adopt window open"); }
    else { startPairing(); Serial.println("[lilygo] master pairing window open"); }
}
static void settingsActivate() {
    switch (gSetSel) {
        case SET_BRIGHT:
            gBrightIdx = (gBrightIdx + 1) % (uint8_t)sizeof(kBright); applyBrightness(); break;
        case SET_FLIP:
            gFlip = !gFlip; gLcd.setRotation(gFlip ? 1 : 3);
            { Preferences p; p.begin("vicdisp", false); p.putBool("flip", gFlip); p.end(); }
            Serial.printf("[lilygo] flip %s\n", gFlip ? "on" : "off"); break;
        case SET_PAIR:    openPairWindow(); break;
        case SET_DIAG:    gNav = NAV_DIAG; gDiagSel = 0; break;
        case SET_ROLE:    gRoleReq = true; Serial.println("[lilygo] role toggle requested (reboot)"); break;
        case SET_RESTART: gRebootReq = true; Serial.println("[lilygo] restart requested"); break;
    }
}
// Diagnostics menu item selected (B long).
static void diagActivate() {
    switch (gDiagSel) {
        case DG_MON:  gNav = NAV_DETAIL; gDetail = DG_MON;  break;
        case DG_DISC: gNav = NAV_DETAIL; gDetail = DG_DISC; break;
        case DG_LINK: gNav = NAV_DETAIL; gDetail = DG_LINK; break;
        case DG_FW:   gNav = NAV_DETAIL; gDetail = DG_FW; gFwSel = 0; break;
        case DG_DEBUG: gDebugCapture = !gDebugCapture;
            Serial.printf("[lilygo] debug capture %s\n", gDebugCapture ? "on" : "off"); break;
        case DG_BACK: gNav = NAV_PAGES; gPage = LP_SET; break;
    }
}
static void bShort() {
    if (gNav == NAV_DIAG) { gDiagSel = (gDiagSel + 1) % DG_N; return; }
    if (gNav == NAV_DETAIL) { if (gDetail == DG_FW) gFwSel ^= 1; return; }
    switch (gPage) {
        case LP_DASH:  gDashDetail = (gDashDetail + 1) % 3; break;
        case LP_FLOW:  gFlowWatts = !gFlowWatts; break;
        case LP_GRAPH: gGraphWinIdx = (gGraphWinIdx + 1) % 5; setGraphWindowMinutes(kGraphWins[gGraphWinIdx]); break;
        case LP_ENV:   gEnvPair ^= 1; break;
        case LP_WEEK:  gWeekScope = (gWeekScope + 1) % 3; break;
        case LP_SET:   gSetSel = (gSetSel + 1) % SET_N; break;
        default:       gBrightIdx = (gBrightIdx + 1) % (uint8_t)sizeof(kBright); applyBrightness(); break;
    }
}
static void bLong() {
    if (gNav == NAV_DIAG) { diagActivate(); return; }
    if (gNav == NAV_DETAIL) {
        if (gDetail == DG_FW) {  // execute Push / Pull (started on the loop task)
            if (gFwSel == 0) gOtaPushReqL = true; else gOtaPullReqL = true;
            Serial.printf("[lilygo] OTA %s requested\n", gFwSel == 0 ? "push" : "pull");
        }
        return;
    }
    if (gPage == LP_GRAPH) gGraphHideSoc = !gGraphHideSoc;
    // Env shares the Graph's history window, so hold-B cycles the same zoom.
    else if (gPage == LP_ENV) {
        gGraphWinIdx = (gGraphWinIdx + 1) % 5;
        setGraphWindowMinutes(kGraphWins[gGraphWinIdx]);
    }
    else if (gPage == LP_INFO) openPairWindow();
    else if (gPage == LP_SET) settingsActivate();
}
// A = next page normally; back/up one level while inside Diagnostics.
static void aShort() {
    if (gNav == NAV_DETAIL) { gNav = NAV_DIAG; return; }
    if (gNav == NAV_DIAG) { gNav = NAV_PAGES; gPage = LP_SET; return; }
    gPage = (gPage + 1) % LP_COUNT;
}
static void aLong() {
    if (gNav == NAV_DETAIL) { gNav = NAV_DIAG; return; }
    if (gNav == NAV_DIAG) { gNav = NAV_PAGES; gPage = LP_SET; return; }
    gPage = (gPage + LP_COUNT - 1) % LP_COUNT;
}

static void pollButton(bool downNow, bool& down, uint32_t& tDown, bool& longFired,
                       void (*onShort)(), void (*onLong)()) {
    uint32_t now = millis();
    const uint32_t kLongMs = 550;
    if (downNow && !down) { down = true; tDown = now; longFired = false; }
    else if (downNow && down && !longFired && now - tDown >= kLongMs) { longFired = true; onLong(); gForce = true; }
    else if (!downNow && down) { down = false; if (!longFired) { onShort(); gForce = true; } }
}

// ---- display task: poll buttons ~60Hz, redraw from the snapshot ~4Hz --------
static void lilygoTask(void*) {
    DashData& local = *gTaskBuf;  // allocated by lilygoBringUp before this task starts
    bool aDown = false, bDown = false, aLongF = false, bLongF = false;
    uint32_t aT = 0, bT = 0, lastDraw = 0;
    for (;;) {
        pollButton(gLcd.buttonA(), aDown, aT, aLongF, aShort, aLong);
        pollButton(gLcd.buttonB(), bDown, bT, bLongF, bShort, bLong);
        uint32_t now = millis();
        if (gHaveSnap && (gForce || now - lastDraw >= 250)) {
            lastDraw = now;
            gForce = false;
            if (xSemaphoreTake(gSnapMux, pdMS_TO_TICKS(20)) == pdTRUE) {
                memcpy(&local, gSnap, sizeof(DashData));
                xSemaphoreGive(gSnapMux);
                drawFrame(local);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(16));  // ~60 Hz button polling
    }
}

// ---- entry points (declared in app.h) --------------------------------------
bool lilygoBringUp() {
    if (!gLcd.begin(3 /*landscape 320x170*/)) {
        Serial.println("[lilygo] panel init FAILED — running headless");
        return false;
    }
    Serial.printf("[lilygo] panel up: %dx%d %s (role %s)\n", gLcd.width(), gLcd.height(),
                  gLcd.buffered() ? "buffered" : "direct", gRole == ROLE_SLAVE ? "SLAVE" : "MASTER");
    {
        Preferences p; p.begin("vicdisp", true);
        uint8_t bl = p.getUChar("bright", 100);
        gFlip = p.getBool("flip", false);
        p.end();
        for (uint8_t i = 0; i < sizeof(kBright); ++i) if (kBright[i] == bl) gBrightIdx = i;
        gLcd.setBrightness(kBright[gBrightIdx]);
        if (gFlip) gLcd.setRotation(1);  // restore a saved 180° flip
    }
    setGraphWindowMinutes(kGraphWins[gGraphWinIdx]);
    // Allocate the three DashData buffers now that a panel is confirmed present.
    gSnap = new (std::nothrow) DashData();
    gTaskBuf = new (std::nothrow) DashData();
    gRenderBuf = new (std::nothrow) DashData();
    if (!gSnap || !gTaskBuf || !gRenderBuf) {
        Serial.println("[lilygo] out of memory for dash buffers — running headless");
        delete gSnap; delete gTaskBuf; delete gRenderBuf;
        gSnap = gTaskBuf = gRenderBuf = nullptr;
        return false;
    }
    gSnapMux = xSemaphoreCreateMutex();
    // Priority 2 (above the Arduino loop) on core 1 so button polling preempts the
    // loop's BLE work; it sleeps 16ms/iter so it never starves the loop.
    xTaskCreatePinnedToCore(lilygoTask, "lilygo", 8192, nullptr, 2, nullptr, 1);
    gDisplayOk = true;
    return true;
}

// Loop side (registry owner): publish a fresh DashData snapshot for the task.
void lilygoRender() {
    if (!gSnapMux || !gRenderBuf) return;
    DashData& tmp = *gRenderBuf;
    collectDashForRole(tmp);
    gFooterNotice = tmp.linkMismatch;
    if (xSemaphoreTake(gSnapMux, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(gSnap, &tmp, sizeof(DashData));
        xSemaphoreGive(gSnapMux);
        gHaveSnap = true;
    }
}

// Loop side: start any OTA clone the Diagnostics screen requested (startPush/Pull
// read flash, so they must run on the loop task, not the display task).
void lilygoService() {
    if (gOtaPushReqL) { gOtaPushReqL = false; gOta.startPush(); }
    if (gOtaPullReqL) { gOtaPullReqL = false; gOta.startPull(); }
}

#endif  // VICMON_HAS_LILYGO
