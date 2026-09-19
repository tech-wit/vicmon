#include "Stats.h"

#include <Preferences.h>

#include <cmath>
#include <ctime>

namespace stats {

// Bump if the Bucket layout changes so stale blobs are ignored rather than
// reinterpreted.
static const uint32_t kBlobVer = 1;
static const uint32_t kSaveIntervalMs = 5 * 60 * 1000;  // persist at most every 5 min
static const float kMoveThreshA = 0.05f;                // |current| above this = active
static const float kMaxStepSecs = 30.0f;                // clamp dt over a stall / clock jump

String Stats::ns() const {
    return profile_ == 0 ? String("vicstat") : "vicstat" + String(profile_);
}

void Stats::begin(int profile) {
    profile_ = profile;
    for (int i = 0; i < COUNT; ++i) b_[i] = Bucket();
    for (int i = 0; i < kDays; ++i) days_[i] = DayRecord();
    dayCount_ = 0;
    dayHead_ = 0;
    runSecs_ = 0;
    lastMs_ = 0;
    lastSaveMs_ = 0;
    dirty_ = false;
    load();
}

void Stats::load() {
    Preferences p;
    p.begin(ns().c_str(), true);
    if (p.getUInt("ver", 0) == kBlobVer) {
        Bucket tmp[COUNT];
        size_t got = p.getBytes("buckets", tmp, sizeof(tmp));
        if (got == sizeof(tmp))
            for (int i = 0; i < COUNT; ++i) b_[i] = tmp[i];
    }
    // Daily history is stored under its own keys (independent of the bucket
    // version) so adding it didn't reset anyone's lifetime totals.
    bool gotDays = false;
    {
        DayRecord dtmp[kDays];
        if (p.getBytes("days2", dtmp, sizeof(dtmp)) == sizeof(dtmp)) {
            for (int i = 0; i < kDays; ++i) days_[i] = dtmp[i];
            gotDays = true;
        }
    }
    if (!gotDays) {
        // Upconvert the v1 layout. getBytes is all-or-nothing on size, so simply
        // widening DayRecord would have silently thrown away every archived day;
        // the pre-net columns are copied across and the two new ones stay zero
        // (unknown for days recorded before they existed, not "no charge").
        DayRecordV1 v1[kDays];
        if (p.getBytes("days", v1, sizeof(v1)) == sizeof(v1)) {
            for (int i = 0; i < kDays; ++i) {
                days_[i] = DayRecord();
                days_[i].dayStamp = v1[i].dayStamp;
                days_[i].solarAh = v1[i].solarAh;
                days_[i].dcdcAh = v1[i].dcdcAh;
                days_[i].chargerAh = v1[i].chargerAh;
                days_[i].loadAh = v1[i].loadAh;
                days_[i].socMin = v1[i].socMin;
                days_[i].socMax = v1[i].socMax;
            }
            gotDays = true;
            dirty_ = true;  // rewrite in the new layout on the next persist
        }
    }
    if (gotDays) {
        dayCount_ = p.getUInt("dayn", 0);
        dayHead_ = p.getUInt("dayh", 0);
        if (dayCount_ > kDays) dayCount_ = kDays;
        if (dayHead_ >= kDays) dayHead_ = 0;
    }
    runSecs_ = p.getDouble("runsec", 0);  // run-time odometer (no-clock day boundary)
    p.end();
}

void Stats::save() {
    Preferences p;
    p.begin(ns().c_str(), false);
    p.putUInt("ver", kBlobVer);
    p.putBytes("buckets", b_, sizeof(b_));
    p.putBytes("days2", days_, sizeof(days_));  // v2 layout: + net charged/discharged
    p.putUInt("dayn", dayCount_);
    p.putUInt("dayh", dayHead_);
    p.putDouble("runsec", runSecs_);
    p.end();
    dirty_ = false;
}

void Stats::captureDay(const Bucket& f) {
    if (f.dayStamp == 0) return;  // don't archive an un-stamped bucket
    DayRecord d;
    d.dayStamp = f.dayStamp;
    d.solarAh = f.solarAh;
    d.dcdcAh = f.dcdcAh;
    d.chargerAh = f.chargerAh;
    d.loadAh = f.loadAh;
    d.socMin = f.socMin;
    d.socMax = f.socMax;
    d.chargedAh = (float)f.chargedAh;
    d.dischargedAh = (float)f.dischargedAh;
    days_[dayHead_] = d;
    dayHead_ = (dayHead_ + 1) % kDays;
    if (dayCount_ < (size_t)kDays) ++dayCount_;
}

void Stats::maybePersist(uint32_t nowMs, bool force) {
    if (!dirty_) return;
    if (force || lastSaveMs_ == 0 || nowMs - lastSaveMs_ >= kSaveIntervalMs) {
        save();
        lastSaveMs_ = nowMs;
    }
}

// yyyymmdd from a local unix timestamp (0 if unknown).
static uint32_t dayOf(uint32_t localEpoch) {
    if (localEpoch == 0) return 0;
    time_t t = static_cast<time_t>(localEpoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);  // localEpoch is already TZ-adjusted, so treat as UTC
    return (tmv.tm_year + 1900) * 10000u + (tmv.tm_mon + 1) * 100u + tmv.tm_mday;
}

static void accMin(float& dst, float v) {
    if (isnan(dst) || v < dst) dst = v;
}
static void accMax(float& dst, float v) {
    if (isnan(dst) || v > dst) dst = v;
}

void Stats::reset(Scope sc) {
    b_[sc] = Bucket();
    dirty_ = true;
}

void Stats::update(const Sample& s, uint32_t nowMs, uint32_t localEpoch) {
    // The current day key: a yyyymmdd calendar stamp when a clock (NTP/manual) is
    // available, otherwise the run-time day index (every 24h of running).
    auto dayKey = [&]() -> uint32_t { return localEpoch ? dayOf(localEpoch) : runDay(); };

    // Establish the time base on the first call (no integration yet).
    if (lastMs_ == 0) {
        lastMs_ = nowMs;
        for (int i = 0; i < COUNT; ++i)
            if (b_[i].startEpoch == 0) b_[i].startEpoch = localEpoch;
        b_[TODAY].dayStamp = dayKey();
        return;
    }

    float dt = (nowMs - lastMs_) / 1000.0f;
    lastMs_ = nowMs;
    if (dt <= 0) return;
    if (dt > kMaxStepSecs) dt = kMaxStepSecs;  // ignore the gap after a long stall
    runSecs_ += dt;                            // odometer drives the no-clock rollover
    float h = dt / 3600.0f;

    // Day rollover: archive the finished day and start a fresh TODAY when the day
    // key ticks over — calendar midnight if clocked, else a 24h run-time boundary.
    uint32_t dk = dayKey();
    if (b_[TODAY].dayStamp == 0) {
        b_[TODAY].dayStamp = dk;
    } else if (dk != b_[TODAY].dayStamp) {
        captureDay(b_[TODAY]);
        b_[TODAY] = Bucket();
        b_[TODAY].dayStamp = dk;
        dirty_ = true;
    }

    // Voltage reference for the watt-hour integrals (Wh = A * V * h).
    float vref = s.battVValid ? s.battV : 12.8f;

    for (int i = 0; i < COUNT; ++i) {
        Bucket& b = b_[i];
        if (b.startEpoch == 0) b.startEpoch = localEpoch;
        b.durationSecs += static_cast<uint32_t>(lroundf(dt));

        if (s.solarValid && s.solarA > 0) {
            b.solarAh += s.solarA * h;
            b.solarWh += s.solarA * vref * h;
        }
        if (s.dcdcValid && s.dcdcA > 0) {
            b.dcdcAh += s.dcdcA * h;
            b.dcdcWh += s.dcdcA * vref * h;
        }
        if (s.chargerValid && s.chargerA > 0) {
            b.chargerAh += s.chargerA * h;
            b.chargerWh += s.chargerA * vref * h;
        }
        if (s.loadValid && s.loadA > 0) {
            b.loadAh += s.loadA * h;
            b.loadWh += s.loadA * vref * h;
        }
        if (s.battAValid) {
            if (s.battA > 0) {
                b.chargedAh += s.battA * h;
                b.chargedWh += s.battA * vref * h;
            } else {
                b.dischargedAh += (-s.battA) * h;
                b.dischargedWh += (-s.battA) * vref * h;
            }
            if (s.battA > kMoveThreshA) b.chargeSecs += static_cast<uint32_t>(lroundf(dt));
            else if (s.battA < -kMoveThreshA) b.dischargeSecs += static_cast<uint32_t>(lroundf(dt));
            accMax(b.peakChargeA, s.battA > 0 ? s.battA : 0);
            accMax(b.peakDischargeA, s.battA < 0 ? -s.battA : 0);
        }
        if (s.socValid) {
            accMin(b.socMin, s.soc);
            accMax(b.socMax, s.soc);
        }
        if (s.battVValid) {
            accMin(b.vMin, s.battV);
            accMax(b.vMax, s.battV);
        }
        float solarW = s.solarWValid ? s.solarW : (s.solarValid ? s.solarA * vref : 0);
        if (solarW > b.peakSolarW) b.peakSolarW = solarW;
        float loadW = s.loadValid ? s.loadA * vref : 0;
        if (loadW > b.peakLoadW) b.peakLoadW = loadW;
    }
    dirty_ = true;
}

}  // namespace stats
