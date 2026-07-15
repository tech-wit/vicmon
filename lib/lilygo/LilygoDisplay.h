// Reusable driver for the LilyGo T-Display-S3 (buttons): ST7789 320x170 IPS panel
// on the ESP32-S3 LCD_CAM 8-bit parallel bus, plus the two tactile buttons. The
// LilyGo counterpart to guition::Display.
//
// Direct-draw (no PSRAM canvas): the 320x170 panel is small enough to update
// in-place, so we skip the full framebuffer — that keeps it working on any S3
// (PSRAM or not) and avoids the 108KB alloc. The app draws through gfx() and owns
// its own partial redraws.
//
// Hardware-verified 2026-07-15 (src/lilygoref bring-up). Pins (do NOT change
// without re-checking the board):
//   POWER_ON=15 (HIGH to power panel+rail)   BL=38 (LEDC PWM backlight)
//   8-bit bus: DC=7 CS=6 WR=8 RD=9   D0..D7=39,40,41,42,45,46,47,48   RST=5
//   Buttons: BOOT=0 (left / A), KEY=14 (right / B) — active LOW, internal pull-ups.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace lilygo {

static constexpr int8_t PIN_POWER_ON = 15;  // panel + rail enable
static constexpr int8_t PIN_BL   = 38;       // backlight (LEDC PWM)
static constexpr int8_t PIN_DC   = 7;
static constexpr int8_t PIN_CS   = 6;
static constexpr int8_t PIN_WR   = 8;
static constexpr int8_t PIN_RD   = 9;
static constexpr int8_t PIN_RST  = 5;
static constexpr int8_t PIN_D0 = 39, PIN_D1 = 40, PIN_D2 = 41, PIN_D3 = 42;
static constexpr int8_t PIN_D4 = 45, PIN_D5 = 46, PIN_D6 = 47, PIN_D7 = 48;
static constexpr int8_t PIN_BTN_A = 0;   // BOOT / left
static constexpr int8_t PIN_BTN_B = 14;  // KEY / right

// Native 170x320 portrait with a 35px column offset; rotation 1/3 => 320x170
// landscape (3 = USB on the left, verified upright).
static constexpr int16_t PANEL_W = 170;
static constexpr int16_t PANEL_H = 320;

class Display {
 public:
  // rotation 1/3 = landscape 320x170. Returns false if the panel fails to init.
  // When PSRAM is present a full-screen Arduino_Canvas is used so the app can
  // redraw whole frames flicker-free (draw into gfx(), then flush()); without
  // PSRAM it falls back to drawing straight to the panel (flush() is a no-op).
  bool begin(uint8_t rotation = 3);

  // Draw target: the PSRAM canvas if buffered, else the panel itself.
  Arduino_GFX* gfx() { return gfx_; }
  void flush() { if (canvas_) canvas_->flush(); }
  bool buffered() const { return canvas_ != nullptr; }
  int16_t width()  const { return gfx_ ? gfx_->width()  : 0; }
  int16_t height() const { return gfx_ ? gfx_->height() : 0; }

  // Change orientation at runtime (landscape 1 <-> 3 flips 180°). Keeps the same
  // 320x170 logical size; the next full redraw presents it.
  void setRotation(uint8_t r) { rotation_ = r; if (gfx_) gfx_->setRotation(r); }
  uint8_t rotation() const { return rotation_; }

  // Backlight 0..100 % via LEDC PWM. setBrightness(0) blanks without forgetting
  // the level; restore with setBrightness(saved).
  void setBrightness(uint8_t pct);
  uint8_t brightness() const { return brightness_; }

  // Read the two buttons (true = pressed / held down, active-LOW debounced by
  // the caller's poll cadence).
  bool buttonA() const { return digitalRead(PIN_BTN_A) == LOW; }
  bool buttonB() const { return digitalRead(PIN_BTN_B) == LOW; }

 private:
  Arduino_DataBus* bus_    = nullptr;
  Arduino_GFX*     panel_  = nullptr;  // the ST7789 itself
  Arduino_Canvas*  canvas_ = nullptr;  // PSRAM framebuffer over panel_ (null if none)
  Arduino_GFX*     gfx_    = nullptr;  // == canvas_ if buffered, else panel_
  uint8_t          brightness_ = 100;
  uint8_t          rotation_   = 3;
};

}  // namespace lilygo
