// Reusable driver for the Guition JC3248W535: AXS15231B 320x480 IPS panel on a
// QSPI bus, driven through a full-screen Arduino_Canvas framebuffer in PSRAM.
//
// The AXS15231B QSPI path in Arduino_GFX has no partial-window DMA, so all
// drawing goes into the canvas and the whole frame is pushed with flush(). This
// wrapper owns that canvas and the backlight; the app (or an LVGL glue layer)
// draws into canvas() and calls flush() to present.
//
// Hardware verified on real hardware 2026-07-07 (PSRAM 8MB, panel + touch OK).
// Pins (do not change without re-verifying against the board):
//   QSPI  CS=45 SCK=47 D0=21 D1=48 D2=40 D3=39  RST=-1  BL=1
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace guition {

// Display QSPI pins.
static constexpr int8_t  LCD_CS  = 45;
static constexpr int8_t  LCD_SCK = 47;
static constexpr int8_t  LCD_D0  = 21;
static constexpr int8_t  LCD_D1  = 48;
static constexpr int8_t  LCD_D2  = 40;
static constexpr int8_t  LCD_D3  = 39;
static constexpr int8_t  LCD_RST = -1;
static constexpr int8_t  LCD_BL  = 1;

// Physical panel geometry (portrait, native).
static constexpr int16_t PANEL_W = 320;
static constexpr int16_t PANEL_H = 480;

class Display {
 public:
  // rotation: 0/2 = portrait 320x480, 1/3 = landscape 480x320 (see writePixel
  // rotation cases in Arduino_Canvas). Returns false if PSRAM is missing or the
  // panel fails to init.
  bool begin(uint8_t rotation = 1);

  Arduino_Canvas* canvas() { return canvas_; }

  // Present the framebuffer to the panel (pushes the whole frame).
  void flush() { if (canvas_) canvas_->flush(); }

  // Change orientation at runtime (e.g. flip 180°: landscape 1 <-> 3). Re-applies
  // to the canvas and clears it; the next flush() presents the new orientation.
  // Only same-parity swaps (1<->3, 0<->2) keep the logical dimensions.
  void setRotation(uint8_t rotation);

  // Logical dimensions after rotation.
  int16_t width()  const { return canvas_ ? canvas_->width()  : PANEL_W; }
  int16_t height() const { return canvas_ ? canvas_->height() : PANEL_H; }
  uint8_t rotation() const { return rotation_; }

  void backlight(bool on) { setBrightness(on ? 100 : 0); }

  // Backlight brightness 0..100 % via LEDC PWM on LCD_BL. Clamped.
  void setBrightness(uint8_t pct);
  uint8_t brightness() const { return brightness_; }

 private:
  Arduino_DataBus* bus_    = nullptr;
  Arduino_GFX*     panel_  = nullptr;
  Arduino_Canvas*  canvas_ = nullptr;
  uint8_t          rotation_ = 1;
  uint8_t          brightness_ = 100;
};

}  // namespace guition
