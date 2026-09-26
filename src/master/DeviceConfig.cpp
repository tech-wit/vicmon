#include "DeviceConfig.h"

#include <Preferences.h>

#include <cstring>

namespace {
// Fixed-size record persisted per device.
struct StoredDevice {
    char name[20];
    uint8_t type;
    uint8_t key[16];
};

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool DeviceConfig::parseHexKey(const String& hex, uint8_t out[16]) {
    if (hex.length() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        int hi = hexNibble(hex[2 * i]);
        int lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

String DeviceConfig::ns() const {
    return profile_ == 0 ? String("vicmon") : "vicmon" + String(profile_);
}

void DeviceConfig::begin(int profile) {
    profile_ = profile;
    load();
    // Deliberately NO seeded devices. A device entry carries a Victron AES key,
    // so anything seeded here would be a real key compiled into every image and
    // written into the NVS of any board that boots it. A fresh unit starts empty;
    // devices are adopted from "Discovered nearby" on the Devices page, pasting
    // each key from VictronConnect.
}

void DeviceConfig::load() {
    Preferences prefs;
    prefs.begin(ns().c_str(), /*readOnly=*/true);
    count_ = prefs.getUChar("ndev", 0);
    if (count_ > kMax) count_ = kMax;
    for (size_t i = 0; i < count_; ++i) {
        char k[8];
        snprintf(k, sizeof(k), "dev%u", static_cast<unsigned>(i));
        StoredDevice rec{};
        slots_[i] = DeviceSlot{};
        if (prefs.getBytes(k, &rec, sizeof(rec)) == sizeof(rec)) {
            strncpy(slots_[i].name, rec.name, sizeof(slots_[i].name) - 1);
            slots_[i].type = static_cast<victron::Record>(rec.type);
            memcpy(slots_[i].key, rec.key, 16);
        }
    }
    prefs.end();
}

void DeviceConfig::save() {
    Preferences prefs;
    prefs.begin(ns().c_str(), /*readOnly=*/false);
    prefs.putUChar("ndev", static_cast<uint8_t>(count_));
    for (size_t i = 0; i < count_; ++i) {
        char k[8];
        snprintf(k, sizeof(k), "dev%u", static_cast<unsigned>(i));
        StoredDevice rec{};
        strncpy(rec.name, slots_[i].name, sizeof(rec.name) - 1);
        rec.type = static_cast<uint8_t>(slots_[i].type);
        memcpy(rec.key, slots_[i].key, 16);
        prefs.putBytes(k, &rec, sizeof(rec));
    }
    prefs.end();
}

bool DeviceConfig::add(const char* name, victron::Record type, const uint8_t key[16]) {
    if (count_ >= kMax || name == nullptr || name[0] == '\0') return false;
    DeviceSlot& s = slots_[count_];
    s = DeviceSlot{};
    strncpy(s.name, name, sizeof(s.name) - 1);
    s.type = type;
    memcpy(s.key, key, 16);
    ++count_;
    return true;
}

bool DeviceConfig::update(size_t idx, const char* name, victron::Record type,
                          const uint8_t* key) {
    if (idx >= count_ || name == nullptr || name[0] == '\0') return false;
    DeviceSlot& s = slots_[idx];
    strncpy(s.name, name, sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = '\0';
    s.type = type;
    if (key != nullptr) memcpy(s.key, key, 16);
    return true;
}

bool DeviceConfig::remove(const char* name) {
    for (size_t i = 0; i < count_; ++i) {
        if (strncmp(slots_[i].name, name, sizeof(slots_[i].name)) == 0) {
            for (size_t j = i; j + 1 < count_; ++j) slots_[j] = slots_[j + 1];
            --count_;
            slots_[count_] = DeviceSlot{};
            return true;
        }
    }
    return false;
}
