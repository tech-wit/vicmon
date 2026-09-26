// M5Capsule (M5Stack, integrated StampS3) peripheral support.
//
// The Capsule is a headless board — no panel — so it plugs into the universal
// image as a set of peripherals rather than a display backend:
//   • BM8563 RTC  -> the time source (seeds the system clock at boot; written back
//                    whenever NTP/manual sets it), so the daily-stats rollover and
//                    the SD log work with a real calendar WITHOUT needing NTP.
//   • Buzzer      -> audible alarm that follows the SoC-critical alert (muted
//                    while charging).
//   • microSD     -> long-history CSV log (one file per day), far beyond the 24 h
//                    on-chip ring buffer.
//   • Power-hold  -> latch the power circuit on so it keeps running off its internal
//                    battery (the StampS3 power button otherwise drops the rail).
//   • Button      -> the side push-button opens/closes the ESP-NOW pairing window.
//   • WS2812 LED  -> status colour normally, a bright flash while pairing is open.
//
// The button and the LED are driven from a small dedicated task rather than the
// main loop, because the loop spends ~2 s of every pass inside a blocking BLE
// scan: polled from there a press would often be missed outright and a "flash"
// would be a sullen blink every two seconds. The task also becomes the SOLE
// owner of neopixelWrite() for this board (the loop publishes a colour via
// capsuleSetLed instead), so two tasks never drive the RMT peripheral at once.
//
// detectBoard() (main.cpp) probes the RTC to positively identify the Capsule; a
// board that isn't one never reaches this code.  Everything here is behind
// VICMON_HAS_M5CAPSULE so the standalone reference envs don't pull it in.

#include "app.h"

#ifdef VICMON_HAS_M5CAPSULE

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <sys/time.h>

// ---- pin map (M5Capsule / StampS3, from M5Unified board_M5Capsule) ----------
static constexpr int PIN_POWER_HOLD = 46;   // drive HIGH to hold the power rail on
static constexpr int PIN_I2C_SDA    = 8;    // internal I2C (BM8563 RTC @0x51, IMU)
static constexpr int PIN_I2C_SCL    = 10;
static constexpr int PIN_BUZZER     = 2;    // passive buzzer, driven by LEDC tone
static constexpr int PIN_SD_SCK     = 14;
static constexpr int PIN_SD_MOSI    = 12;
static constexpr int PIN_SD_MISO    = 39;
static constexpr int PIN_SD_CS      = 11;
static constexpr int PIN_BUTTON     = 42;   // side push-button (BtnA), active LOW
static constexpr int PIN_LED        = 21;   // WS2812 RGB status LED (data)
// Capsule v1.1 (Stamp-S3A) puts the RGB LED behind an independent electronic power
// switch to save power: GPIO38 must be driven HIGH or the LED is simply unpowered
// and ignores everything sent on GPIO21 — silently, with no way to tell the
// difference from a dead LED. On the older v1.0 (plain StampS3) GPIO38 is unused,
// so driving it HIGH is harmless there and one image covers both revisions.
static constexpr int PIN_LED_POWER  = 38;

static constexpr uint8_t RTC_ADDR   = 0x51; // BM8563 (PCF8563-compatible)
static constexpr int     BUZZER_CH  = 6;    // LEDC channel (0-3 may be used by panel PWM)

// ---- module state ----------------------------------------------------------
static bool sBusUp  = false;   // internal I2C started
static bool sRtcOk  = false;   // RTC present and holding a valid (>= 2023) time
static bool sSdOk   = false;   // microSD mounted

// Steady status colour published by the loop (capsuleSetLed) and rendered by the
// button/LED task. Packed into one word so a reader can never tear a half-updated
// colour out of three separate bytes.
static volatile uint32_t sLedSteady = 0;
// While sLedTestActive is set the task leaves the pixel alone, so capsuleLedTest()
// can drive it without the 25 ms tick overwriting each colour immediately. An
// explicit flag rather than a magic "0 = not testing" deadline: millis() cast to
// int32_t goes negative after ~24.8 days of uptime, which would make a bare
// `(int32_t)(now - 0) < 0` deadline compare true forever and silently kill the
// status LED on any long-running device.
static volatile bool sLedTestActive = false;
static volatile uint32_t sLedTestUntil = 0;

