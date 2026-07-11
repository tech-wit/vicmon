// AXS15231B capacitive touch on the Guition JC3248W535, over I2C (polled).
//   SDA=4 SCL=8 INT=3 addr=0x3B
// Raw controller coordinates are panel-native (portrait, x:0..319 y:0..479);
// read() maps them into the display's logical orientation to match the canvas
// rotation set in guition::Display.
#pragma once

#include <Arduino.h>

namespace guition {

static constexpr int8_t  TP_SDA  = 4;
static constexpr int8_t  TP_SCL  = 8;
static constexpr int8_t  TP_INT  = 3;
static constexpr uint8_t TP_ADDR = 0x3B;

struct TouchPoint {
  int16_t x = 0;
  int16_t y = 0;
  bool pressed = false;
};

class Touch {
 public:
  // rotation must match guition::Display's rotation (0/2 portrait, 1/3 landscape).
  bool begin(uint8_t rotation = 1);

  // Change orientation at runtime; must track guition::Display's rotation so the
  // coordinate mapping stays aligned with the canvas (e.g. after a 180° flip).
  void setRotation(uint8_t rotation) { rotation_ = rotation; }

  // Poll the controller. Returns true and fills `p` (mapped to logical coords)
  // when a finger is down; returns false otherwise.
  bool read(TouchPoint& p);

 private:
  uint8_t rotation_ = 1;
};

}  // namespace guition
