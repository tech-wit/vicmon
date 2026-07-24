// M5Capsule (M5Stack, integrated StampS3) peripheral support.
//
// The Capsule is a headless board — no panel — so it plugs into the universal
// image as a set of peripherals rather than a display backend:
//   • BM8563 RTC  -> the time source (seeds the system clock at boot; written back
//                    whenever NTP/manual sets it), so the daily-stats rollover and
//                    the SD log work with a real calendar WITHOUT needing NTP.
//   • Buzzer      -> audible alarm that follows the SoC-critical alert.
//   • microSD     -> long-history CSV log (one file per day), far beyond the 24 h
//                    on-chip ring buffer.
//   • Power-hold  -> latch the power circuit on so it keeps running off its internal
//                    battery (the StampS3 power button otherwise drops the rail).
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

static constexpr uint8_t RTC_ADDR   = 0x51; // BM8563 (PCF8563-compatible)
static constexpr int     BUZZER_CH  = 6;    // LEDC channel (0-3 may be used by panel PWM)

// ---- module state ----------------------------------------------------------
static bool sBusUp  = false;   // internal I2C started
static bool sRtcOk  = false;   // RTC present and holding a valid (>= 2023) time
static bool sSdOk   = false;   // microSD mounted

bool capsuleRtcOk() { return sRtcOk; }
bool capsuleSdOk()  { return sSdOk; }

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
    Serial.printf("[capsule] power-hold on, RTC %s, SD %s\n",
                  sRtcOk ? "set" : "unset", sSdOk ? "mounted" : "none");
}

// ---- buzzer ----------------------------------------------------------------
// Non-blocking alarm: while the SoC-critical condition holds, emit a short beep
// roughly every 30 s. Silent otherwise, or if the user disabled the buzzer.
void capsuleServiceBuzzer(bool socCrit, uint32_t now) {
    static bool     beeping = false;
    static uint32_t beepOffAt = 0, nextBeepAt = 0;
    if (!gBuzzerEnable) socCrit = false;
    if (beeping && (int32_t)(now - beepOffAt) >= 0) {  // end an in-progress beep
        ledcWriteTone(BUZZER_CH, 0);
        beeping = false;
    }
    if (!socCrit) { nextBeepAt = 0; return; }
    if (nextBeepAt == 0) nextBeepAt = now;             // beep immediately on entering crit
    if (!beeping && (int32_t)(now - nextBeepAt) >= 0) {
        ledcWriteTone(BUZZER_CH, 3000);
        beeping = true;
        beepOffAt  = now + 150;                        // 150 ms chirp
        nextBeepAt = now + 30000;                      // repeat every 30 s while critical
    }
}

// ---- microSD data log ------------------------------------------------------
// Append one CSV row per minute to /vicmon/YYYYMMDD.csv (named from local date).
// Needs a valid clock for the filename + timestamp, so it no-ops until the RTC
// (or NTP) has one. Values are written blank when the signal isn't available.
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

    bool isNew = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) { sSdOk = false; return; }  // card pulled? stop trying until reboot
    if (isNew)
        f.println("utc,localtime,soc,vbat,ibat,solar,charger,dcdc,load,vstart");

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
