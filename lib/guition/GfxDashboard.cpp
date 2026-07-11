// Dashboard dispatcher: the bottom tab bar + the page-descriptor registry that
// maps each Page to its renderer. The pages themselves live in gfx_dash.cpp /
// gfx_flow.cpp / gfx_graph.cpp / gfx_week.cpp / gfx_settings.cpp; the shared
// palette, layout constants and drawing primitives are in gfx_internal.h /
// gfx_common.cpp. (Split from a single ~1000-line file, P4.)
#include "gfx_internal.h"

namespace guition {

// ---------------------------------------------------------------- tab bar ----
static void renderTabs(Arduino_GFX* c, Page page) {
  static const char* names[PAGE_COUNT] = {"Dash", "Mimic", "Graph", "Week", "Settings"};
  c->fillRect(0, TAB_Y, W, TAB_H, kBg);
  c->drawFastHLine(0, TAB_Y, W, kGrey);
  for (int i = 0; i < PAGE_COUNT; ++i) {
    int x = i * TAB_W;
    bool active = (i == page);
    if (active) {
      c->fillRect(x, TAB_Y + 1, TAB_W, TAB_H - 1, kCard);
      c->fillRect(x, TAB_Y, TAB_W, 3, kBlue);
    }
    gtext(c, &FreeSans9pt7b, x + TAB_W / 2, TAB_Y + TAB_H / 2 + 6, names[i],
          active ? kText : kMuted, C);
  }
}

int tabHitTest(int tx, int ty) {
  if (ty < TAB_Y) return -1;
  int i = tx / TAB_W;
  if (i < 0) i = 0;
  if (i >= PAGE_COUNT) i = PAGE_COUNT - 1;
  return i;
}

// -------------------------------------------------------------- dispatch ----
// Page-descriptor registry: one renderer per Page, indexed by the enum. Adding a
// page is a new gfx_<page>.cpp + one row here (and a tab name above).
typedef void (*PageRenderFn)(Arduino_GFX*, const DashData&);
static const PageRenderFn kPageRender[PAGE_COUNT] = {
    renderDash,      // PAGE_DASH
    renderFlow,      // PAGE_FLOW
    renderGraph,     // PAGE_GRAPH  (slave builds history from received frames)
    renderDays,      // PAGE_DAYS   (slave fills from the stats frame)
    renderSettings,  // PAGE_SETTINGS
};

void renderPage(Arduino_GFX* c, Page page, const DashData& d) {
  c->fillScreen(kBg);
  PageRenderFn fn = (page < PAGE_COUNT) ? kPageRender[page] : nullptr;
  (fn ? fn : renderDash)(c, d);
  renderTabs(c, page);
  c->setFont(nullptr);
}

void renderDashboard(Arduino_GFX* c, const DashData& d) { renderPage(c, PAGE_DASH, d); }

}  // namespace guition
