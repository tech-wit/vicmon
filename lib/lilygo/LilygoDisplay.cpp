#include "LilygoDisplay.h"

namespace lilygo {

// LEDC (PWM) backlight on PIN_BL. Channel 0 is free here (the Guition driver uses
// channel 7; the two boards never run their backlight in the same image at once,
// but keep them distinct to be safe).
static constexpr uint8_t  kBlChannel = 0;
static constexpr uint32_t kBlFreq    = 5000;  // Hz
static constexpr uint8_t  kBlResBits = 8;     // 0..255 duty

void Display::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  if (pct > 0) brightness_ = pct;  // remember a non-zero level across a blank
  ledcWrite(kBlChannel, (uint32_t)pct * 255 / 100);
}

bool Display::begin(uint8_t rotation) {
  rotation_ = rotation;
  // The panel + its rail are gated behind POWER_ON — nothing lights without it.
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  ledcSetup(kBlChannel, kBlFreq, kBlResBits);
  ledcAttachPin(PIN_BL, kBlChannel);
  ledcWrite(kBlChannel, 0);  // stay dark until the first frame is drawn (no flash)

  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);

  bus_ = new Arduino_ESP32LCD8(PIN_DC, PIN_CS, PIN_WR, PIN_RD,
                               PIN_D0, PIN_D1, PIN_D2, PIN_D3,
                               PIN_D4, PIN_D5, PIN_D6, PIN_D7);
  // IPS panel; 170x320 native with a 35px column offset on both parities.
  // Construct portrait-native (rotation 0); the canvas (or panel) applies the
  // landscape rotation so draw ops + flush stay consistent.
  panel_ = new Arduino_ST7789(bus_, PIN_RST, 0 /*native*/, true /*IPS*/,
                              PANEL_W, PANEL_H, 35, 0, 35, 0);

  if (psramFound()) {
    // Full-screen framebuffer in PSRAM (170*320*2 = ~106KB): the app redraws a
    // whole frame and flush() pushes it, so complex pages never flicker.
    canvas_ = new Arduino_Canvas(PANEL_W, PANEL_H, panel_);
    if (!canvas_->begin()) return false;
    canvas_->setRotation(rotation);
    gfx_ = canvas_;
  } else {
    if (!panel_->begin()) return false;
    panel_->setRotation(rotation);
    gfx_ = panel_;
  }
  gfx_->fillScreen(BLACK);
  if (canvas_) canvas_->flush();
  return true;
}

}  // namespace lilygo
