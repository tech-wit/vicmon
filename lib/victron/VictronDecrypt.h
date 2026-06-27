#pragma once
#include <cstddef>
#include <cstdint>

namespace victron {

// Victron Energy BLE company identifier (little-endian on the wire: E1 02).
constexpr uint16_t kCompanyId = 0x02E1;

// "Extra manufacturer data" layout (the bytes AFTER the 2-byte company id),
// matching the victron-ble reference container:
//   [0..1] prefix          (0x10 0x00 — product advertisement, 2 bytes)
//   [2..3] model id (LE)
//   [4]    readout type
//   [5..6] IV / data counter (LE) -> AES-CTR little-endian initial counter
//   [7]    key-check byte  (first byte of the AES key)
//   [8..]  AES-128-CTR encrypted payload (little-endian counter)
//
// Device type is NOT encoded here; it derives from the model id (or, for us,
// from which configured key matched).

// Returns the model id (offset 2..3, LE). Caller must ensure extraLen >= 4.
inline uint16_t modelId(const uint8_t* extra) {
    return static_cast<uint16_t>(extra[2]) | (static_cast<uint16_t>(extra[3]) << 8);
}

// Decrypts the encrypted payload of an "extra manufacturer data" buffer.
// `key` is the 16-byte per-device key from VictronConnect.
// Writes up to outCap decrypted bytes into `out`.
// Returns the number of decrypted bytes, or -1 on error (too short, bad prefix,
// or key mismatch — i.e. this advertisement isn't for this key).
int decrypt(const uint8_t* extra, size_t extraLen, const uint8_t key[16],
            uint8_t* out, size_t outCap);

}  // namespace victron
