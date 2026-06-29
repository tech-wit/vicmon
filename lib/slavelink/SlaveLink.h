#pragma once
#include <stdint.h>
#include <string.h>

// Wire format for the master -> slave live-data link (ESP-NOW broadcast).
//
// The master fills a Snapshot from the same resolved signals the web UI uses and
// broadcasts it ~1/s to FF:FF:FF:FF:FF:FF; any number of slaves listen with no
// pairing. The frame is small (< 250 B, the ESP-NOW limit) and self-healing: a
// dropped frame just means one stale second, so there are no ACKs/retransmits.
//
// Shared by the master firmware, the slave firmware, and the native unit test.
// Bump kVersion whenever the layout changes so a mismatched slave ignores frames
// rather than misreading them.
namespace slavelink {

static const uint8_t kMagic0 = 'V';
static const uint8_t kMagic1 = 'S';
static const uint8_t kVersion = 1;

// Per-field validity (a device may be stale/missing). Mirrors the Snapshot
// fields so a slave knows which numbers to trust vs. show as "--".
enum Valid : uint16_t {
    V_SOC = 1 << 0,
    V_BATTV = 1 << 1,
    V_BATTA = 1 << 2,
    V_SOLAR = 1 << 3,
    V_CHARGER = 1 << 4,
    V_DCDC = 1 << 5,
    V_LOAD = 1 << 6,
    V_TTG = 1 << 7,
    V_STARTERV = 1 << 8,
};

enum Mode : uint8_t {
    M_UNKNOWN = 0,
    M_CHARGING = 1,
    M_DISCHARGING = 2,
    M_IDLE = 3,
};

// Fixed-point encodings keep the frame compact; INT16_MIN marks "n/a" as a
// belt-and-braces companion to the `valid` bitfield.
static const int16_t kNA = -32768;

#pragma pack(push, 1)
struct Snapshot {
    uint8_t magic0;       // 'V'
    uint8_t magic1;       // 'S'
    uint8_t version;      // kVersion
    uint8_t mode;         // Mode
    uint16_t valid;       // Valid bitfield
    uint8_t alertWorst;   // 0 none / 1 warning / 2 critical
    uint8_t profile;      // active profile id (0..3)

    int16_t soc_d;        // SoC, deci-percent (0..1000)
    int16_t battV_cv;     // battery voltage, centivolts
    int16_t battA_da;     // battery current, deci-amps (signed: + charge)
    int16_t solarA_da;    // solar current into battery, deci-amps
    int16_t chargerA_da;  // AC-charger current, deci-amps
    int16_t dcdcA_da;     // DC-DC output current, deci-amps
    int16_t loadA_da;     // load current, deci-amps
    int16_t starterV_cv;  // starter/aux voltage, centivolts
    uint16_t ttg_min;     // time-to-go, minutes (0xFFFF = n/a)

    uint16_t seq;         // increments each broadcast (slave can spot gaps)
    uint32_t uptime_s;    // master uptime, seconds
};
#pragma pack(pop)

// ---- helpers ----------------------------------------------------------------

inline void fillHeader(Snapshot& s) {
    s.magic0 = kMagic0;
    s.magic1 = kMagic1;
    s.version = kVersion;
}
inline bool validHeader(const Snapshot& s) {
    return s.magic0 == kMagic0 && s.magic1 == kMagic1 && s.version == kVersion;
}

// Scalar encode/decode (host-testable, no float libs pulled into the header
// beyond the cast). The +0.5 rounding avoids needing <math.h>.
inline int16_t encDeci(bool valid, float x) {
    if (!valid) return kNA;
    return (int16_t)(x >= 0 ? x * 10.0f + 0.5f : x * 10.0f - 0.5f);
}
inline int16_t encCenti(bool valid, float x) {
    if (!valid) return kNA;
    return (int16_t)(x >= 0 ? x * 100.0f + 0.5f : x * 100.0f - 0.5f);
}
inline float decDeci(int16_t v) { return v == kNA ? 0.0f : v / 10.0f; }
inline float decCenti(int16_t v) { return v == kNA ? 0.0f : v / 100.0f; }

}  // namespace slavelink
