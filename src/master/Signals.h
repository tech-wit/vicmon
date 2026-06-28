#pragma once
#include <Arduino.h>

#include "Registry.h"
#include "VictronTypes.h"

// Signal-binding layer: maps logical "panel signals" (the things the mimic /
// display show) onto a specific device + field. Persisted to NVS so the data
// contract for the display is configured once and survives reboots.
namespace sig {

// A bindable field on some device.
enum class Field : uint8_t {
    None = 0,
    BattSOC,
    BattV,
    BattA,
    BattConsumed,
    BattAuxStarterV,
    BattTTG,
    DcDcInV,
    DcDcOutV,
    DcDcInA,
    DcDcOutA,
    DcDcState,
    SolarBattV,
    SolarBattA,
    SolarPvW,
    SolarYield,
    SolarLoadA,
    SolarState,
    ChgBattV,
    ChgBattA,
    ChgState,
};

// Logical panel signals the mimic/display consume.
enum class Role : uint8_t {
    BatterySOC = 0,
    BatteryV,
    BatteryA,
    BatteryConsumed,
    BatteryStarterV,
    BatteryTTG,
    SolarA,
    SolarW,
    ChargerA,
    DcDcInA,
    DcDcOutA,
    LoadA,
    COUNT,
};

// Sentinel device names for computed (not directly measured) signals.
constexpr const char* kDerived = "(derived)";        // load = sources - net battery
constexpr const char* kChargeOnly = "(charge_only)"; // max(0, +battery current)
constexpr const char* kLoadOnly = "(load_only)";     // max(0, -battery current)
constexpr size_t kRoleCount = static_cast<size_t>(Role::COUNT);

const char* roleKey(Role r);    // machine name, e.g. "battery_soc"
const char* roleLabel(Role r);  // human label, e.g. "Battery SOC"
const char* fieldKey(Field f);  // machine name, e.g. "batt_a"
const char* fieldLabel(Field f);

// Fields offered by a given device type (for the bindings dropdowns).
size_t fieldsForType(victron::Record type, Field* out, size_t max);

struct Resolved {
    float value = 0.0f;
    bool valid = false;
};

// Resolve a (device-name, field) against the live registry. Invalid if the
// device is missing, stale, the wrong type, or the field reports "not available".
Resolved resolveField(DeviceSlot* slots, size_t n, const char* device, Field f,
                      uint32_t now);

struct Binding {
    char device[20] = {0};
    Field field = Field::None;
    bool bound() const { return device[0] != '\0' && field != Field::None; }
};

// NVS-backed role -> binding table.
class SignalMap {
public:
    // Load profile `profile` from NVS; if empty, auto-bind sensible defaults.
    void begin(DeviceSlot* slots, size_t n, int profile);

    const Binding& binding(Role r) const { return b_[static_cast<size_t>(r)]; }
    void set(Role r, const char* device, Field f);
    void save();

private:
    void load();
    void seedDefaults(DeviceSlot* slots, size_t n);
    String ns() const;
    Binding b_[kRoleCount];
    int profile_ = 0;
};

}  // namespace sig
