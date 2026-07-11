#include "GuitionDisplay.h"

namespace guition {

// LEDC (PWM) backlight on LCD_BL.
static constexpr uint8_t  kBlChannel = 7;
static constexpr uint32_t kBlFreq    = 5000;  // Hz
static constexpr uint8_t  kBlResBits = 8;     // 0..255 duty

void Display::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  brightness_ = pct;
  ledcWrite(kBlChannel, (uint32_t)pct * 255 / 100);
}

void Display::setRotation(uint8_t rotation) {
  rotation_ = rotation;
  if (!canvas_) return;
  // Arduino_GFX::setRotation just updates the rotation used by the canvas'
  // per-pixel mapping (1<->3 keep the 480x320 framebuffer, no realloc). Clear so
  // no stale pixels from the old orientation survive until the next full redraw.
  canvas_->setRotation(rotation_);
  canvas_->fillScreen(BLACK);
}

bool Display::begin(uint8_t rotation) {
  rotation_ = rotation;

  if (!psramFound()) {
    // The canvas framebuffer (320*480*2 = 300KB) cannot fit without PSRAM.
    return false;
  }

  // Backlight via LEDC PWM so brightness is adjustable (Settings page).
  ledcSetup(kBlChannel, kBlFreq, kBlResBits);
  ledcAttachPin(LCD_BL, kBlChannel);
  setBrightness(brightness_);

  bus_ = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
  // Panel constructed portrait-native; the canvas applies the rotation (software)
  // so Arduino_GFX draw ops + flush stay consistent. Landscape (rotation 1) draws
  // cleanly via the GFX primitives — proven. We render the dashboard with those
  // primitives directly (LVGL's flush folds on this canvas).
  panel_ = new Arduino_AXS15231B(bus_, LCD_RST, 0 /*panel rotation*/, false /*IPS*/,
                                 PANEL_W, PANEL_H, 0, 0, 0, 0);
  canvas_ = new Arduino_Canvas(PANEL_W, PANEL_H, panel_);
  // Default QSPI speed (80MHz) — the reference library uses this; 40MHz caused
  // the whole frame to display doubled (below the panel's timing needs).
  if (!canvas_->begin()) {
    return false;
  }
  canvas_->setRotation(rotation_);  // 1 = landscape 480x320
  canvas_->fillScreen(BLACK);
  canvas_->flush();
  return true;
}

}  // namespace guition
