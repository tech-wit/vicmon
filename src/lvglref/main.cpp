// LVGL v9 on the real Guition JC3248W535 panel.
//
// This is the stock Arduino_GFX "LVGL_Arduino_v9" example, re-pointed from its
// dummy ILI9341 onto lib/guition's known-good AXS15231B canvas + touch. Standalone
// (no BLE/WiFi) so we can see plainly whether LVGL renders cleanly on this panel's
// full-frame PSRAM canvas — the thing that folded during the earlier bring-up.
//
// Render path: LVGL draws into a small partial buffer; my_disp_flush() blits each
// area into the canvas with the rotation-aware draw16bitRGBBitmap(); loop() pushes
// the whole canvas to the panel with flush(). It runs lv_demo_widgets() so real
// LVGL widgets (tabs, charts, sliders, meters) exercise the panel.

#include <Arduino.h>
#include <lvgl.h>

#include <GuitionDisplay.h>
#include <GuitionTouch.h>

static guition::Display gDisplay;
static guition::Touch   gTouch;

static uint32_t screenWidth;
static uint32_t screenHeight;
static lv_color_t* disp_draw_buf;

static uint32_t millis_cb(void) { return millis(); }

// LVGL -> canvas. draw16bitRGBBitmap honours the canvas rotation, so no manual
// coordinate juggling. LVGL 9 renders RGB565 little-endian; the QSPI panel wants
// it byte-swapped, so swap in place before blitting.
static void my_disp_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(px_map, w * h);
  gDisplay.canvas()->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
  lv_display_flush_ready(disp);
}

static void my_touchpad_read(lv_indev_t*, lv_indev_data_t* data) {
  guition::TouchPoint tp;
  if (gTouch.read(tp)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = tp.x;
    data->point.y = tp.y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---- Hand-built LVGL showcase -----------------------------------------------
// A tabview with three tabs demonstrating the things LVGL gives us for free:
// styled widgets, fly-out animations, an auto-scrolling chart, and live-bound
// controls — all touch-driven through the indev above.

// Font-crispness test: every label sits on a FLAT solid fill (no theme gradient),
// so anti-aliased glyph edges blend against one constant color. If text is crisp
// here but grainy in the widget demo, the grain was the background, not the font.
static lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* font, const char* txt,
                          lv_align_t align, int dx, int dy) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, txt);
  lv_obj_align(l, align, dx, dy);
  return l;
}

static void build_ui() {
  lv_obj_t* scr = lv_screen_active();
  // Flat, solid background — kill the default theme's gradient entirely.
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1220), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Anti-aliased Montserrat (4bpp) — smooth shapes, but AA edges fringe on 565.
  mk_label(scr, &lv_font_montserrat_28, "Montserrat 28 (AA)  13.4V", LV_ALIGN_TOP_MID, 0, 12);
  mk_label(scr, &lv_font_montserrat_20, "Montserrat 20 (AA)  9.7A charging", LV_ALIGN_TOP_MID, 0, 52);

  // 1-bit unscii (no AA) — every pixel on/off, crisp like the GFX FreeSans fonts.
  mk_label(scr, &lv_font_unscii_16, "unscii 16 (1-bit)  13.4V  9.7A", LV_ALIGN_TOP_MID, 0, 96);
  mk_label(scr, &lv_font_unscii_16, "NO ANTIALIAS 0123456789", LV_ALIGN_TOP_MID, 0, 124);

  // Same comparison on a solid card.
  lv_obj_t* card = lv_obj_create(scr);
  lv_obj_set_size(card, 320, 96);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x182438), 0);
  lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  mk_label(card, &lv_font_montserrat_28, "AA 126W", LV_ALIGN_LEFT_MID, 8, 0);
  mk_label(card, &lv_font_unscii_16, "1bit 126W", LV_ALIGN_RIGHT_MID, -8, 0);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== LVGL v9 on Guition JC3248W535 ===");
  Serial.printf("LVGL v%d.%d.%d\n", lv_version_major(), lv_version_minor(), lv_version_patch());

  if (!gDisplay.begin(1 /*landscape 480x320*/)) {
    Serial.println("FATAL: display.begin() failed");
    while (true) delay(1000);
  }
  Serial.printf("Display OK: %dx%d\n", gDisplay.width(), gDisplay.height());
  gTouch.begin(1);

  lv_init();
  lv_tick_set_cb(millis_cb);

  screenWidth  = gDisplay.width();
  screenHeight = gDisplay.height();

  // Partial draw buffer: 1/10th of the frame, in internal RAM for speed.
  uint32_t bufPixels = screenWidth * 40;
  disp_draw_buf = (lv_color_t*)heap_caps_malloc(bufPixels * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!disp_draw_buf) disp_draw_buf = (lv_color_t*)heap_caps_malloc(bufPixels * 2, MALLOC_CAP_8BIT);
  if (!disp_draw_buf) { Serial.println("FATAL: draw buf alloc failed"); while (true) delay(1000); }

  lv_display_t* disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, disp_draw_buf, NULL, bufPixels * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  build_ui();  // hand-built LVGL showcase (tabs, chart, slider, arc, button)

  Serial.println("Setup done");
}

void loop() {
  lv_task_handler();
  gDisplay.flush();  // push the canvas to the panel
  delay(5);
}
