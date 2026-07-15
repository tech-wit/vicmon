// LilyGo T-Display-S3 (buttons) panel bring-up — standalone reference sketch, the
// LilyGo counterpart to src/gfxref (Guition). Proves the ST7789 320x170 panel +
// backlight + the two tactile buttons in isolation, before the driver is wired
// into the master app behind BOARD_LILYGO / DashData.
//
// Hardware: ESP32-S3-WROOM-1 N16R8, 1.9" 170x320 ST7789 on the S3 LCD_CAM 8-bit
// parallel bus. Pins from the LilyGo T-Display-S3 schematic / board pins_arduino.h
// (do NOT change without re-checking against the board):
//   POWER_ON=15 (must be HIGH to power the panel + rail)   BL=38 (backlight)
//   8-bit bus: DC=7 CS=6 WR=8 RD=9   D0..D7=39,40,41,42,45,46,47,48   RST=5
//   Buttons: BOOT=0 (left), KEY=14 (right) — active LOW, internal pull-ups.
//
// The test frame is deliberately self-diagnosing: a 1px border hugs all four
// edges (if it's inset or wrapped, the ST7789 column/row offset is wrong) and
// each corner is labelled TL/TR/BL/BR (reveals rotation/mirroring at a glance).

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ---- LilyGo T-Display-S3 pin map -------------------------------------------
static constexpr int PIN_POWER_ON = 15;  // LCD + board power rail enable
static constexpr int PIN_BL       = 38;  // backlight
static constexpr int PIN_DC       = 7;
static constexpr int PIN_CS       = 6;
static constexpr int PIN_WR       = 8;
static constexpr int PIN_RD       = 9;
static constexpr int PIN_RST      = 5;
static constexpr int PIN_D0 = 39, PIN_D1 = 40, PIN_D2 = 41, PIN_D3 = 42;
static constexpr int PIN_D4 = 45, PIN_D5 = 46, PIN_D6 = 47, PIN_D7 = 48;
static constexpr int PIN_BTN_BOOT = 0;   // left button (also BOOT)
static constexpr int PIN_BTN_KEY  = 14;  // right button

// Native panel is 170 (w) x 320 (h) portrait with a 35px column offset; rotation
// 1/3 gives the 320x170 landscape we want. Start at 3 (USB to the left).
static constexpr uint8_t kRotation = 3;

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX*     gfx = nullptr;

// Backlight via LEDC PWM so we can prove brightness control too.
static constexpr uint8_t  kBlChannel = 0;
static constexpr uint32_t kBlFreq    = 5000;
static constexpr uint8_t  kBlResBits = 8;
static void backlight(uint8_t pct) {
    if (pct > 100) pct = 100;
    ledcWrite(kBlChannel, (uint32_t)pct * 255 / 100);
}

static void drawTestFrame(int16_t w, int16_t h) {
    gfx->fillScreen(BLACK);

    // Full-bleed border: must touch every edge. Inset/wrapped => bad offset.
    gfx->drawRect(0, 0, w, h, WHITE);
    gfx->drawRect(1, 1, w - 2, h - 2, RGB565(80, 80, 80));

    // Vertical colour bars across the top band — proves colour order (RGB, not BGR).
    const uint16_t bars[] = {RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE};
    const int nb = sizeof(bars) / sizeof(bars[0]);
    int bw = w / nb;
    for (int i = 0; i < nb; ++i) gfx->fillRect(4 + i * bw, 4, bw - 2, 26, bars[i]);

    // Labelled corner markers — reveal rotation / mirroring instantly.
    gfx->setTextColor(BLACK);
    gfx->setTextSize(1);
    auto corner = [&](int16_t x, int16_t y, uint16_t c, const char* s) {
        gfx->fillRect(x, y, 20, 12, c);
        gfx->setCursor(x + 2, y + 2);
        gfx->print(s);
    };
    corner(2, 2, GREEN, "TL");
    corner(w - 22, 2, GREEN, "TR");
    corner(2, h - 14, GREEN, "BL");
    corner(w - 22, h - 14, GREEN, "BR");

    // Title + resolution.
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(10, 44);
    gfx->print("Vicmon");
    gfx->setTextSize(1);
    gfx->setCursor(10, 66);
    gfx->print("LilyGo T-Display-S3 bring-up");
    gfx->setCursor(10, 80);
    gfx->printf("ST7789  %dx%d  rot%d", w, h, kRotation);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[lilygoref] T-Display-S3 panel bring-up");

    // The panel and its rail are gated behind POWER_ON — nothing lights without it.
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);

    ledcSetup(kBlChannel, kBlFreq, kBlResBits);
    ledcAttachPin(PIN_BL, kBlChannel);
    backlight(0);  // stay dark until the first frame is pushed (no boot flash)

    pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
    pinMode(PIN_BTN_KEY, INPUT_PULLUP);

    bus = new Arduino_ESP32LCD8(PIN_DC, PIN_CS, PIN_WR, PIN_RD,
                                PIN_D0, PIN_D1, PIN_D2, PIN_D3,
                                PIN_D4, PIN_D5, PIN_D6, PIN_D7);
    // IPS panel; 170x320 native with a 35px column offset on both parities.
    gfx = new Arduino_ST7789(bus, PIN_RST, kRotation, true /*IPS*/,
                             170, 320, 35, 0, 35, 0);
    if (!gfx->begin()) {
        Serial.println("[lilygoref] gfx->begin() FAILED");
        return;
    }
    Serial.printf("[lilygoref] panel up: %dx%d\n", gfx->width(), gfx->height());

    drawTestFrame(gfx->width(), gfx->height());
    backlight(100);
    Serial.println("[lilygoref] frame drawn, backlight on. Press BOOT(0) / KEY(14).");
}

void loop() {
    static uint32_t lastSec = 0;
    static int lastBoot = -1, lastKey = -1;
    int b = digitalRead(PIN_BTN_BOOT);  // active LOW
    int k = digitalRead(PIN_BTN_KEY);

    if (b != lastBoot || k != lastKey) {
        lastBoot = b;
        lastKey = k;
        Serial.printf("[lilygoref] BOOT=%s  KEY=%s\n",
                      b == LOW ? "DOWN" : "up", k == LOW ? "DOWN" : "up");
        // Live button state on-screen: filled = pressed.
        int16_t w = gfx->width(), h = gfx->height();
        int16_t y = h - 40;
        gfx->fillRect(10, y, 130, 22, BLACK);
        gfx->setTextSize(1);
        gfx->setTextColor(WHITE);
        if (b == LOW) gfx->fillRect(10, y, 60, 20, RED);
        else gfx->drawRect(10, y, 60, 20, RED);
        gfx->setCursor(20, y + 6);
        gfx->setTextColor(b == LOW ? BLACK : RED);
        gfx->print("BOOT");
        if (k == LOW) gfx->fillRect(80, y, 60, 20, CYAN);
        else gfx->drawRect(80, y, 60, 20, CYAN);
        gfx->setCursor(92, y + 6);
        gfx->setTextColor(k == LOW ? BLACK : CYAN);
        gfx->print("KEY");
    }

    // Once-a-second uptime tick — proves the panel keeps taking writes.
    uint32_t s = millis() / 1000;
    if (s != lastSec) {
        lastSec = s;
        int16_t w = gfx->width();
        gfx->fillRect(w - 70, 44, 66, 16, BLACK);
        gfx->setTextSize(1);
        gfx->setTextColor(RGB565(0, 255, 0));
        gfx->setCursor(w - 68, 48);
        gfx->printf("up %lus", (unsigned long)s);
    }
    delay(20);
}
