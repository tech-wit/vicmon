// Settings page: profiles/tunables/bindings/diagnostics (split from GfxDashboard.cpp, P4).
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace guition {

// --------------------------------------------------------- settings page ----
// Layout constants shared by renderSettings() and settingsHit().
static constexpr int PROF_X = 8,   PROF_Y = 66, PROF_W = 200, PROF_RH = 24;  // profile rows
static constexpr int TUN_X = 216,  TUN_Y = 68, TUN_W = W - 8 - 216, TUN_RH = 18;  // tunable rows (9 fit above -/+)
static constexpr int ADJ_Y = 232, ADJ_H = 40;                               // -/+ buttons
static constexpr int ADJ_W = (TUN_W - 8) / 2;
static constexpr int PAIR_X = 14, PAIR_Y = 240, PAIR_W = 188, PAIR_H = 30;   // Pair action (Tune)
// Tunables a slave shows on Tune (display-relevant only; the rest are master alert
// settings). render + hit-test share this ordering.
static const uint8_t kSlaveTun[3] = {TUN_BRIGHT, TUN_TZ, TUN_FLIP};

struct TunInfo { const char* label; const char* unit; };
static const TunInfo kTun[TUNABLE_N] = {
  {"Brightness", "%"}, {"Battery cap", "Ah"}, {"Deadband", "A"}, {"Timezone", "h"},
  {"SoC warn", "%"},   {"SoC crit", "%"},     {"Volt low", "V"}, {"Volt high", "V"},
  {"Screen flip", ""},
};

static float tunValue(const DashData& d, int i) {
  switch (i) {
    case TUN_BRIGHT:   return d.brightness;
    case TUN_BATTCAP:  return d.battCapAh;
    case TUN_DEADBAND: return d.deadbandA;
    case TUN_TZ:       return d.tzMin / 60.0f;
    case TUN_SOCWARN:  return d.socWarn;
    case TUN_SOCCRIT:  return d.socCrit;
    case TUN_VLOW:     return d.vLow;
    case TUN_VHIGH:    return d.vHigh;
    case TUN_FLIP:     return d.displayFlip ? 1 : 0;
  }
  return 0;
}

static void tunText(char* buf, size_t n, int i, float v) {
  switch (i) {
    case TUN_BRIGHT:  snprintf(buf, n, "%.0f%%", v); break;
    case TUN_BATTCAP: (v <= 0) ? snprintf(buf, n, "auto") : snprintf(buf, n, "%.0f Ah", v); break;
    case TUN_DEADBAND:snprintf(buf, n, "%.2f A", v); break;
    case TUN_TZ:      snprintf(buf, n, "%+.1f h", v); break;
    case TUN_SOCWARN:
    case TUN_SOCCRIT: snprintf(buf, n, "%.0f%%", v); break;
    case TUN_FLIP:    snprintf(buf, n, v > 0.5f ? "Flipped" : "Normal"); break;
    default:          snprintf(buf, n, "%.1f V", v); break;
  }
}

static void button(Arduino_GFX* c, int x, int y, int w, int h, const char* label,
                   uint16_t accent) {
  c->fillRoundRect(x, y, w, h, 8, kGrey);
  c->drawRoundRect(x, y, w, h, 8, accent);
  gtext(c, &FreeSansBold18pt7b, x + w / 2, y + h / 2 + 8, label, kText, C);
}

// --- Settings sub-view toggle (Tune | Bind) + signal-bindings editor --------
// The Bindings view mirrors the web app's "Panel signals" section: each logical
// role shows its current source; tapping a row asks the firmware to cycle it to
// the next available device+field (the firmware owns the option list + NVS).
static constexpr int SV_W = 74, SV_H = 34, SV_Y = 6;
static constexpr int SV_DIAG_X = W - 8 - SV_W;
static constexpr int SV_BIND_X = SV_DIAG_X - 6 - SV_W;
static constexpr int SV_TUNE_X = SV_BIND_X - 6 - SV_W;

// Short role labels (order matches sig::Role / the firmware's bindLabel[]).
static const char* kRoleNames[ROLE_N] = {
  "Battery SoC", "Battery V",  "Battery A",   "Consumed Ah", "Starter V",   "Time to go",
  "Solar A",     "Solar W",    "Charger A",   "DC-DC in A",  "DC-DC out A", "Load A",
};

// Big, touch-friendly rows; the list + picker paginate BIND_PERPAGE per page.
static constexpr int BIND_TOP = 44, BIND_ROWH = 38;

// Page nav = up/down arrow buttons in a reserved column down the right edge
// (up = previous page, down = next). Rows shrink to `rowRight()` to clear it.
static constexpr int NAVCOL_W = 46;
static constexpr int NAVCOL_X = W - 8 - NAVCOL_W;
static constexpr int NAV_UP_Y = 44, NAV_UP_H = 112;
static constexpr int NAV_DN_Y = 164, NAV_DN_H = 112;

static void renderViewToggle(Arduino_GFX* c, int view, int role) {
  const char* names[3] = {"Tune", "Bind", "Diag"};
  int xs[3] = {SV_TUNE_X, SV_BIND_X, SV_DIAG_X};
  for (int i = 0; i < 3; ++i) {
    if (role == 1 && i == 1) continue;  // slave has no Bind — hide the pill
    bool active = (i == view);
    c->fillRoundRect(xs[i], SV_Y, SV_W, SV_H, 7, active ? kBlue : kGrey);
    gtext(c, &FreeSansBold12pt7b, xs[i] + SV_W / 2, SV_Y + 23, names[i], active ? kBg : kText, C);
  }
}

// Right edge of the list/picker rows: full width, or shy of the nav column when
// the content paginates.
static int rowRight(int total) { return total > 1 ? NAVCOL_X - 6 : W - 6; }

// Up/down page buttons + a small "cur/total" label (drawn only when >1 page).
static void renderNav(Arduino_GFX* c, int cur, int total) {
  if (total <= 1) return;
  bool hasUp = cur > 0, hasDn = cur < total - 1;
  int cx = NAVCOL_X + NAVCOL_W / 2;
  c->fillRoundRect(NAVCOL_X, NAV_UP_Y, NAVCOL_W, NAV_UP_H, 8, hasUp ? kGrey : kCard);
  uint16_t uc = hasUp ? kText : kMuted;
  int uy = NAV_UP_Y + NAV_UP_H / 2;
  c->fillTriangle(cx, uy - 13, cx - 15, uy + 9, cx + 15, uy + 9, uc);
  c->fillRoundRect(NAVCOL_X, NAV_DN_Y, NAVCOL_W, NAV_DN_H, 8, hasDn ? kGrey : kCard);
  uint16_t dc = hasDn ? kText : kMuted;
  int dy = NAV_DN_Y + NAV_DN_H / 2;
  c->fillTriangle(cx, dy + 13, cx - 15, dy - 9, cx + 15, dy - 9, dc);
  char b[8];
  snprintf(b, sizeof(b), "%d/%d", cur + 1, total);
  gtext(c, &FreeSans9pt7b, cx, NAV_UP_Y + NAV_UP_H + 12, b, kMuted, C);
}

static int navHit(int x, int y) {
  if (x < NAVCOL_X || x >= NAVCOL_X + NAVCOL_W) return -1;
  if (y >= NAV_UP_Y && y < NAV_UP_Y + NAV_UP_H) return 0;  // up = prev page
  if (y >= NAV_DN_Y && y < NAV_DN_Y + NAV_DN_H) return 1;  // down = next page
  return -1;
}

int bindListPages() { return (ROLE_N + BIND_PERPAGE - 1) / BIND_PERPAGE; }

static void renderBindings(Arduino_GFX* c, const DashData& d) {
  int total = bindListPages();
  renderNav(c, d.bindPage, total);
  int rowR = rowRight(total);
  int base = d.bindPage * BIND_PERPAGE;
  for (int s = 0; s < BIND_PERPAGE; ++s) {
    int role = base + s;
    if (role >= ROLE_N) break;
    int y = BIND_TOP + s * BIND_ROWH;
    c->fillRoundRect(6, y, rowR - 6, BIND_ROWH - 6, 6, kCard);
    gtext(c, &FreeSansBold12pt7b, 16, y + 25, kRoleNames[role], kText, L);
    const char* v = d.bindLabel[role];
    bool none = (v[0] == '\0' || strncmp(v, "-- none", 7) == 0);
    gtext(c, &FreeSans9pt7b, rowR - 10, y + 25, none ? "-- none --" : v, none ? kMuted : kBlue, R);
  }
}

int settingsViewHit(int x, int y) {
  if (y < SV_Y || y >= SV_Y + SV_H) return -1;
  if (x >= SV_TUNE_X && x < SV_TUNE_X + SV_W) return 0;
  if (x >= SV_BIND_X && x < SV_BIND_X + SV_W) return 1;
  if (x >= SV_DIAG_X && x < SV_DIAG_X + SV_W) return 2;
  return -1;
}

int bindingHit(int x, int y) {
  int nv = navHit(x, y);
  if (nv == 0) return -2;  // prev page
  if (nv == 1) return -3;  // next page
  if (x < 6 || x >= W - 6 || y < BIND_TOP) return -1;
  int s = (y - BIND_TOP) / BIND_ROWH;
  return (s >= 0 && s < BIND_PERPAGE) ? s : -1;
}

// --- Source-picker menu (opened by tapping a signal in the Bindings list) ----
// The two "derived" sources (shared indices 1 and 2) only apply to the current-
// flow roles, mirroring the web dropdown.
static bool isCurrentRole(int r) {
  return r == 6 || r == 8 || r == 9 || r == 10 || r == 11;  // Solar/Charger/DcDcIn/DcDcOut/Load A
}

int bindVisible(int role, int srcCount, int* out, int max) {
  int n = 0;
  auto add = [&](int s) { if (n < max) out[n++] = s; };
  add(0);                                   // -- none --
  if (isCurrentRole(role)) { add(1); add(2); }  // derived charge / load
  for (int s = 3; s < srcCount; ++s) add(s);    // device + field sources
  return n;
}

static void renderBindMenu(Arduino_GFX* c, const DashData& d) {
  int role = d.menuRole;
  gtext(c, &FreeSansBold18pt7b, 12, 28, kRoleNames[role], kText);
  // Back pill (top-right, where the view toggle sits).
  c->fillRoundRect(SV_BIND_X, SV_Y, SV_W, SV_H, 7, kGrey);
  gtext(c, &FreeSansBold12pt7b, SV_BIND_X + SV_W / 2, SV_Y + 23, "Back", kText, C);

  int vis[BIND_MAXSRC];
  int vc = bindVisible(role, d.srcCount, vis, BIND_MAXSRC);
  int pages = (vc + BIND_PERPAGE - 1) / BIND_PERPAGE;
  if (pages < 1) pages = 1;
  int cur = d.menuPage < pages ? d.menuPage : pages - 1;
  renderNav(c, cur, pages);

  int rowR = rowRight(pages);
  int curShared = (role >= 0 && role < ROLE_N) ? d.bindIdx[role] : -1;
  int base = cur * BIND_PERPAGE;
  for (int s = 0; s < BIND_PERPAGE; ++s) {
    int idx = base + s;
    if (idx >= vc) break;
    int shared = vis[idx];
    int y = BIND_TOP + s * BIND_ROWH;
    bool sel = (shared == curShared);
    c->fillRoundRect(6, y, rowR - 6, BIND_ROWH - 6, 6, sel ? kBlue : kCard);
    gtext(c, &FreeSansBold12pt7b, 16, y + 25, d.srcLabels[shared], sel ? kBg : kText, L);
  }
}

int bindMenuHit(int x, int y) {
  if (x >= SV_BIND_X && x < SV_BIND_X + SV_W && y >= SV_Y && y < SV_Y + SV_H) return -2;  // Back
  int nv = navHit(x, y);
  if (nv == 0) return -3;  // prev page
  if (nv == 1) return -4;  // next page
  if (x < 6 || x >= W - 6 || y < BIND_TOP) return -1;
  int s = (y - BIND_TOP) / BIND_ROWH;
  return (s >= 0 && s < BIND_PERPAGE) ? s : -1;
}

// --- Settings > Diagnostics sub-view (ESP-NOW / pairing + devices + role) ----
// The Diag sub-view is a small menu (screen 0) that opens one of several screens.
// Menu rows, a shared bottom Back button, and an in-screen control button.
static constexpr int DM_TOP = 52, DM_RH = 42, DM_GAP = 8, DM_X = 8, DM_W = W - 16;
static constexpr int DBACK_H = 34, DBACK_Y = TAB_Y - DBACK_H - 6, DBACK_X = 8, DBACK_W = W - 16;
static constexpr int DCTL_Y = 52, DCTL_H = 42, DCTL_X = 8, DCTL_W = W - 16;
static constexpr int DROLE_BTN_Y = 116, DROLE_BTN_H = 46;
static constexpr int DLINK_UNPAIR_Y = 196;  // slave Link screen: Unpair button
// Firmware screen: two side-by-side action buttons.
static constexpr int DFW_BTN_Y = 64, DFW_BTN_H = 46, DFW_GAP = 10;
static constexpr int DFW_BTN_W = (DCTL_W - DFW_GAP) / 2;
static constexpr int DFW_R_X = DCTL_X + DFW_BTN_W + DFW_GAP;  // right button x

static void diagBtn(Arduino_GFX* c, int x, int y, int w, int h, const char* label,
                    uint16_t bg, uint16_t fg) {
  c->fillRoundRect(x, y, w, h, 7, bg);
  gtext(c, &FreeSansBold12pt7b, x + w / 2, y + h / 2 + 7, label, fg, C);
}

// Menu structure — role-dependent. Master: Monitored/Discovered/Debug/Role.
// Slave (no BLE): Link/Role. render + hit-test share this ordering.
// Master: Monitored/Discovered/Debug/Firmware/Role/Restart.
// Slave (no BLE):  Link/Firmware/Role/Restart.
// Restart is a normal row rather than a pinned bottom button — as a button it sat
// on top of the 5th row and, because its hit-test ran first, ate every tap meant
// for "Switch to Slave".
static int diagMenuCount(int role) { return role == 1 ? 4 : 6; }
int diagMenuPages(int role) {
  return (diagMenuCount(role) + DIAG_PERPAGE - 1) / DIAG_PERPAGE;
}
static DiagAction diagMenuAction(int role, int i) {
  if (role == 1) {
    switch (i) {
      case 0:  return DIAG_OPEN_LINK;
      case 1:  return DIAG_OPEN_FW;
      case 2:  return DIAG_OPEN_ROLE;
      default: return DIAG_RESTART;
    }
  }
  switch (i) {
    case 0:  return DIAG_OPEN_MON;
    case 1:  return DIAG_OPEN_DISC;
    case 2:  return DIAG_OPEN_DEBUG;
    case 3:  return DIAG_OPEN_FW;
    case 4:  return DIAG_OPEN_ROLE;
    default: return DIAG_RESTART;
  }
}
static void diagMenuLabel(const DashData& d, int i, char* out, size_t n) {
  if (d.role == 1) {
    switch (i) {
      case 0:  snprintf(out, n, "Link status"); break;
      case 1:  snprintf(out, n, "Firmware"); break;
      case 2:  snprintf(out, n, "Switch to Master"); break;
      default: snprintf(out, n, "Restart device"); break;
    }
    return;
  }
  switch (i) {
    case 0:  snprintf(out, n, "Monitored (%d)", d.monCount); break;
    case 1:  snprintf(out, n, "Discovered (%d)", d.discCount); break;
    case 2:  snprintf(out, n, d.debugCapture ? "Debug capture: ON" : "Debug capture: OFF"); break;
    case 3:  snprintf(out, n, "Firmware"); break;
    case 4:  snprintf(out, n, "Switch to Slave"); break;
    default: snprintf(out, n, "Restart device"); break;
  }
}

static void diagBack(Arduino_GFX* c) {
  diagBtn(c, DBACK_X, DBACK_Y, DBACK_W, DBACK_H, "< Back", kGrey, kText);
}

static void renderDiagMenu(Arduino_GFX* c, const DashData& d) {
  char buf[32];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Diagnostics", kText);
  int cnt = diagMenuCount(d.role);
  int total = diagMenuPages(d.role);
  renderNav(c, d.diagPage, total);          // same up/down arrows as the Bindings list
  int rowR = rowRight(total);               // rows shrink to clear the nav column
  int base = d.diagPage * DIAG_PERPAGE;
  for (int s = 0; s < DIAG_PERPAGE; ++s) {
    int i = base + s;
    if (i >= cnt) break;
    int y = DM_TOP + s * (DM_RH + DM_GAP);
    c->fillRoundRect(DM_X, y, rowR - DM_X, DM_RH, 8, kCard);
    diagMenuLabel(d, i, buf, sizeof(buf));
    gtext(c, &FreeSansBold12pt7b, DM_X + 18, y + DM_RH / 2 + 7, buf, kText, L);
    gtext(c, &FreeSansBold18pt7b, rowR - 12, y + DM_RH / 2 + 9, ">", kMuted, R);
  }
}

static void renderDiagMon(Arduino_GFX* c, const DashData& d) {
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Monitored", kText);
  int y = 46;
  for (int i = 0; i < d.monCount && i < DashData::MON_N; ++i) {
    if (y + 30 > DBACK_Y - 4) break;
    c->fillRoundRect(8, y, W - 16, 28, 6, kCard);
    c->fillCircle(23, y + 14, 5, d.monLive[i] ? kGreen : kRed);
    gtext(c, &FreeSansBold12pt7b, 38, y + 19, d.monName[i], kText);
    gtext(c, &FreeSans9pt7b, 200, y + 19, d.monType[i], kMuted);
    gtext(c, &FreeSans9pt7b, W - 14, y + 19, d.monVal[i], kText, R);
    y += 32;
  }
  if (d.monCount == 0) gtext(c, &FreeSans9pt7b, 12, 64, "No configured devices.", kMuted);
  diagBack(c);
}

static void renderDiagDisc(Arduino_GFX* c, const DashData& d) {
  char buf[48];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Discovered", kText);
  if (d.debugCapture) gtext(c, &FreeSans9pt7b, W - 12, 24, "debug on", kBlue, R);
  int y = 46;
  for (int i = 0; i < d.discCount && i < DashData::DISC_N; ++i) {
    bool raw = d.debugCapture && d.discRaw[i][0];
    int rh = raw ? 38 : 28;
    if (y + rh > DBACK_Y - 4) break;
    c->fillRoundRect(8, y, W - 16, rh, 6, kCard);
    gtext(c, &FreeSansBold12pt7b, 16, y + 19, d.discName[i][0] ? d.discName[i] : "(unnamed)", kText);
    snprintf(buf, sizeof(buf), "%s 0x%04X %ddBm", d.discMac[i], d.discModel[i], d.discRssi[i]);
    gtext(c, &FreeSans9pt7b, W - 14, y + 19, buf, kMuted, R);
    if (raw) { snprintf(buf, sizeof(buf), "raw %s", d.discRaw[i]); gtext(c, &FreeSans9pt7b, 16, y + 33, buf, kBlue); }
    y += rh + 4;
  }
  if (d.discCount == 0) gtext(c, &FreeSans9pt7b, 12, 64, "No unknown devices nearby.", kMuted);
  diagBack(c);
}

static void renderDiagDebug(Arduino_GFX* c, const DashData& d) {
  char buf[48];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Debug", kText);
  diagBtn(c, DCTL_X, DCTL_Y, DCTL_W, DCTL_H,
          d.debugCapture ? "Debug capture: ON" : "Debug capture: OFF",
          d.debugCapture ? kGreen : kGrey, d.debugCapture ? kBg : kText);
  int y = DCTL_Y + DCTL_H + 24;
  gtext(c, &FreeSans9pt7b, 12, y, "Captures raw bytes of unknown Victron adverts", kMuted); y += 20;
  gtext(c, &FreeSans9pt7b, 12, y, "(shown on the Discovered screen).", kMuted); y += 28;
  snprintf(buf, sizeof(buf), "Devices set / seen:  %d / %d", d.devPaired, d.devSeen);
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText); y += 20;
  snprintf(buf, sizeof(buf), "Free heap:  %u KB", d.freeHeapKb);
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText); y += 20;
  snprintf(buf, sizeof(buf), "Firmware:  %s", d.version[0] ? d.version : "--");
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText); y += 20;
  snprintf(buf, sizeof(buf), "Master id %08X  seq %u", d.masterId, d.snapSeq);
  gtext(c, &FreeSans9pt7b, 12, y, buf, kMuted);
  diagBack(c);
}

