#pragma once
#include <Arduino.h>

// Energy counters & trip statistics. Integrates the live currents into
// amp-hours / watt-hours and tracks extremes (min/max SoC & V, peak power) over
// three scopes:
//   TODAY  - auto-resets at local midnight when an NTP clock is available,
//            otherwise simply accumulates "since boot".
//   TRIP   - user-resettable journey/camp meter.
//   TOTAL  - lifetime, persisted, reset only on explicit request.
//
// Buckets are persisted to NVS (one blob per profile, written every few minutes
// and on any reset) so a reboot loses at most a few minutes of accumulation.
namespace stats {

// One accumulating set of counters. Doubles are used for the integrals so long
// run-times don't lose precision. NAN min/max means "not yet recorded".
struct Bucket {
    double solarAh = 0, solarWh = 0;
    double dcdcAh = 0, dcdcWh = 0;
    double chargerAh = 0, chargerWh = 0;
    double loadAh = 0, loadWh = 0;
    double chargedAh = 0, chargedWh = 0;        // into battery (net current > 0)
    double dischargedAh = 0, dischargedWh = 0;  // out of battery (magnitude)

    float socMin = NAN, socMax = NAN;
    float vMin = NAN, vMax = NAN;
    float peakSolarW = 0, peakLoadW = 0;
    float peakChargeA = 0, peakDischargeA = 0;

    uint32_t chargeSecs = 0, dischargeSecs = 0;
    uint32_t durationSecs = 0;  // wall-time this bucket has been accumulating
    uint32_t startEpoch = 0;    // local unix seconds at start (0 = clock unknown)
    uint32_t dayStamp = 0;      // yyyymmdd the TODAY bucket belongs to (0 = none)
};

enum Scope { TODAY = 0, TRIP = 1, TOTAL = 2, COUNT = 3 };

// A finished "day", archived from the TODAY bucket at the rollover, for the
// multi-day energy history. A day boundary is local midnight when a clock (NTP
// or manually set) is available; otherwise it's every 24h of run-time, so the
// history works with no clock at all.
struct DayRecord {
    uint32_t dayStamp = 0;  // yyyymmdd (clocked) or run-day index 1.. (no clock); 0 = empty
    float solarAh = 0, dcdcAh = 0, chargerAh = 0;  // Ah in, by source
    float loadAh = 0;                              // Ah out (load)
    float socMin = NAN, socMax = NAN;
};
static const int kDays = 14;  // ring capacity; the UI shows the last 7

// dayStamp values below this are run-day indices (no clock); at/above are
// yyyymmdd calendar stamps. (Smallest real yyyymmdd ~ 2020_01_01.)
static const uint32_t kRunDayMax = 100000;

// A snapshot of the readings to integrate for one step.
struct Sample {
    bool socValid = false;
    float soc = 0;
    bool battVValid = false;
    float battV = 0;
    bool battAValid = false;
    float battA = 0;  // signed: + charging, - discharging
    bool solarValid = false;
    float solarA = 0;
    bool solarWValid = false;
    float solarW = 0;
    bool dcdcValid = false;
    float dcdcA = 0;
    bool chargerValid = false;
    float chargerA = 0;
    bool loadValid = false;
    float loadA = 0;
};

class Stats {
public:
    void begin(int profile);  // load persisted buckets for this profile

    // Integrate one step. `localEpoch` is local unix seconds (already TZ-offset)
    // or 0 if no clock yet; used only for the TODAY midnight rollover + labels.
    void update(const Sample& s, uint32_t nowMs, uint32_t localEpoch);

    void reset(Scope sc);  // clear one scope's counters

    // Persist if dirty and enough time has passed (or force, e.g. on reset).
    void maybePersist(uint32_t nowMs, bool force = false);

    const Bucket& bucket(Scope sc) const { return b_[sc]; }

    // Archived daily history, chronological (0 = oldest of the last `dayCount()`).
    size_t dayCount() const { return dayCount_; }
    const DayRecord& day(size_t i) const {
        size_t start = (dayHead_ + kDays - dayCount_) % kDays;
        return days_[(start + i) % kDays];
    }

    // Total run-time accumulated across reboots (the odometer that drives the
    // no-clock day rollover), and the current run-day index (1-based).
    uint32_t runSecs() const { return (uint32_t)runSecs_; }
    uint32_t runDay() const { return (uint32_t)(runSecs_ / 86400.0) + 1; }

private:
    void load();
    void save();
    void captureDay(const Bucket& finished);  // archive a completed day at rollover
    String ns() const;

    Bucket b_[COUNT];
    DayRecord days_[kDays];
    size_t dayCount_ = 0, dayHead_ = 0;
    double runSecs_ = 0;  // persisted run-time odometer (no-clock day boundary)
    int profile_ = 0;
    uint32_t lastMs_ = 0;
    uint32_t lastSaveMs_ = 0;
    bool dirty_ = false;
};

}  // namespace stats
