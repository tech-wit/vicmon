#pragma once
#include <Arduino.h>

#include "Registry.h"
#include "VictronTypes.h"

// Persistent (NVS-backed) list of monitored Victron devices. Holds the runtime
// DeviceSlot array the BLE scanner and API read from.
//
// NOTE: mutations (add/remove) happen from the web-server task while the BLE
// task reads the slots; config changes are rare/manual so this is acceptable
// for now. Add a mutex if it proves racy.
class DeviceConfig {
public:
    static constexpr size_t kMax = 8;

    // Load profile `profile` from NVS. Profile 0 (the original namespace) seeds
    // the known BMV + Orion XS when empty; other profiles start empty.
    void begin(int profile);

    size_t count() const { return count_; }
    DeviceSlot* slots() { return slots_; }

    void clear() { count_ = 0; }  // drop all in-memory devices (for import)
    bool add(const char* name, victron::Record type, const uint8_t key[16]);
    // Update device at index. Pass key=nullptr to keep the existing key.
    bool update(size_t idx, const char* name, victron::Record type, const uint8_t* key);
    bool remove(const char* name);
    void save();

    // Parses a 32-hex-char key into 16 bytes. Returns false if malformed.
    static bool parseHexKey(const String& hex, uint8_t out[16]);

private:
    void load();
    String ns() const;

    DeviceSlot slots_[kMax];
    size_t count_ = 0;
    int profile_ = 0;
};
