// Unit ENV Pro (Bosch BME688) on the M5Capsule's Grove Port A.
//
// Adds cabin environment to the monitor: temperature, relative humidity,
// barometric pressure and gas resistance. The readings join the same paths the
// Victron signals use — the history rings (web Trend + Environment charts), the
// microSD daily CSV, the panel JSON and the ESP-NOW snapshot a slave mimics.
//
// Why a separate I2C bus: the Capsule's INTERNAL bus (Wire, SDA=8/SCL=10) already
// carries the BM8563 RTC and is brought up by capsuleBringUp() before this runs.
// Port A is a different pair of pins entirely (SDA=13/SCL=15, per M5Unified's
// board_M5Capsule external-I2C entry), so the sensor gets Wire1 to itself. 100 kHz
// rather than 400: Grove leads are long, unshielded and often daisy-chained.
//
// Why no BSEC: Bosch's IAQ/eCO2 index needs their closed-source pre-compiled BSEC2
// blob (restrictive licence, ~100 KB flash, NVS-backed calibration state) — a poor
// trade on a no-PSRAM StampS3 that is already tight on heap. The raw gas resistance
// is exposed instead: it is not an absolute air-quality number, but it tracks VOC
// load well as a *relative* trend, which is what the graph shows. Rising resistance
// = cleaner air; falling = more VOCs.
//
// Sampling is a non-blocking two-step state machine. A BME688 forced-mode
// measurement with the gas heater takes ~200 ms, which is far too long to sit and
// block in the loop (which is already spending ~2 s per BLE scan). Instead one
// pass kicks the measurement off and a later pass collects it, so the loop never
// waits on the sensor.

#include "app.h"

#ifdef VICMON_HAS_ENVPRO

#include <Arduino.h>
#include <Wire.h>
#include <bme68xLibrary.h>

// ---- Port A (Grove HY2.0-4P) on the M5Capsule -------------------------------
static constexpr int PIN_PORTA_SDA = 13;
static constexpr int PIN_PORTA_SCL = 15;
static constexpr uint32_t PORTA_HZ = 100000;

// The ENV Pro straps the BME688 to 0x77; 0x76 is the same part with SDO pulled
// low, which is what most third-party BME680/688 breakouts do. Try both so a
// generic sensor on the same port works.
static constexpr uint8_t kAddrPrimary = 0x77;
static constexpr uint8_t kAddrAlt = 0x76;

// Heater profile: 320 degC for 150 ms is Bosch's general-purpose VOC setpoint and
// what their own examples use. Hotter/longer reads more gases but costs power and
// measurement time.
static constexpr uint16_t kHeaterTempC = 320;
static constexpr uint16_t kHeaterMs = 150;

// One sample every 5 s, matching the fine history ring's interval — there is no
// point measuring faster than the ring can record, and the gas heater is the most
// power-hungry thing on the board.
static constexpr uint32_t kSamplePeriodMs = 5000;

// A reading older than this stops being reported as live (same spirit as the
// Victron device staleness): two missed samples means something is wrong.
static constexpr uint32_t kStaleMs = 30000;

// ---- module state ----------------------------------------------------------
static Bme68x sBme;
static bool sPresent = false;        // sensor found and initialised
static uint8_t sAddr = 0;            // address it actually answered on
static EnvReading sLast;             // most recent good reading
static uint32_t sNextSampleMs = 0;   // when to trigger the next measurement
static uint32_t sReadyAtMs = 0;      // when the in-flight measurement should be done
static bool sMeasuring = false;      // a forced-mode measurement is in flight
static uint16_t sFailStreak = 0;     // consecutive collect failures (re-init after a while)

bool envPresent() { return sPresent; }

// Pure read — deliberately does NOT touch sLast. The web handlers run on the
// AsyncTCP task while envService() runs on the loop task, so a reader that also
// wrote would be racing the writer on every /api/panel poll. Expiry is done by
// envService() instead (expireIfStale, below), which is loop-task-only.
const EnvReading& envReading() { return sLast; }

// A yanked Grove lead should blank the panel rather than freeze the last numbers
// on screen forever. Called from envService on the loop task only.
static void expireIfStale(uint32_t now) {
    if (sLast.valid && now - sLast.lastMs > kStaleMs) {
        sLast.valid = false;
        sLast.gasValid = false;
    }
}

// ---- bring-up --------------------------------------------------------------
// Returns true if a BME68x answered. Safe to call again to re-probe after a
// cable was plugged in (or pulled and re-seated).
static bool tryInit(uint8_t addr) {
    sBme.begin(addr, Wire1);
    if (sBme.checkStatus() == BME68X_ERROR) return false;
    // 2x temperature / 16x pressure / 1x humidity is Bosch's recommended set for
    // the "low power" gas profile; the defaults of setTPH() already match it.
    sBme.setTPH();
    sBme.setHeaterProf(kHeaterTempC, kHeaterMs);
    return sBme.checkStatus() != BME68X_ERROR;
}