static void renderDiagRole(Arduino_GFX* c, const DashData& d) {
  char buf[48];
  bool slave = (d.role == 1);
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Role", kText);
  snprintf(buf, sizeof(buf), "Currently: %s", slave ? "SLAVE" : "MASTER");
  gtext(c, &FreeSansBold12pt7b, 12, 76, buf, slave ? kBlue : kGreen);
  gtext(c, &FreeSans9pt7b, 12, 100, "The device reboots into the other role.", kMuted);
  diagBtn(c, DCTL_X, DROLE_BTN_Y, DCTL_W, DROLE_BTN_H,
          slave ? "Switch to Master" : "Switch to Slave", kBlue, kBg);
  diagBack(c);
}

static void renderDiagLink(Arduino_GFX* c, const DashData& d) {  // slave role
  char buf[48];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Link", kText);
  int y = 72;
  if (!d.masterId) {
    gtext(c, &FreeSansBold12pt7b, 12, y, "Not paired", kText); y += 28;
    gtext(c, &FreeSans9pt7b, 12, y, "Pair from the Tune screen (Pair button).", kMuted); y += 22;
    gtext(c, &FreeSans9pt7b, 12, y,
          d.heardInvite ? "A master is inviting pairing now." : "Listening for a master...",
          d.heardInvite ? kGreen : kMuted);
  } else {
    snprintf(buf, sizeof(buf), "Master %08X", d.masterId);
    gtext(c, &FreeSansBold12pt7b, 12, y, buf, kText); y += 28;
    gtext(c, &FreeSans9pt7b, 12, y, d.linkLive ? "Receiving live" : "Stale / out of range",
          d.linkLive ? kGreen : kAmber); y += 22;
    snprintf(buf, sizeof(buf), "Channel %u    dropped %lu", d.linkChannel, (unsigned long)d.linkDrops);
    gtext(c, &FreeSans9pt7b, 12, y, buf, kMuted);
    diagBtn(c, DCTL_X, DLINK_UNPAIR_Y, DCTL_W, DBACK_H, "Unpair", kGrey, kText);
  }
  diagBack(c);
}

