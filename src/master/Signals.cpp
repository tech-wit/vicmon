#include "Signals.h"

#include <Preferences.h>

#include <cstring>

namespace sig {

const char* roleKey(Role r) {
    switch (r) {
        case Role::BatterySOC: return "battery_soc";
        case Role::BatteryV: return "battery_v";
        case Role::BatteryA: return "battery_a";
        case Role::BatteryConsumed: return "battery_consumed";
        case Role::BatteryStarterV: return "battery_starter_v";
        case Role::BatteryTTG: return "battery_ttg";
        case Role::SolarA: return "solar_a";
        case Role::SolarW: return "solar_w";
        case Role::ChargerA: return "charger_a";
        case Role::DcDcInA: return "dcdc_in_a";
        case Role::DcDcOutA: return "dcdc_out_a";
        case Role::LoadA: return "load_a";
        default: return "?";
    }
}

const char* roleLabel(Role r) {
    switch (r) {
        case Role::BatterySOC: return "Battery SOC";
        case Role::BatteryV: return "Battery Voltage";
        case Role::BatteryA: return "Battery Current";
        case Role::BatteryConsumed: return "Battery Consumed (Ah)";
        case Role::BatteryStarterV: return "Starter Voltage";
        case Role::BatteryTTG: return "Time to Go";
        case Role::SolarA: return "Solar Current";
        case Role::SolarW: return "Solar Power";
        case Role::ChargerA: return "Charger Current";
        case Role::DcDcInA: return "DC-DC Input Current";
        case Role::DcDcOutA: return "DC-DC Output Current";
        case Role::LoadA: return "Load Current";
        default: return "?";
    }
}

const char* fieldKey(Field f) {
    switch (f) {
        case Field::BattSOC: return "batt_soc";
        case Field::BattV: return "batt_v";
        case Field::BattA: return "batt_a";
        case Field::BattConsumed: return "batt_consumed";
        case Field::BattAuxStarterV: return "batt_aux_starter_v";
        case Field::BattTTG: return "batt_ttg";
        case Field::DcDcInV: return "dcdc_in_v";
        case Field::DcDcOutV: return "dcdc_out_v";
        case Field::DcDcInA: return "dcdc_in_a";
        case Field::DcDcOutA: return "dcdc_out_a";
        case Field::DcDcState: return "dcdc_state";
        case Field::SolarBattV: return "solar_batt_v";
        case Field::SolarBattA: return "solar_batt_a";
        case Field::SolarPvW: return "solar_pv_w";
        case Field::SolarYield: return "solar_yield";
        case Field::SolarLoadA: return "solar_load_a";
        case Field::SolarState: return "solar_state";
        case Field::ChgBattV: return "chg_batt_v";
        case Field::ChgBattA: return "chg_batt_a";
        case Field::ChgState: return "chg_state";
        default: return "none";
    }
}

const char* fieldLabel(Field f) {
    switch (f) {
        case Field::BattSOC: return "SOC (%)";
        case Field::BattV: return "Voltage (V)";
        case Field::BattA: return "Current (A)";
        case Field::BattConsumed: return "Consumed (Ah)";
        case Field::BattAuxStarterV: return "Starter aux (V)";
        case Field::BattTTG: return "Time to go (min)";
        case Field::DcDcInV: return "Input voltage (V)";
        case Field::DcDcOutV: return "Output voltage (V)";
        case Field::DcDcInA: return "Input current (A)";
        case Field::DcDcOutA: return "Output current (A)";
        case Field::DcDcState: return "State";
        case Field::SolarBattV: return "Battery voltage (V)";
        case Field::SolarBattA: return "Battery current (A)";
        case Field::SolarPvW: return "PV power (W)";
        case Field::SolarYield: return "Yield today (kWh)";
        case Field::SolarLoadA: return "Load current (A)";
        case Field::SolarState: return "State";
        case Field::ChgBattV: return "Battery voltage (V)";
        case Field::ChgBattA: return "Battery current (A)";
        case Field::ChgState: return "State";
        default: return "none";
    }
}

size_t fieldsForType(victron::Record type, Field* out, size_t max) {
    static const Field battery[] = {Field::BattSOC, Field::BattV, Field::BattA,
                                    Field::BattConsumed, Field::BattAuxStarterV,
                                    Field::BattTTG};
    static const Field dcdc[] = {Field::DcDcInV, Field::DcDcOutV, Field::DcDcInA,
                                 Field::DcDcOutA, Field::DcDcState};
    static const Field solar[] = {Field::SolarBattV, Field::SolarBattA, Field::SolarPvW,
                                  Field::SolarYield, Field::SolarLoadA, Field::SolarState};
    static const Field charger[] = {Field::ChgBattV, Field::ChgBattA, Field::ChgState};
    const Field* src = nullptr;
    size_t n = 0;
    if (type == victron::Record::BatteryMonitor) { src = battery; n = sizeof(battery) / sizeof(Field); }
    else if (type == victron::Record::OrionXs) { src = dcdc; n = sizeof(dcdc) / sizeof(Field); }
    else if (type == victron::Record::SolarCharger) { src = solar; n = sizeof(solar) / sizeof(Field); }
    else if (type == victron::Record::AcCharger) { src = charger; n = sizeof(charger) / sizeof(Field); }
    if (n > max) n = max;
    for (size_t i = 0; i < n; ++i) out[i] = src[i];
    return n;
}

Resolved resolveField(DeviceSlot* slots, size_t n, const char* device, Field f,
                      uint32_t now) {
    Resolved r;
    if (device == nullptr || device[0] == '\0' || f == Field::None) return r;
    DeviceSlot* s = nullptr;
    for (size_t i = 0; i < n; ++i) {
        if (strncmp(slots[i].name, device, sizeof(slots[i].name)) == 0) { s = &slots[i]; break; }
    }
    if (s == nullptr || s->stale(now)) return r;

    const victron::BatteryData& b = s->battery;
    const victron::DcDcData& d = s->dcdc;
    switch (f) {
        case Field::BattSOC: r = {b.soc, b.socValid}; break;
        case Field::BattV: r = {b.voltage, b.voltageValid}; break;
        case Field::BattA: r = {b.current, b.currentValid}; break;
        case Field::BattConsumed: r = {b.consumedAh, b.consumedValid}; break;
        case Field::BattAuxStarterV: r = {b.auxValue, b.auxValid}; break;
        case Field::BattTTG: r = {static_cast<float>(b.timeToGoMin), b.ttgValid}; break;
        case Field::DcDcInV: r = {d.inputVoltage, d.inputVValid}; break;
        case Field::DcDcOutV: r = {d.outputVoltage, d.outputVValid}; break;
        case Field::DcDcInA: r = {d.inputCurrent, d.inputIValid}; break;
        case Field::DcDcOutA: r = {d.outputCurrent, d.outputIValid}; break;
        case Field::DcDcState: r = {static_cast<float>(d.deviceState), true}; break;
        case Field::SolarBattV: r = {s->solar.batteryVoltage, s->solar.battVValid}; break;
        case Field::SolarBattA: r = {s->solar.batteryCurrent, s->solar.battIValid}; break;
        case Field::SolarPvW: r = {s->solar.pvPower, s->solar.pvValid}; break;
        case Field::SolarYield: r = {s->solar.yieldToday, s->solar.yieldValid}; break;
        case Field::SolarLoadA: r = {s->solar.loadCurrent, s->solar.loadValid}; break;
        case Field::SolarState: r = {static_cast<float>(s->solar.deviceState), true}; break;
        case Field::ChgBattV: r = {s->charger.batteryVoltage, s->charger.battVValid}; break;
        case Field::ChgBattA: r = {s->charger.batteryCurrent, s->charger.battIValid}; break;
        case Field::ChgState: r = {static_cast<float>(s->charger.deviceState), true}; break;
        default: break;
    }
    return r;
}

// ---- SignalMap (NVS) -------------------------------------------------------

String SignalMap::ns() const {
    return profile_ == 0 ? String("vicsig2") : "vicsig2_" + String(profile_);
}

void SignalMap::begin(DeviceSlot* slots, size_t n, int profile) {
    profile_ = profile;
    load();
    bool any = false;
    for (size_t i = 0; i < kRoleCount; ++i) if (b_[i].device[0] != '\0') any = true;
    if (!any) {
        seedDefaults(slots, n);
        save();
    }
}

void SignalMap::load() {
    Preferences prefs;
    prefs.begin(ns().c_str(), /*readOnly=*/true);
    for (size_t i = 0; i < kRoleCount; ++i) {
        char dk[8], fk[8];
        snprintf(dk, sizeof(dk), "d%u", static_cast<unsigned>(i));
        snprintf(fk, sizeof(fk), "f%u", static_cast<unsigned>(i));
        String dev = prefs.getString(dk, "");
        strncpy(b_[i].device, dev.c_str(), sizeof(b_[i].device) - 1);
        b_[i].field = static_cast<Field>(prefs.getUChar(fk, 0));
    }
    prefs.end();
}

void SignalMap::save() {
    Preferences prefs;
    prefs.begin(ns().c_str(), /*readOnly=*/false);
    for (size_t i = 0; i < kRoleCount; ++i) {
        char dk[8], fk[8];
        snprintf(dk, sizeof(dk), "d%u", static_cast<unsigned>(i));
        snprintf(fk, sizeof(fk), "f%u", static_cast<unsigned>(i));
        prefs.putString(dk, b_[i].device);
        prefs.putUChar(fk, static_cast<uint8_t>(b_[i].field));
    }
    prefs.end();
}

void SignalMap::set(Role r, const char* device, Field f) {
    Binding& b = b_[static_cast<size_t>(r)];
    memset(b.device, 0, sizeof(b.device));
    if (device) strncpy(b.device, device, sizeof(b.device) - 1);
    b.field = f;
}

void SignalMap::seedDefaults(DeviceSlot* slots, size_t n) {
    // Auto-bind from the first device of each relevant type.
    const char* bmv = nullptr;
    const char* orion = nullptr;
    const char* solar = nullptr;
    const char* charger = nullptr;
    for (size_t i = 0; i < n; ++i) {
        if (!bmv && slots[i].type == victron::Record::BatteryMonitor) bmv = slots[i].name;
        if (!orion && slots[i].type == victron::Record::OrionXs) orion = slots[i].name;
        if (!solar && slots[i].type == victron::Record::SolarCharger) solar = slots[i].name;
        if (!charger && slots[i].type == victron::Record::AcCharger) charger = slots[i].name;
    }
    if (bmv) {
        set(Role::BatterySOC, bmv, Field::BattSOC);
        set(Role::BatteryV, bmv, Field::BattV);
        set(Role::BatteryA, bmv, Field::BattA);
        set(Role::BatteryConsumed, bmv, Field::BattConsumed);
        set(Role::BatteryStarterV, bmv, Field::BattAuxStarterV);
        set(Role::BatteryTTG, bmv, Field::BattTTG);
    }
    if (orion) {
        set(Role::DcDcInA, orion, Field::DcDcInA);
        set(Role::DcDcOutA, orion, Field::DcDcOutA);
    }
    if (solar) {
        set(Role::SolarA, solar, Field::SolarBattA);
        set(Role::SolarW, solar, Field::SolarPvW);
    }
    if (charger) {
        set(Role::ChargerA, charger, Field::ChgBattA);
    }
    // Load defaults to the derived "load (0 when charging)" signal.
    set(Role::LoadA, kLoadOnly, Field::None);
}

}  // namespace sig
