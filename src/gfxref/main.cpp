// Reference test: me-processware JC3248W535 driver's own Basic example, verbatim.
// Establishes a known-good baseline — if THIS renders cleanly, the panel + library
// work and any issue is in our code. Portrait (the library's default rotation).
#include <JC3248W535.h>

JC3248W535_Display display;
JC3248W535_Touch touch;
Arduino_Canvas* gfx;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535 reference Basic example");

  if (!display.begin()) {
    Serial.println("display.begin FAILED");
    while (1) delay(1000);
  }
  Serial.println("display OK");
  display.setRotation(ROTATION_90);  // LANDSCAPE — does the reference lib double?
  if (!touch.begin()) Serial.println("touch FAILED");
  else { Serial.println("touch OK"); display.setTouchRotation(&touch); }

  gfx = display.getCanvas();
  Serial.printf("canvas %dx%d\n", gfx->width(), gfx->height());
  gfx->fillScreen(WHITE);
  gfx->setTextColor(BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(40, 40);
  gfx->print("LANDSCAPE TEST");
  gfx->setTextSize(2);
  gfx->setCursor(40, 90);
  gfx->print("Reference lib, rotation 90");
  gfx->drawRect(40, 130, 260, 120, RED);
  gfx->fillRect(50, 140, 60, 40, GREEN);
  gfx->fillRect(130, 140, 60, 40, BLUE);
  gfx->fillRect(210, 140, 60, 40, YELLOW);
  display.flush();
  Serial.println("Ready");
}

void loop() {
  // Reference lib, landscape, DENSE content REDRAWN every 500ms — mirrors our
  // dashboard's behaviour to see if it doubles too.
  static uint32_t last = 0;
  static int n = 0;
  if (millis() - last >= 500) {
    last = millis();
    n++;
    gfx->fillScreen(RGB565(0x0b, 0x12, 0x20));  // dark bg like the dashboard
    gfx->fillRect(0, 0, 480, 44, RGB565(0x3f, 0xb9, 0x50));
    gfx->setTextColor(BLACK); gfx->setTextSize(3);
    gfx->setCursor(180, 12); gfx->print("Charging");
    // a grid of labelled rows + a couple of filled bars
    gfx->setTextSize(3); gfx->setTextColor(WHITE);
    char b[24]; snprintf(b, sizeof(b), "13.0%dV", n % 10);
    gfx->setCursor(20, 120); gfx->print(b);
    gfx->setCursor(300, 120); gfx->print("9.7A");
    gfx->fillRect(20, 180, 260, 18, GREEN);
    for (int i = 0; i < 4; i++) {
      gfx->fillRoundRect(304, 60 + i * 66, 168, 60, 6, RGB565(0x18, 0x24, 0x38));
      gfx->setTextSize(2); gfx->setTextColor(YELLOW);
      gfx->setCursor(312, 68 + i * 66); gfx->print("Tile");
    }
    display.flush();
  }
  delay(5);
}
