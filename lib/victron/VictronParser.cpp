#include "VictronParser.h"

#include "BitReader.h"

namespace victron {

bool parseBatteryMonitor(const uint8_t* decrypted, size_t len, BatteryData& out) {
    // 118 bits of fields -> 15 bytes minimum.
    if (decrypted == nullptr || len < 15) return false;

    BitReader r(decrypted, len);
    uint16_t ttg = static_cast<uint16_t>(r.readUnsigned(16));
    int32_t voltageRaw = r.readSigned(16);
    uint16_t alarm = static_cast<uint16_t>(r.readUnsigned(16));
    uint16_t aux = static_cast<uint16_t>(r.readUnsigned(16));
    uint8_t auxMode = static_cast<uint8_t>(r.readUnsigned(2));
    int32_t currentRaw = r.readSigned(22);
    uint32_t consumedRaw = r.readUnsigned(20);
    uint16_t soc = static_cast<uint16_t>(r.readUnsigned(10));

    out.valid = true;

    out.ttgValid = (ttg != 0xFFFF);
    out.timeToGoMin = ttg;

    out.voltageValid = (static_cast<uint16_t>(voltageRaw & 0xFFFF) != 0x7FFF);
    out.voltage = voltageRaw * 0.01f;

    out.alarm = alarm;

    out.auxMode = static_cast<AuxMode>(auxMode);
    switch (out.auxMode) {
        case AuxMode::StarterVoltage:
            out.auxValid = (aux != 0x7FFF);
            out.auxValue = static_cast<int16_t>(aux) * 0.01f;  // signed V
            break;
        case AuxMode::Midpoint:
            out.auxValid = (aux != 0xFFFF);
            out.auxValue = aux * 0.01f;  // V
            break;
        case AuxMode::Temperature:
            out.auxValid = (aux != 0xFFFF);
            out.auxValue = aux * 0.01f - 273.15f;  // Kelvin*0.01 -> degC
            break;
        case AuxMode::None:
        default:
            out.auxValid = false;
            out.auxValue = 0.0f;
            break;
    }

    // 22-bit signed sentinel for "not available" is 0x3FFFFF (== -1 raw? no:
    // 0x3FFFFF read as 22-bit unsigned == max; treat as NA).
    out.currentValid = (static_cast<uint32_t>(currentRaw & 0x3FFFFF) != 0x3FFFFF);
    out.current = currentRaw * 0.001f;

    out.consumedValid = (consumedRaw != 0xFFFFF);
    out.consumedAh = -(static_cast<float>(consumedRaw)) * 0.1f;

    out.socValid = (soc != 0x3FF);
    out.soc = soc * 0.1f;

    return true;
}

bool parseOrionXs(const uint8_t* decrypted, size_t len, DcDcData& out) {
    // 8+8 + 16*4 + 32 = 112 bits -> 14 bytes minimum.
    if (decrypted == nullptr || len < 14) return false;

    BitReader r(decrypted, len);
    out.deviceState = static_cast<uint8_t>(r.readUnsigned(8));
    out.chargerError = static_cast<uint8_t>(r.readUnsigned(8));

    uint16_t outV = static_cast<uint16_t>(r.readUnsigned(16));
    int32_t outI = r.readSigned(16);
    uint16_t inV = static_cast<uint16_t>(r.readUnsigned(16));
    int32_t inI = r.readSigned(16);
    out.offReason = r.readUnsigned(32);

    out.outputVValid = (outV != 0xFFFF);
    out.outputVoltage = outV * 0.01f;
    out.outputIValid = (static_cast<uint16_t>(outI & 0xFFFF) != 0x7FFF);
    out.outputCurrent = outI * 0.1f;
    out.inputVValid = (inV != 0xFFFF);
    out.inputVoltage = inV * 0.01f;
    out.inputIValid = (static_cast<uint16_t>(inI & 0xFFFF) != 0x7FFF);
    out.inputCurrent = inI * 0.1f;

    out.valid = true;
    return true;
}

bool parseSolarCharger(const uint8_t* decrypted, size_t len, SolarData& out) {
    // 8+8+16+16+16+16+9 = 89 bits -> 12 bytes.
    if (decrypted == nullptr || len < 12) return false;

    BitReader r(decrypted, len);
    out.deviceState = static_cast<uint8_t>(r.readUnsigned(8));
    out.chargerError = static_cast<uint8_t>(r.readUnsigned(8));
    int32_t bv = r.readSigned(16);
    int32_t bi = r.readSigned(16);
    uint16_t yld = static_cast<uint16_t>(r.readUnsigned(16));
    uint16_t pv = static_cast<uint16_t>(r.readUnsigned(16));
    uint16_t load = static_cast<uint16_t>(r.readUnsigned(9));

    out.battVValid = (static_cast<uint16_t>(bv & 0xFFFF) != 0x7FFF);
    out.batteryVoltage = bv * 0.01f;
    out.battIValid = (static_cast<uint16_t>(bi & 0xFFFF) != 0x7FFF);
    out.batteryCurrent = bi * 0.1f;
    out.yieldValid = (yld != 0xFFFF);
    out.yieldToday = yld * 0.01f;
    out.pvValid = (pv != 0xFFFF);
    out.pvPower = pv;
    out.loadValid = (load != 0x1FF);
    out.loadCurrent = load * 0.1f;
    out.valid = true;
    return true;
}

bool parseAcCharger(const uint8_t* decrypted, size_t len, AcChargerData& out) {
    // device_state u8, charger_error u8, battery_voltage_1 u13, battery_current_1 u11
    // = 40 bits -> 5 bytes minimum.
    if (decrypted == nullptr || len < 5) return false;

    BitReader r(decrypted, len);
    out.deviceState = static_cast<uint8_t>(r.readUnsigned(8));
    out.chargerError = static_cast<uint8_t>(r.readUnsigned(8));
    uint16_t bv = static_cast<uint16_t>(r.readUnsigned(13));
    uint16_t bi = static_cast<uint16_t>(r.readUnsigned(11));

    out.battVValid = (bv != 0x1FFF);
    out.batteryVoltage = bv * 0.01f;
    out.battIValid = (bi != 0x7FF);
    out.batteryCurrent = bi * 0.1f;
    out.valid = true;
    return true;
}

}  // namespace victron