// Firmware clone (OTA) screen — push our image to the paired device, or pull a
// newer image from it. Reachable in either role.
static void renderDiagFw(Arduino_GFX* c, const DashData& d) {
  char buf[64];
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Firmware", kText);
  // Two actions, side by side.
  diagBtn(c, DCTL_X, DFW_BTN_Y, DFW_BTN_W, DFW_BTN_H, "Send to peer", kBlue, kBg);
  diagBtn(c, DFW_R_X, DFW_BTN_Y, DFW_BTN_W, DFW_BTN_H, "Get from peer", kGrey, kText);
  // Transfer status (when active).
  int y = DFW_BTN_Y + DFW_BTN_H + 30;
  if (d.otaStatus[0]) {
    if (d.otaBusy) snprintf(buf, sizeof(buf), "%s %u%%", d.otaStatus, d.otaPct);
    else           snprintf(buf, sizeof(buf), "%s", d.otaStatus);
    gtext(c, &FreeSans9pt7b, 12, y, buf, d.otaBusy ? kBlue : kMuted);
  }
  // Versions — lower on the screen, consistent non-bold text.
  y += 30;
  snprintf(buf, sizeof(buf), "This unit: %s", d.version[0] ? d.version : "--");
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText);
  y += 24;
  if (d.otaPeerKnown)
    snprintf(buf, sizeof(buf), "Paired: %s (%s)", d.otaPeerVer[0] ? d.otaPeerVer : "?", d.otaPeerRel);
  else
    snprintf(buf, sizeof(buf), "Paired: not heard yet");
  gtext(c, &FreeSans9pt7b, 12, y, buf, kText);
  diagBack(c);
}

