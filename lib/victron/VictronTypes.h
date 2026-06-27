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
// NOTE: field order/scaling below is the best reconstruction from the
// `victron-ble` reference and is UNVERIFIED against real hardware — confirm
// against a live hex dump + VictronConnect readings during Phase 1 bring-up.
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

}  // namespace victron
