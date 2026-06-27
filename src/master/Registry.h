#pragma once
#include <Arduino.h>

#include "VictronTypes.h"

// One monitored device: its identity/key plus the latest decoded values and
// when they were last refreshed. (Phase 2 will move the key list into NVS via
// the config portal; for now it is seeded from a static table.)
struct DeviceSlot {
    char name[20] = {0};
    victron::Record type = victron::Record::Unknown;
    uint8_t key[16] = {0};

    bool everSeen = false;
    uint32_t lastSeenMs = 0;
    victron::BatteryData battery;  // valid when type == BatteryMonitor
    victron::DcDcData dcdc;         // valid when type == OrionXs

    // Data is considered stale (device out of range / powered off) if we have
    // not seen a fresh advertisement within this window.
    bool stale(uint32_t now, uint32_t timeoutMs = 15000) const {
        return !everSeen || (now - lastSeenMs) > timeoutMs;
    }
};