static void renderDiag(Arduino_GFX* c, const DashData& d) {
  switch (d.diagScreen) {
    case DS_MON:   renderDiagMon(c, d); break;
    case DS_DISC:  renderDiagDisc(c, d); break;
    case DS_DEBUG: renderDiagDebug(c, d); break;
    case DS_ROLE:  renderDiagRole(c, d); break;
    case DS_LINK:  renderDiagLink(c, d); break;
    case DS_FW:    renderDiagFw(c, d); break;
    default:       renderDiagMenu(c, d); break;
  }
}

int diagHit(int x, int y, int role, int screen, int page) {
  if (screen == DS_MENU) {
    int total = diagMenuPages(role);
    int nv = navHit(x, y);
    if (nv == 0) return DIAG_MENU_PREV;
    if (nv == 1) return DIAG_MENU_NEXT;
    int rowR = rowRight(total);
    if (x < DM_X || x >= rowR) return DIAG_NONE;
    for (int s = 0; s < DIAG_PERPAGE; ++s) {
      int ry = DM_TOP + s * (DM_RH + DM_GAP);
      if (y >= ry && y < ry + DM_RH) {
        int i = page * DIAG_PERPAGE + s;
        return (i < diagMenuCount(role)) ? diagMenuAction(role, i) : DIAG_NONE;
      }
    }
    return DIAG_NONE;
  }
  if (y >= DBACK_Y && y < DBACK_Y + DBACK_H && x >= DBACK_X && x < DBACK_X + DBACK_W) return DIAG_BACK;
  if (screen == DS_DEBUG && y >= DCTL_Y && y < DCTL_Y + DCTL_H && x >= DCTL_X && x < DCTL_X + DCTL_W)
    return DIAG_DEBUG_TOGGLE;
  if (screen == DS_ROLE && y >= DROLE_BTN_Y && y < DROLE_BTN_Y + DROLE_BTN_H &&
      x >= DCTL_X && x < DCTL_X + DCTL_W)
    return DIAG_ROLE_TOGGLE;
  if (screen == DS_LINK && y >= DLINK_UNPAIR_Y && y < DLINK_UNPAIR_Y + DBACK_H &&
      x >= DCTL_X && x < DCTL_X + DCTL_W)
    return DIAG_UNPAIR;
  if (screen == DS_FW && y >= DFW_BTN_Y && y < DFW_BTN_Y + DFW_BTN_H) {
    if (x >= DCTL_X && x < DCTL_X + DFW_BTN_W) return DIAG_OTA_PUSH;
    if (x >= DFW_R_X && x < DFW_R_X + DFW_BTN_W) return DIAG_OTA_PULL;
  }
  return DIAG_NONE;
}