void envBringUp() {
    // Port A gets its own bus — Wire is the Capsule's internal RTC/IMU bus.
    Wire1.begin(PIN_PORTA_SDA, PIN_PORTA_SCL, PORTA_HZ);
    sPresent = false;
    for (uint8_t a : {kAddrPrimary, kAddrAlt}) {
        if (tryInit(a)) { sPresent = true; sAddr = a; break; }
    }
    if (sPresent)
        Serial.printf("[env] BME68x @0x%02X on Port A (SDA=%d SCL=%d), heater %uC/%ums\n",
                      sAddr, PIN_PORTA_SDA, PIN_PORTA_SCL, kHeaterTempC, kHeaterMs);
    else
        Serial.printf("[env] no BME68x on Port A (SDA=%d SCL=%d) — run `env` to scan\n",
                      PIN_PORTA_SDA, PIN_PORTA_SCL);
    sNextSampleMs = millis();
}

// ---- sampling --------------------------------------------------------------
// Two-step, non-blocking: trigger a forced-mode measurement, then collect it once
// the sensor's own reported duration has elapsed. Call every loop; it does nothing
// until something is due.
void envService(uint32_t now) {
    expireIfStale(now);  // before the early returns below, so a dead sensor does expire
    if (!sPresent) {
        // Re-probe occasionally so plugging the unit in (or re-seating a Grove
        // lead) recovers without a reboot.
        if ((int32_t)(now - sNextSampleMs) < 0) return;
        sNextSampleMs = now + 5000;
        for (uint8_t a : {kAddrPrimary, kAddrAlt}) {
            if (tryInit(a)) {
                sPresent = true;
                sAddr = a;
                Serial.printf("[env] BME68x @0x%02X appeared on Port A\n", sAddr);
                break;
            }
        }
        return;
    }

    if (!sMeasuring) {
        if ((int32_t)(now - sNextSampleMs) < 0) return;
        sBme.setOpMode(BME68X_FORCED_MODE);
        if (sBme.checkStatus() == BME68X_ERROR) {  // bus gone?
            sNextSampleMs = now + kSamplePeriodMs;
            if (++sFailStreak > 5) { sPresent = false; sFailStreak = 0; }
            return;
        }
        // getMeasDur() covers the T/P/H conversion only — the gas heater's soak time
        // is configured separately and must be added on top (as Bosch's C forced-mode
        // example does; their Arduino example omits it and under-waits).
        sReadyAtMs = now + (sBme.getMeasDur(BME68X_FORCED_MODE) / 1000) + kHeaterMs + 5;
        sMeasuring = true;
        return;
    }

    if ((int32_t)(now - sReadyAtMs) < 0) return;  // still cooking
    sMeasuring = false;
    sNextSampleMs = now + kSamplePeriodMs;

    // NB: fetchData() is the success test, NOT getData(). In forced mode getData()
    // copies the field into `d` and then returns 0 unconditionally (it only returns
    // a count for parallel/sequential mode), so treating its return value as
    // "worked" discards every good reading. Bosch's own example ignores it too.
    bme68xData d = {};
    uint8_t n = sBme.fetchData();
    sBme.getData(d);
    if (!n || !(d.status & BME68X_NEW_DATA_MSK)) {
        ++sFailStreak;
        // Report the first failure of a run and then every 20th, so a persistent
        // fault is visible on the console without drowning it.
        if (sFailStreak == 1 || (sFailStreak % 20) == 0)
            Serial.printf("[env] read failed (n=%u api=%d dstatus=0x%02X) streak=%u\n",
                          n, sBme.checkStatus(), d.status, sFailStreak);
        if (sFailStreak > 5) { sPresent = false; sFailStreak = 0; }
        return;
    }
    sFailStreak = 0;
    sLast.tempC = d.temperature;          // degC
    sLast.humidity = d.humidity;          // %RH
    sLast.pressureHpa = d.pressure / 100.0f;  // the API reports Pascals
    sLast.gasOhm = d.gas_resistance;
    // The gas number is only meaningful once the heater has reached and held its
    // setpoint — from cold that takes several measurement cycles, during which the
    // sensor still returns a (meaningless) resistance. Gate on both of Bosch's flags.
    sLast.gasValid = (d.status & BME68X_GASM_VALID_MSK) && (d.status & BME68X_HEAT_STAB_MSK);
    sLast.valid = true;
    sLast.lastMs = now;
}

// ---- diagnostics -----------------------------------------------------------
// Serial `env`: scan Port A and report the live reading. The scan is the useful
// half when nothing is found — it says whether the wiring is dead or the unit is
// simply answering on an address we didn't expect.
void envDump() {
    Serial.printf("[env] Port A SDA=%d SCL=%d @%luHz — scanning...\n",
                  PIN_PORTA_SDA, PIN_PORTA_SCL, (unsigned long)PORTA_HZ);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; ++a) {
        Wire1.beginTransmission(a);
        if (Wire1.endTransmission() == 0) {
            Serial.printf("[env]   device @0x%02X\n", a);
            ++found;
        }
    }
    if (!found) Serial.println("[env]   nothing responded (check the Grove lead)");
    if (!sPresent) { Serial.println("[env] sensor not initialised"); return; }
    const EnvReading& e = envReading();
    if (!e.valid) { Serial.println("[env] no fresh reading yet"); return; }
    Serial.printf("[env] %.2fC  %.1f%%RH  %.1fhPa  gas %.1fkOhm (%s)  age %lums\n",
                  e.tempC, e.humidity, e.pressureHpa, e.gasOhm / 1000.0f,
                  e.gasValid ? "stable" : "warming", (unsigned long)(millis() - e.lastMs));
}

#endif  // VICMON_HAS_ENVPRO
