#include "GuitionTouch.h"

#include <Wire.h>

#include "GuitionDisplay.h"  // PANEL_W / PANEL_H

namespace guition {

// Fixed AXS15231B "read touchpad" command; controller replies with 8 bytes.
static const uint8_t AXS_READ_TOUCH[8] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x08};

bool Touch::begin(uint8_t rotation) {
  rotation_ = rotation;
  Wire.begin(TP_SDA, TP_SCL);
  Wire.setClock(400000);
  pinMode(TP_INT, INPUT);
  Wire.beginTransmission(TP_ADDR);
  return Wire.endTransmission() == 0;
}

bool Touch::read(TouchPoint& p) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(AXS_READ_TOUCH, sizeof(AXS_READ_TOUCH));
  if (Wire.endTransmission() != 0) { p.pressed = false; return false; }
  delayMicroseconds(50);

  uint8_t d[8] = {0};
  Wire.requestFrom(TP_ADDR, (uint8_t)8);
  int i = 0;
  while (Wire.available() && i < 8) d[i++] = Wire.read();
  if (i < 8) { p.pressed = false; return false; }

  // Finger down when d[0]==0 && d[1]!=0. Coords are 12-bit, panel-native px.
  if (d[0] != 0 || d[1] == 0) { p.pressed = false; return false; }
  int16_t rx = ((d[2] & 0x0F) << 8) | d[3];  // 0..PANEL_W-1
  int16_t ry = ((d[4] & 0x0F) << 8) | d[5];  // 0..PANEL_H-1

  // Map panel-native coords into the logical orientation (mirrors the
  // Arduino_Canvas rotation cases).
  switch (rotation_) {
    case 1:  // landscape
      p.x = ry;
      p.y = (PANEL_W - 1) - rx;
      break;
    case 2:  // portrait inverted
      p.x = (PANEL_W - 1) - rx;
      p.y = (PANEL_H - 1) - ry;
      break;
    case 3:  // landscape inverted
      p.x = (PANEL_H - 1) - ry;
      p.y = rx;
      break;
    default:  // 0: portrait
      p.x = rx;
      p.y = ry;
  }
  p.pressed = true;
  return true;
}

}  // namespace guition