bool capsuleRtcOk() { return sRtcOk; }
bool capsuleSdOk()  { return sSdOk; }

// Called from the loop's updateLed(). Stores only — the task does the writing.
void capsuleSetLed(uint8_t r, uint8_t g, uint8_t b) {
    sLedSteady = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ---- small helpers ---------------------------------------------------------
static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0f)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

// Proleptic-Gregorian date<->unix-day conversion (Howard Hinnant's algorithms).
// Avoids relying on timegm(), which isn't dependable across newlib configs.
static long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void civilFromDays(long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

// ---- BM8563 real-time clock ------------------------------------------------
// Register layout (matches the PCF8563): 0x02 sec (bit7 = VL/invalid), 0x03 min,
// 0x04 hour, 0x05 day, 0x06 weekday, 0x07 month (bit7 = century), 0x08 year.
uint32_t capsuleRtcUtc() {
    if (!sBusUp) return 0;
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((int)RTC_ADDR, 7) != 7) return 0;
    uint8_t sec = Wire.read(), minu = Wire.read(), hour = Wire.read();
    uint8_t day = Wire.read();
    Wire.read();  // weekday (unused)
    uint8_t mon = Wire.read(), yr = Wire.read();
    if (sec & 0x80) return 0;  // VL: clock integrity lost since last set -> unset
    int      Y  = 2000 + bcd2dec(yr);
    unsigned M  = bcd2dec(mon & 0x1f);
    unsigned D  = bcd2dec(day & 0x3f);
    unsigned h  = bcd2dec(hour & 0x3f);
    unsigned mi = bcd2dec(minu & 0x7f);
    unsigned s  = bcd2dec(sec & 0x7f);
    if (Y < 2023 || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || s > 59)
        return 0;  // never set (or garbage) -> treat as no clock
    return (uint32_t)(daysFromCivil(Y, M, D) * 86400L + h * 3600L + mi * 60L + s);
}

void capsuleRtcSet(uint32_t utc) {
    if (!sBusUp || utc < 1700000000u) return;
    long days = (long)(utc / 86400);
    unsigned rem = utc % 86400;
    int Y; unsigned M, D;
    civilFromDays(days, Y, M, D);
    unsigned h = rem / 3600, mi = (rem % 3600) / 60, s = rem % 60;
    unsigned wd = (unsigned)((days % 7 + 11) % 7);  // 1970-01-01 was a Thursday (=4)
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x02);
    Wire.write(dec2bcd((uint8_t)s));   // writing seconds also clears the VL flag
    Wire.write(dec2bcd((uint8_t)mi));
    Wire.write(dec2bcd((uint8_t)h));
    Wire.write(dec2bcd((uint8_t)D));
    Wire.write((uint8_t)wd);
    Wire.write(dec2bcd((uint8_t)M));   // century bit 0 => 20xx
    Wire.write(dec2bcd((uint8_t)(Y - 2000)));
    Wire.endTransmission();
    sRtcOk = true;
}

// ---- detection -------------------------------------------------------------
// Bring up the internal I2C bus and see whether the BM8563 ACKs. Neither the
// Guition nor the LilyGo has a device at 0x51 on these pins, so an ACK uniquely
// identifies the Capsule. Leaves the bus running on success so capsuleBringUp()
// (and the RTC read) can reuse it; detectBoard() releases the pins on a miss.
bool capsuleProbe() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
    sBusUp = true;
    Wire.beginTransmission(RTC_ADDR);
    return Wire.endTransmission() == 0;
}

// ---- bring-up --------------------------------------------------------------
static void capsuleTask(void*);  // button + status LED (defined below)

