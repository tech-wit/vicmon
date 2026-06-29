#pragma once
#include <cstdint>

namespace victron {

// Value of the "record type" byte (extra manufacturer data offset 3) that
// identifies the device category / decrypted record layout.
enum class Record : uint8_t {
    Unknown = 0x00,
    SolarCharger = 0x01,
    BatteryMonitor = 0x02,
    Inverter = 0x03,
    DcDcConverter = 0x04,
    SmartLithium = 0x05,
    AcCharger = 0x08,
    OrionXs = 0x0F,
};

// Aux input mode reported by a SmartShunt / BMV.
enum class AuxMode : uint8_t {
    StarterVoltage = 0,
    Midpoint = 1,
    Temperature = 2,
    None = 3,
};

// Decoded BMV-7xx / SmartShunt advertisement. Each *_valid flag is false when
// the device reports the field's "not available" sentinel.
struct BatteryData {
    bool valid = false;

    bool ttgValid = false;
    uint16_t timeToGoMin = 0;  // minutes; 0xFFFF sentinel => ttgValid=false

    bool voltageValid = false;
    float voltage = 0.0f;  // V

    uint16_t alarm = 0;  // alarm reason bitfield

    AuxMode auxMode = AuxMode::None;
    bool auxValid = false;
    float auxValue = 0.0f;  // starter/midpoint -> V, temperature -> degC

    bool currentValid = false;
    float current = 0.0f;  // A, positive = charge, negative = discharge

    bool consumedValid = false;
    float consumedAh = 0.0f;  // Ah, negative (energy taken out)

    bool socValid = false;
    float soc = 0.0f;  // %
};

// Decoded Orion XS DC-DC charger advertisement (record type 0x0F).
// Field order/scaling verified against real hardware (the "4wd" profile's Orion
// XS) — input/output V & A cross-check with VictronConnect (2026-06-28).
struct DcDcData {
    bool valid = false;
    uint8_t deviceState = 0;   // 0=off, 3=bulk, 4=absorption, 5=float, ...
    uint8_t chargerError = 0;
    bool inputVValid = false;
    float inputVoltage = 0.0f;   // V (starter/alternator side)
    bool outputVValid = false;
    float outputVoltage = 0.0f;  // V (house battery side)
    bool inputIValid = false;
    float inputCurrent = 0.0f;   // A
    bool outputIValid = false;
    float outputCurrent = 0.0f;  // A
    uint32_t offReason = 0;
};

// Decoded SmartSolar MPPT advertisement (record type 0x01). Scaling verified
// against real hardware (the "4wd" profile's solar charger, 2026-06-28).
struct SolarData {
    bool valid = false;
    uint8_t deviceState = 0;
    uint8_t chargerError = 0;
    bool battVValid = false;
    float batteryVoltage = 0.0f;  // V
    bool battIValid = false;
    float batteryCurrent = 0.0f;  // A (into battery)
    bool pvValid = false;
    float pvPower = 0.0f;  // W
    bool yieldValid = false;
    float yieldToday = 0.0f;  // kWh
    bool loadValid = false;
    float loadCurrent = 0.0f;  // A
};

// Decoded AC charger advertisement (record type 0x08). Minimal + UNVERIFIED:
// only the primary output is parsed.
struct AcChargerData {
    bool valid = false;
    uint8_t deviceState = 0;
    uint8_t chargerError = 0;
    bool battVValid = false;
    float batteryVoltage = 0.0f;  // V (output 1)
    bool battIValid = false;
    float batteryCurrent = 0.0f;  // A (output 1)
};

}  // namespace victron