void renderSettings(Arduino_GFX* c, const DashData& d) {
  int view = d.setView;
  if (d.role == 1 && view == 1) view = 0;  // slave has no signal bindings -> Tune
  // Bindings view has a full-screen source picker when a signal is selected.
  if (view == 1 && d.menuRole >= 0 && d.menuRole < ROLE_N) {
    renderBindMenu(c, d);
    return;
  }
  renderViewToggle(c, view, d.role);
  if (view == 2) { renderDiag(c, d); return; }  // Diag draws its own per-screen header
  gtext(c, &FreeSansBold18pt7b, 12, 28, "Settings", kText);
  if (view == 1) { renderBindings(c, d); return; }

  char buf[48];
  bool slave = (d.role == 1);
  int ntun = slave ? 3 : TUNABLE_N;  // slave: brightness + timezone + screen flip

  // --- Left column: profiles (master) or link status (slave) + shared status ---
  c->fillRoundRect(8, 44, 200, TAB_Y - 52, 10, kCard);
  int sy;
  if (!slave) {
    gtext(c, &FreeSans9pt7b, 18, 60, "PROFILES", kMuted);
    for (int i = 0; i < 4; ++i) {
      int ry = PROF_Y + i * PROF_RH;
      if (!d.profUsed[i]) continue;
      bool active = (i == d.profileId);
      if (active) c->fillRoundRect(PROF_X + 6, ry, PROF_W - 12, PROF_RH - 2, 5, kGrey);
      c->fillCircle(PROF_X + 16, ry + (PROF_RH - 2) / 2, 4, active ? kGreen : kMuted);
      gtext(c, &FreeSans9pt7b, PROF_X + 28, ry + 15, d.profNames[i], active ? kText : kMuted);
    }
    sy = PROF_Y + 4 * PROF_RH + 12;
  } else {
    gtext(c, &FreeSans9pt7b, 18, 60, "LINK", kMuted);
    if (d.masterId) {
      snprintf(buf, sizeof(buf), "Master %08X", d.masterId);
      gtext(c, &FreeSans9pt7b, 18, 82, buf, kText);
      gtext(c, &FreeSans9pt7b, 18, 102, d.linkLive ? "receiving live" : "stale / no signal",
            d.linkLive ? kGreen : kAmber);
      snprintf(buf, sizeof(buf), "ch %u   drops %lu", d.linkChannel, (unsigned long)d.linkDrops);
      gtext(c, &FreeSans9pt7b, 18, 122, buf, kMuted);
    } else {
      gtext(c, &FreeSans9pt7b, 18, 82, "Not paired", kText);
      gtext(c, &FreeSans9pt7b, 18, 102, "tap Pair below", kMuted);
    }
    sy = 148;
  }
  // Shared status lines.
  const int dy = 15;
  auto stat = [&](const char* label, const char* val) {
    gtext(c, &FreeSans9pt7b, 18, sy, label, kMuted);
    gtext(c, &FreeSans9pt7b, 200, sy, val, kText, R);
    sy += dy;
  };
  stat("AP", d.apSsid);
  stat("Pass", d.apPass);
  stat("IP", d.ipStr);
  uint32_t up = d.uptimeSec;
  if (up >= 86400) snprintf(buf, sizeof(buf), "%ud %uh", up / 86400, (up % 86400) / 3600);
  else if (up >= 3600) snprintf(buf, sizeof(buf), "%uh %um", up / 3600, (up % 3600) / 60);
  else snprintf(buf, sizeof(buf), "%um", up / 60);
  stat("Uptime", buf);  // dev/heap/firmware moved to Diag > Debug
  // Pair action: master opens the pairing window, slave adopts.
  char pb[24];
  if (d.pairing) snprintf(pb, sizeof(pb), "%s %ds", slave ? "Adopting" : "Pairing", d.pairSecLeft);
  else           snprintf(pb, sizeof(pb), "%s", slave ? "Pair" : "Pair slave");
  diagBtn(c, PAIR_X, PAIR_Y, PAIR_W, PAIR_H, pb, d.pairing ? kAmber : kBlue, kBg);

  // --- Right column: tunables (slave shows only display-relevant ones) ---
  c->fillRoundRect(TUN_X, 44, TUN_W, TAB_Y - 52, 10, kCard);
  gtext(c, &FreeSans9pt7b, TUN_X + 12, 60, "ADJUST", kMuted);
  for (int r = 0; r < ntun; ++r) {
    int i = slave ? kSlaveTun[r] : r;
    int ry = TUN_Y + r * TUN_RH;
    bool sel = (i == d.setSel);
    if (sel) c->fillRoundRect(TUN_X + 6, ry, TUN_W - 12, TUN_RH - 2, 4, kGrey);
    gtext(c, &FreeSans9pt7b, TUN_X + 14, ry + 15, kTun[i].label, sel ? kText : kMuted);
    tunText(buf, sizeof(buf), i, tunValue(d, i));
    gtext(c, &FreeSansBold12pt7b, TUN_X + TUN_W - 14, ry + 16, buf, sel ? kBlue : kText, R);
  }
  button(c, TUN_X, ADJ_Y, ADJ_W, ADJ_H, "-", kBlue);
  button(c, TUN_X + ADJ_W + 8, ADJ_Y, ADJ_W, ADJ_H, "+", kBlue);
}