void capsuleBringUp() {
    // Latch power ASAP so a battery-only cold boot doesn't drop the rail once the
    // StampS3 power button is released.
    pinMode(PIN_POWER_HOLD, OUTPUT);
    digitalWrite(PIN_POWER_HOLD, HIGH);

    if (!sBusUp) capsuleProbe();  // ensure the RTC bus is up (idempotent)

    // Buzzer: LEDC tone generator, silent until the alarm sounds.
    ledcSetup(BUZZER_CH, 3000, 8);
    ledcAttachPin(PIN_BUZZER, BUZZER_CH);
    ledcWriteTone(BUZZER_CH, 0);

    // microSD over a dedicated SPI bus.
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    sSdOk = SD.begin(PIN_SD_CS, SPI, 20000000);
    if (sSdOk) SD.mkdir("/vicmon");

    // Seed the system clock from the RTC so time() (and thus currentUtcEpoch())
    // reports real calendar time with no NTP. If NTP later syncs it wins and gets
    // written back to the RTC by serviceClockPersist()->saveClock().
    uint32_t u = capsuleRtcUtc();
    if (u) {
        struct timeval tv = { (time_t)u, 0 };
        settimeofday(&tv, nullptr);
        sRtcOk = true;
    }
    // Power the RGB LED's rail before the task starts driving it.
    pinMode(PIN_LED_POWER, OUTPUT);
    digitalWrite(PIN_LED_POWER, HIGH);

    // Button + status LED, on their own task (see the file header for why).
    // Core 1 alongside loopTask, leaving core 0 to WiFi/BLE.
    xTaskCreatePinnedToCore(capsuleTask, "vcapbtn", 2560, nullptr, 1, nullptr, 1);

    Serial.printf("[capsule] power-hold on, RTC %s, SD %s, button GPIO%d, "
                  "LED GPIO%d (rail GPIO%d on)\n",
                  sRtcOk ? "set" : "unset", sSdOk ? "mounted" : "none",
                  PIN_BUTTON, PIN_LED, PIN_LED_POWER);
}

// ---- button + status LED ---------------------------------------------------
// One task owns both, ticking fast enough that a press is never missed and the
// pairing flash actually looks like a flash.

static constexpr uint32_t LED_TICK_MS   = 25;    // task period
static constexpr uint32_t BTN_DEBOUNCE  = 40;    // ms the level must hold to count
static constexpr uint32_t BTN_LONG_MS   = 1500;  // press longer than this is ignored
static constexpr uint32_t FLASH_HALF_MS = 150;   // pairing flash: ~3.3 Hz

// Pairing flash colour: a bright amber that none of the steady status colours use
// (those are red / red-green / green / green-blue / faint white), so "pairing" is
// unmistakable at a glance rather than a slightly different shade of the usual.
static constexpr uint8_t FLASH_R = 120, FLASH_G = 70, FLASH_B = 0;

// Debounced press/release edge detector. Returns true once per completed press
// that was shorter than BTN_LONG_MS — acting on RELEASE rather than press means a
// press-and-hold can be abandoned, and a stuck/shorted button can't machine-gun
// the pairing window open and shut.
static bool buttonClicked(uint32_t now) {
    static bool stable = true;        // debounced level (true = released, pin is active LOW)
    static bool lastRaw = true;
    static uint32_t changedAt = 0;
    static uint32_t pressedAt = 0;

    bool raw = digitalRead(PIN_BUTTON) != LOW;
    if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
    if (raw == stable || (now - changedAt) < BTN_DEBOUNCE) return false;

    stable = raw;
    if (!stable) { pressedAt = now; return false; }       // press edge
    return (now - pressedAt) < BTN_LONG_MS;               // release edge: a click?
}

