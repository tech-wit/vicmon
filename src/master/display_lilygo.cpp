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

// ---- pages & UI state ------------------------------------------------------
enum LPage : uint8_t { LP_DASH = 0, LP_FLOW, LP_GRAPH, LP_WEEK, LP_INFO, LP_SET, LP_COUNT };
static const char* kPageName[LP_COUNT] = {"Dashboard", "Power Flow", "Graph", "Week", "Status", "Settings"};
static const uint16_t kPageColor[LP_COUNT] = {C_GREEN, C_ALT, C_SOLAR, C_LOAD, C_BLUE, RGB565(190, 195, 210)};
static volatile uint8_t gPage = LP_DASH;

// Settings page: a list cycled with B (short), the highlighted item activated with
// B (long / press-and-hold). Items act rather than edit — 2-button friendly.
enum SetItem : uint8_t { SET_BRIGHT = 0, SET_FLIP, SET_PAIR, SET_ROLE, SET_RESTART, SET_N };
static volatile uint8_t gSetSel = 0;
static volatile bool gFlip = false;      // 180° screen flip (rotation 3 normal / 1 flipped)

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

// Published snapshot (loop writes under mutex; display task reads).
static DashData gSnap;
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
static void drawFooter(const char* bHint) {
    Arduino_GFX* g = G();
    g->fillRect(0, FOOT_Y, W, H - FOOT_Y, RGB565(22, 22, 28));
    g->drawFastHLine(0, FOOT_Y, W, RGB565(55, 55, 68));
    T(6, H - 5, F_S, RGB565(205, 205, 216), kPageName[gPage]);
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

    // Right column: battery V / A + detail.
    T(174, 14, F_S, C_LBL, "BATTERY");
    if (d.battValid) {
        Tf(174, 42, F_L, C_BLUE, "%.2fV", d.v);
        Tf(174, 72, F_L, ampColor(d.a), "%+.1fA", d.a);
    } else {
        T(174, 42, F_L, C_DIM, "--V");
        T(174, 72, F_L, C_DIM, "--A");
    }
    char det[28];
    if (gDashDetail == 0) guition::ttgLabel(det, sizeof(det), d);
    else if (gDashDetail == 1) snprintf(det, sizeof(det), "Batt %.0fW", d.v * d.a);
    else if (d.starterValid) snprintf(det, sizeof(det), "Start %.1fV", d.starterV);
    else strcpy(det, "Start --");
    T(174, 92, F_S, C_LBL, det);

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

// ---- Page: Graph -----------------------------------------------------------
static void plotAmps(const int16_t* a, int n, int x0, int pw, int midY, int halfH,
                     float span, uint16_t col) {
    Arduino_GFX* g = G();
    int px = -1, py = -1;
    for (int i = 0; i < n; ++i) {
        if (a[i] == -32768) { px = -1; continue; }
        int x = x0 + (n <= 1 ? 0 : i * pw / (n - 1));
        int y = midY - (int)(a[i] / 10.0f / span * halfH);
        if (y < midY - halfH) y = midY - halfH;
        if (y > midY + halfH) y = midY + halfH;
        if (px >= 0) g->drawLine(px, py, x, y, col);
        else g->drawPixel(x, y, col);
        px = x; py = y;
    }
}
static void pageGraph(const DashData& d) {
    Arduino_GFX* g = G();
    int x0 = 6, y0 = 4, pw = W - 12, ph = 112;
    int midY = y0 + ph / 2, halfH = ph / 2;
    g->drawRect(x0, y0, pw, ph, RGB565(45, 45, 56));
    Tf(x0 + 4, y0 + 18, F_M, kPageColor[LP_GRAPH], "%s", kGraphWinName[gGraphWinIdx]);

    if (d.histCount < 2) {
        T(x0 + 96, midY + 4, F_S, C_LBL, d.graphSyncing ? "syncing..." : "no data yet");
    } else {
        float span = 5.0f;
        const int16_t* series[5] = {d.histBatt, d.histSolar, d.histCharger, d.histDcdc, d.histLoad};
        for (int s = 0; s < 5; ++s)
            for (int i = 0; i < d.histCount; ++i)
                if (series[s][i] != -32768) { float m = fabsf(series[s][i] / 10.0f); if (m > span) span = m; }
        g->drawFastHLine(x0, midY, pw, RGB565(60, 60, 74));
        const uint16_t col[5] = {C_GREEN, C_SOLAR, C_CHG, C_ALT, C_LOAD};
        for (int s = 0; s < 5; ++s) plotAmps(series[s], d.histCount, x0, pw, midY, halfH, span, col[s]);
        if (!gGraphHideSoc) {
            int px = -1, py = -1;
            for (int i = 0; i < d.histCount; ++i) {
                if (d.histSoc[i] == -32768) { px = -1; continue; }
                int x = x0 + (d.histCount <= 1 ? 0 : i * pw / (d.histCount - 1));
                int y = y0 + ph - (int)(d.histSoc[i] / 1000.0f * ph);
                if (px >= 0) g->drawLine(px, py, x, y, C_SOC);
                px = x; py = y;
            }
        }
        TR(x0 + pw - 4, y0 + 18, F_M, C_TEXT, "%.0fA", span);
    }
    // Legend.
    struct Leg { const char* n; uint16_t c; bool on; };
    Leg leg[6] = {{"Bat", C_GREEN, true}, {"Sol", C_SOLAR, true}, {"Chg", C_CHG, true},
                  {"Alt", C_ALT, true}, {"Load", C_LOAD, true}, {"SoC", C_SOC, !gGraphHideSoc}};
    int lx = 6;
    for (int i = 0; i < 6; ++i) {
        g->fillRect(lx, 128, 9, 9, leg[i].on ? leg[i].c : C_DIM);
        T(lx + 12, 138, F_S, leg[i].on ? C_TEXT : C_DIM, leg[i].n);
        lx += 12 + textW(F_S, leg[i].n) + 12;
    }
    drawFooter("B:zoom  hold:SoC");
}

// ---- Page: Week ------------------------------------------------------------
static void pageWeek(const DashData& d) {
    Arduino_GFX* g = G();
    int x0 = 6, top = 6, chartH = 66, midY = top + chartH / 2;
    g->drawFastHLine(x0, midY, W - 12, RGB565(60, 60, 74));
    float mx = 1.0f;
    for (int i = 0; i < d.dayCount; ++i) {
        float in = d.daySolarAh[i] + d.dayDcdcAh[i] + d.dayChargerAh[i];
        if (in > mx) mx = in;
        if (d.dayLoadAh[i] > mx) mx = d.dayLoadAh[i];
    }
    int slot = (W - 12) / 7, bw = slot - 8;
    for (int i = 0; i < d.dayCount; ++i) {
        int x = x0 + i * slot + 4;
        int inH = (int)((d.daySolarAh[i] + d.dayDcdcAh[i] + d.dayChargerAh[i]) / mx * (chartH / 2 - 2));
        int outH = (int)(d.dayLoadAh[i] / mx * (chartH / 2 - 2));
        if (inH > 0) g->fillRect(x, midY - inH, bw, inH, C_GREEN);
        if (outH > 0) g->fillRect(x, midY + 1, bw, outH, C_LOAD);
    }
    // Scope meter.
    const DashData::StatMeter& m = gWeekScope == 0 ? d.statToday : gWeekScope == 1 ? d.statTrip : d.statTotal;
    int sy = 92;
    g->drawFastHLine(x0, sy - 4, W - 12, RGB565(45, 45, 56));
    Tf(x0, sy + 14, F_M, kPageColor[LP_WEEK], "%s", kScopeName[gWeekScope]);
    int hh = (int)(m.durSecs / 3600), mm = (int)((m.durSecs % 3600) / 60);
    TR(W - 6, sy + 12, F_S, C_LBL, "%dh%02dm", hh, mm);
    Tf(x0, sy + 34, F_S, C_GREEN, "In %.1f", m.inAh);
    Tf(x0, sy + 50, F_S, C_LOAD, "Out %.1f", m.outAh);
    Tf(118, sy + 34, F_S, C_SOLAR, "Sol %.1f", m.solarAh);
    Tf(118, sy + 50, F_S, C_ALT, "Alt %.1f", m.dcdcAh);
    Tf(224, sy + 34, F_S, C_CHG, "Chg %.1f", m.chargerAh);
    Tf(224, sy + 50, F_S, C_LOAD, "Ld %.1f", m.loadAh);
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
    const char* names[SET_N] = {"Brightness", "Flip 180", "Pair", "Role", "Restart"};
    char val[SET_N][20];
    snprintf(val[SET_BRIGHT], 20, "%d%%", kBright[gBrightIdx]);
    snprintf(val[SET_FLIP], 20, "%s", gFlip ? "On" : "Off");
    if (d.pairing) snprintf(val[SET_PAIR], 20, "OPEN %ds", d.pairSecLeft);
    else snprintf(val[SET_PAIR], 20, "%s", gRole == ROLE_SLAVE ? "adopt" : "open");
    snprintf(val[SET_ROLE], 20, "%s", gRole == ROLE_SLAVE ? "-> Master" : "-> Slave");
    snprintf(val[SET_RESTART], 20, "reboot");
    int y0 = 8, rh = 27;
    for (int i = 0; i < SET_N; ++i) {
        int ry = y0 + i * rh;
        bool sel = (i == gSetSel);
        if (sel) {
            g->fillRoundRect(4, ry, W - 8, rh - 3, 4, RGB565(38, 42, 54));
            g->drawRoundRect(4, ry, W - 8, rh - 3, 4, kPageColor[LP_SET]);
        }
        T(12, ry + 18, F_M, sel ? C_TEXT : C_LBL, names[i]);
        TR(W - 12, ry + 18, F_M, sel ? kPageColor[LP_SET] : C_LBL, "%s", val[i]);
    }
    drawFooter("B:next  hold:select");
}

// ---- frame render (display task side) --------------------------------------
static void drawFrame(const DashData& d) {
    G()->fillScreen(C_BG);
    G()->drawFastHLine(0, 0, W, kPageColor[gPage]);
    switch (gPage) {
        case LP_DASH:  pageDash(d);  break;
        case LP_FLOW:  pageFlow(d);  break;
        case LP_GRAPH: pageGraph(d); break;
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
        case SET_ROLE:    gRoleReq = true; Serial.println("[lilygo] role toggle requested (reboot)"); break;
        case SET_RESTART: gRebootReq = true; Serial.println("[lilygo] restart requested"); break;
    }
}
static void bShort() {
    switch (gPage) {
        case LP_DASH:  gDashDetail = (gDashDetail + 1) % 3; break;
        case LP_FLOW:  gFlowWatts = !gFlowWatts; break;
        case LP_GRAPH: gGraphWinIdx = (gGraphWinIdx + 1) % 5; setGraphWindowMinutes(kGraphWins[gGraphWinIdx]); break;
        case LP_WEEK:  gWeekScope = (gWeekScope + 1) % 3; break;
        case LP_SET:   gSetSel = (gSetSel + 1) % SET_N; break;
        default:       gBrightIdx = (gBrightIdx + 1) % (uint8_t)sizeof(kBright); applyBrightness(); break;
    }
}
static void bLong() {
    if (gPage == LP_GRAPH) gGraphHideSoc = !gGraphHideSoc;
    else if (gPage == LP_INFO) openPairWindow();
    else if (gPage == LP_SET) settingsActivate();
}
static void aShort() { gPage = (gPage + 1) % LP_COUNT; }
static void aLong()  { gPage = (gPage + LP_COUNT - 1) % LP_COUNT; }

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
    static DashData local;
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
                memcpy(&local, &gSnap, sizeof(local));
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
    gSnapMux = xSemaphoreCreateMutex();
    // Priority 2 (above the Arduino loop) on core 1 so button polling preempts the
    // loop's BLE work; it sleeps 16ms/iter so it never starves the loop.
    xTaskCreatePinnedToCore(lilygoTask, "lilygo", 8192, nullptr, 2, nullptr, 1);
    gDisplayOk = true;
    return true;
}

// Loop side (registry owner): publish a fresh DashData snapshot for the task.
void lilygoRender() {
    if (!gSnapMux) return;
    static DashData tmp;
    collectDashForRole(tmp);
    if (xSemaphoreTake(gSnapMux, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(&gSnap, &tmp, sizeof(gSnap));
        xSemaphoreGive(gSnapMux);
        gHaveSnap = true;
    }
}

// Buttons are polled by the display task, so nothing to do on the loop here.
void lilygoService() {}

#endif  // VICMON_HAS_LILYGO