SettingsHitResult settingsHit(int x, int y, int role) {
  auto in = [&](int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
  };
  int ntun = role == 1 ? 3 : TUNABLE_N;  // slave list = brightness + timezone + screen flip
  // Pair action + adjust buttons.
  if (in(PAIR_X, PAIR_Y, PAIR_W, PAIR_H)) return {SA_PAIR, 0};
  if (in(TUN_X, ADJ_Y, ADJ_W, ADJ_H)) return {SA_ADJ_DN, 0};
  if (in(TUN_X + ADJ_W + 8, ADJ_Y, ADJ_W, ADJ_H)) return {SA_ADJ_UP, 0};
  // Tunable rows (slave list = brightness + timezone only).
  if (in(TUN_X, TUN_Y, TUN_W, ntun * TUN_RH)) {
    int r = (y - TUN_Y) / TUN_RH;
    if (r >= 0 && r < ntun) return {SA_SELECT_ROW, role == 1 ? (int)kSlaveTun[r] : r};
  }
  // Profile rows (master only).
  if (role == 0 && in(PROF_X, PROF_Y, PROF_W, 4 * PROF_RH)) {
    int row = (y - PROF_Y) / PROF_RH;
    if (row >= 0 && row < 4) return {SA_PROFILE, row};
  }
  return {SA_NONE, 0};
}

}  // namespace guition