static void capsuleTask(void*) {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    uint32_t lastWriteColour = 0xFFFFFFFFu;  // impossible value -> always write once
    bool flashOn = false;
    uint32_t flashAt = 0;

    for (;;) {
        uint32_t now = millis();

        // A self-test owns the pixel while it runs; resync afterwards so the next
        // tick repaints whatever the current state should be.
        if (sLedTestActive && (int32_t)(now - sLedTestUntil) < 0) {
            lastWriteColour = 0xFFFFFFFFu;
            vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
            continue;
        }

        if (buttonClicked(now)) {
            togglePairingMode();
            Serial.printf("[capsule] button: pairing %s\n",
                          pairingModeActive() ? "OPEN" : "closed");
        }

        uint32_t want;
        if (pairingModeActive()) {
            if ((int32_t)(now - flashAt) >= 0) {
                flashOn = !flashOn;
                flashAt = now + FLASH_HALF_MS;
            }
            want = flashOn ? (((uint32_t)FLASH_R << 16) | ((uint32_t)FLASH_G << 8) | FLASH_B) : 0;
        } else {
            flashOn = false;
            flashAt = now;  // next pairing window starts lit immediately
            want = sLedSteady;
        }

        // Only touch the RMT when the colour actually changes — at a 25 ms tick an
        // unconditional write would be 40 needless bit-bangs a second.
        if (want != lastWriteColour) {
            lastWriteColour = want;
            neopixelWrite(PIN_LED, (want >> 16) & 0xFF, (want >> 8) & 0xFF, want & 0xFF);
        }
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

// Serial `led`: drive the WS2812 through unmistakable full-brightness colours so
// it is obvious whether the pin drives a visible LED at all. The normal status
// colours are deliberately dim (2..40) to avoid a blinding indicator in a dark
// cab, which makes "not working" and "working but barely visible" hard to tell
// apart by eye — this removes that ambiguity.
void capsuleLedTest() {
    // Deliberately NO red: the Capsule has its own hardware power/charge LED that
    // glows red and is not under software control, so a red step in this test is
    // indistinguishable from it. Green/blue/white with OFF gaps between means
    // anything the observer sees is unambiguously us driving the WS2812.
    struct Step { const char* name; uint8_t r, g, b; uint16_t ms; };
    static const Step steps[] = {
        {"GREEN", 0, 255, 0, 2500}, {"off", 0, 0, 0, 1200},
        {"BLUE", 0, 0, 255, 2500},  {"off", 0, 0, 0, 1200},
        {"WHITE", 255, 255, 255, 2500}, {"off", 0, 0, 0, 1200},
    };
    Serial.printf("[led] GPIO%d (rail GPIO%d) — watch for GREEN, BLUE, WHITE\n",
                  PIN_LED, PIN_LED_POWER);
    for (const Step& st : steps) {
        sLedTestUntil = millis() + st.ms + 1500;  // keep the task off the pixel
        sLedTestActive = true;
        Serial.printf("[led]   %s\n", st.name);
        neopixelWrite(PIN_LED, st.r, st.g, st.b);
        delay(st.ms);
    }
    neopixelWrite(PIN_LED, 0, 0, 0);
    sLedTestActive = false;  // hand the pixel back
    Serial.println("[led] self-test done");
}

// ---- buzzer ----------------------------------------------------------------
// Non-blocking alarm: while the caller holds `alarm` true, emit a short beep
// roughly every 30 s. Silent otherwise, or if the user disabled the buzzer. The
// caller decides what sounds it (SoC-critical, and not charging) — see loop().
void capsuleServiceBuzzer(bool alarm, uint32_t now) {
    static bool     beeping = false;
    static uint32_t beepOffAt = 0, nextBeepAt = 0;
    if (!gBuzzerEnable) alarm = false;
    if (beeping && (int32_t)(now - beepOffAt) >= 0) {  // end an in-progress beep
        ledcWriteTone(BUZZER_CH, 0);
        beeping = false;
    }
    if (!alarm) { nextBeepAt = 0; return; }
    if (nextBeepAt == 0) nextBeepAt = now;             // beep immediately on entering the alarm
    if (!beeping && (int32_t)(now - nextBeepAt) >= 0) {
        ledcWriteTone(BUZZER_CH, 3000);
        beeping = true;
        beepOffAt  = now + 150;                        // 150 ms chirp
        nextBeepAt = now + 30000;                      // repeat every 30 s while it holds
    }
}

// ---- microSD data log ------------------------------------------------------
// Append one CSV row per minute to /vicmon/YYYYMMDD.csv (named from local date).
// Needs a valid clock for the filename + timestamp, so it no-ops until the RTC
// (or NTP) has one. Values are written blank when the signal isn't available.
// Column set of the daily log. Adding a column here changes the shape of rows
// appended to a file that a PREVIOUS firmware started earlier the same day, which
// would leave a parser silently mis-reading everything after the upgrade — so the
// header of an existing file is checked and re-emitted when it no longer matches.
static const char kCsvHeader[] =
    "utc,localtime,soc,vbat,ibat,solar,charger,dcdc,load,vstart,"
    "tempC,humidity,pressure_hpa,gas_kohm";

// True when `path` already starts with exactly kCsvHeader (i.e. same schema).
static bool csvHeaderMatches(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    String first = f.readStringUntil('\n');
    f.close();
    first.trim();
    return first == kCsvHeader;
}

static void csvNum(String& row, const sig::Resolved& r, int dp) {
    row += ',';
    if (r.valid) row += String(r.value, dp);
}

void capsuleLogSample(uint32_t now) {
    static uint32_t lastLogMs = 0;
    if (!sSdOk) return;
    if (lastLogMs != 0 && now - lastLogMs < 60000) return;  // one row / minute
    uint32_t utc = currentUtcEpoch();
    if (!utc) return;                                       // no clock yet -> can't date it
    lastLogMs = now;

    uint32_t local = currentLocalEpoch();
    long days = (long)(local / 86400);
    unsigned rem = local % 86400;
    int Y; unsigned M, D;
    civilFromDays(days, Y, M, D);
    unsigned h = rem / 3600, mi = (rem % 3600) / 60, s = rem % 60;

    char path[32], tstr[24];
    snprintf(path, sizeof(path), "/vicmon/%04d%02u%02u.csv", Y, M, D);
    snprintf(tstr, sizeof(tstr), "%04d-%02u-%02u %02u:%02u:%02u", Y, M, D, h, mi, s);

    // Write the header for a brand-new file, and also when an existing one was
    // started by a firmware with a different column set (the day of an upgrade).
    // A second header line mid-file is at least self-describing; rows that just
    // silently grew four columns would not be. The check costs a file open, so do
    // it once per file rather than on every one-minute row — the answer can only
    // change when the date (and so the path) rolls over.
    static char checkedPath[32] = {0};
    bool needHeader = false;
    if (strcmp(checkedPath, path) != 0) {
        needHeader = !SD.exists(path) || !csvHeaderMatches(path);
        snprintf(checkedPath, sizeof(checkedPath), "%s", path);
    }
    File f = SD.open(path, FILE_APPEND);
    if (!f) { sSdOk = false; return; }  // card pulled? stop trying until reboot
    if (needHeader) f.println(kCsvHeader);

    PanelModel pm = collectPanel(now);
    String row = String(utc) + ',' + tstr;
    csvNum(row, pm.soc, 1);
    csvNum(row, pm.battV, 2);
    csvNum(row, pm.battA, 1);
    csvNum(row, pm.solarA, 1);
    csvNum(row, pm.chargerA, 1);
    csvNum(row, pm.dcdcOutA, 1);
    csvNum(row, pm.loadA, 1);
    csvNum(row, pm.starterV, 2);
    // Environment (Unit ENV Pro). Blank columns where no sensor is attached, or
    // for gas alone while its heater is still settling — same "absent means
    // unknown, never zero" convention as the electrical columns above.
    const EnvReading& e = envReading();
    csvNum(row, {e.tempC, e.valid}, 2);
    csvNum(row, {e.humidity, e.valid}, 1);
    csvNum(row, {e.pressureHpa, e.valid}, 1);
    csvNum(row, {e.gasOhm / 1000.0f, e.gasValid}, 1);
    f.println(row);
    f.close();
}

// Serial diagnostic: list the microSD log files (name + size) under /vicmon.
void capsuleDumpSd() {
    if (!sSdOk) { Serial.println("[sd] not mounted"); return; }
    File dir = SD.open("/vicmon");
    if (!dir) { Serial.println("[sd] /vicmon missing"); return; }
    int n = 0;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        Serial.printf("[sd] %s  %lu bytes\n", e.name(), (unsigned long)e.size());
        e.close();
        ++n;
    }
    dir.close();
    if (!n) Serial.println("[sd] no log files yet");
}

#endif  // VICMON_HAS_M5CAPSULE
