#pragma once
#include <cstddef>
#include <cstdint>

#include "VictronTypes.h"

namespace victron {

// Parses a decrypted Battery Monitor (BMV / SmartShunt, record type 0x02)
// payload. Field order/scaling follows Victron's "Extra Manufacturer Data"
// spec and the `victron-ble` reference. Returns false if the buffer is too
// short. Bit layout (LSB-first):
//   ttg          u16  minutes
//   voltage      s16  0.01 V
//   alarm        u16  bitfield
//   aux          u16  starter -> 0.01 V (s16) / temp -> 0.01 K / mid -> 0.01 V
//   aux_mode     u2   see victron::AuxMode
//   current      s22  0.001 A
//   consumed_ah  u20  0.1 Ah (reported negative)
//   soc          u10  0.1 %
bool parseBatteryMonitor(const uint8_t* decrypted, size_t len, BatteryData& out);

// Parses a decrypted Orion XS DC-DC charger (record type 0x0F) payload.
// UNVERIFIED layout (LSB-first) — confirm against hardware before trusting:
//   device_state   u8
//   charger_error  u8
//   output_voltage u16  0.01 V
//   output_current s16  0.1 A
//   input_voltage  u16  0.01 V
//   input_current  s16  0.1 A
//   off_reason     u32
bool parseOrionXs(const uint8_t* decrypted, size_t len, DcDcData& out);

}  // namespace victron
