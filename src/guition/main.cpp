// Guition JC3248W535 — standalone dashboard demo (no WiFi, no LVGL).
// Renders the Arduino_GFX dashboard with animated synthetic data so the display
// can be exercised on the bench. The same lib/guition modules run in the master
// firmware. Touch is polled and printed to serial.

#include <Arduino.h>

#include <GuitionDisplay.h>
#include <GuitionTouch.h>
#include <GfxDashboard.h>

static guition::Display display;
static guition::Touch   touch;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Guition dashboard demo (Arduino_GFX) ===");

  if (!display.begin(1 /*landscape 480x320*/)) {
    Serial.println("FATAL: display.begin() failed");
    while (true) delay(1000);
  }
  Serial.printf("Display OK: %dx%d\n", display.width(), display.height());
  touch.begin(1);
}

static guition::Page page = guition::PAGE_DASH;

void loop() {
  static uint32_t last = 0;
  static bool wasDown = false;
  uint32_t now = millis();

  guition::TouchPoint tp;
  bool down = touch.read(tp);
  bool redraw = false;
  if (down && !wasDown) {
    int t = guition::tabHitTest(tp.x, tp.y);
    if (t >= 0) { page = (guition::Page)t; redraw = true; }
  }
  wasDown = down;

  if (redraw || now - last >= 500) {
    last = now;
    // Synthetic "day": SoC swings, solar follows a sine, a drive window pushes DC-DC.
    float phase = fmodf(now / 1000.0f, 60.0f) / 60.0f;   // 60s day
    float sun = sinf(phase * 2.0f * PI);
    bool engine = (phase > 0.6f && phase < 0.8f);

    guition::DashData d;
    d.battValid = true;
    d.soc = 50 + 45 * sinf(phase * 2 * PI);
    d.v = 13.2f + 0.3f * sun;
    float solarA = sun > 0 ? sun * 18 : 0;
    float dcdcA = engine ? 28 : 0;
    float loadA = 5;
    d.a = solarA + dcdcA - loadA;
    d.mode = d.a > 0.5f ? "Charging" : (d.a < -0.5f ? "Discharging" : "Idle");
    d.ttgValid = true; d.ttg = 320;
    d.starterValid = true; d.starterV = 12.6f;
    d.solarValid = true; d.solarW = solarA * 13; d.solarA = solarA;
    d.chargerValid = false;
    d.dcdcValid = engine; d.dcdcOutA = dcdcA; d.dcdcInVValid = engine; d.dcdcInV = 13.8f;
    d.loadValid = true; d.loadA = loadA; d.loadDerived = true;

    guition::renderPage(display.canvas(), page, d);
    display.flush();
  }
  delay(5);
}
