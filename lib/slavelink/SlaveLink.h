#pragma once
#include <stdint.h>
#include <string.h>

// Wire format for the master -> slave live-data link (ESP-NOW broadcast).
//
// The master fills a Snapshot from the same resolved signals the web UI uses and
// broadcasts it ~1/s to FF:FF:FF:FF:FF:FF. The frame is small (< 250 B, the
// ESP-NOW limit) and self-healing: a dropped frame just means one stale second,
// so there are no ACKs/retransmits.
//
// Every frame carries the master's stable `masterId` (derived from its factory
// MAC). A slave that has been *paired* to a master only accepts frames whose
// masterId matches its stored one, so several masters can broadcast in the same
// area without cross-talk. Pairing is opt-in on both ends: the master sets the
// F_PAIRING flag while its pairing window is open, and the slave adopts that
// masterId only when the user also presses its pair button (see src/slave).
//
// Shared by the master firmware, the slave firmware, and the native unit test.
// Bump kVersion whenever the layout changes so a mismatched slave ignores frames
// rather than misreading them.
namespace slavelink {

static const uint8_t kMagic0 = 'V';
static const uint8_t kMagic1 = 'S';
static const uint8_t kVersion = 5;  // v2 masterId+flags; v3 solar W/V, dc-dc V, consumed Ah; v4 capacity; v5 clock (StatsFrame.utcNow)

// Frame flags (bitfield in Snapshot.flags).
enum Flags : uint8_t {
    F_PAIRING = 1 << 0,  // master's pairing window is open (invites adoption)
};

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
    V_SOLARW = 1 << 9,     // solar PV power (W)
    V_SOLARV = 1 << 10,    // solar battery-side voltage
    V_DCDCINV = 1 << 11,   // DC-DC input voltage
    V_DCDCOUTV = 1 << 12,  // DC-DC output voltage
    V_CONSUMED = 1 << 13,  // battery consumed Ah
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

    uint32_t masterId;    // stable per-master id (from factory MAC) — for filtering
    uint8_t flags;        // Flags bitfield (F_PAIRING while the window is open)

    int16_t soc_d;        // SoC, deci-percent (0..1000)
    int16_t battV_cv;     // battery voltage, centivolts
    int16_t battA_da;     // battery current, deci-amps (signed: + charge)
    int16_t solarA_da;    // solar current into battery, deci-amps
    int16_t chargerA_da;  // AC-charger current, deci-amps
    int16_t dcdcA_da;     // DC-DC output current, deci-amps
    int16_t loadA_da;     // load current, deci-amps
    int16_t starterV_cv;  // starter/aux voltage, centivolts
    uint16_t ttg_min;     // time-to-go, minutes (0xFFFF = n/a)

    // v3 additions — round out the mimic (solar/dc-dc detail + battery consumed).
    int16_t solarW_w;     // solar PV power, whole watts
    int16_t solarV_cv;    // solar battery-side voltage, centivolts
    int16_t dcdcInV_cv;   // DC-DC input (alternator-side) voltage, centivolts
    int16_t dcdcOutV_cv;  // DC-DC output (house-side) voltage, centivolts
    int16_t consumedAh_da;// battery consumed, deci-amp-hours (signed, usually < 0)
    uint16_t capacityAh;  // v4: configured battery capacity, whole Ah (0 = unknown)

    uint16_t seq;         // increments each broadcast (slave can spot gaps)
    uint32_t uptime_s;    // master uptime, seconds
};

// A second, low-rate frame carrying the 7-day energy history for a slave's Week
// page (won't fit the main Snapshot). Magic 'V','T' distinguishes it from the
// snapshot ('V','S'); the receiver dispatches on length + magic. Broadcast a few
// times a minute — the data changes only at the daily rollover.
static const uint8_t kStatsMagic1 = 'T';
// One resettable energy meter, in whole Ah (integers only — this struct is sent
// packed and float would risk unaligned loads on the receiver).
struct StatMeterW {
    uint32_t inAh, outAh;                        // charged into / discharged from battery
    uint32_t solarAh, dcdcAh, chargerAh, loadAh; // source breakdown + load
    uint32_t durSecs;                            // wall-time accumulating
};
struct StatsFrame {
    uint8_t magic0;       // 'V'
    uint8_t magic1;       // 'T'
    uint8_t version;      // kVersion
    uint8_t clockOk;      // a clock (NTP/manual) is set — date labels vs run-days
    uint32_t masterId;    // filter to our paired master
    uint8_t dayCount;     // number of valid past-day entries (0..7)
    uint8_t pad_[3];
    uint32_t dayNow;      // current day key (run-index or yyyymmdd) for axis labels
    uint32_t utcNow;      // master's current UTC epoch (0 = no clock); slave adopts it as its clock
    StatMeterW today, trip, total;                                       // resettable meters
    uint16_t daySolarAh[7], dayDcdcAh[7], dayChargerAh[7], dayLoadAh[7]; // whole Ah per day
    uint32_t dayStamp[7]; // yyyymmdd (clocked) or run-day index per past-day entry
};

// ---- Graph history sync (on-demand, paced) ---------------------------------
// The slave pulls the master's two trend rings once after boot, then extends
// them from the live Snapshot stream. The request is tiny; the reply is many
// small chunks sent unicast (link-layer ACK/retry) and gently paced so it never
// contends with the BLE scan / AP / live broadcast. Magics 'V','Q' / 'V','C'.
static const uint8_t kHistReqMagic1 = 'Q';
static const uint8_t kHistChunkMagic1 = 'C';
// Ring capacities — MUST match the app's HIST_CAP / HIST2_CAP (fine 1h@5s,
// coarse 24h@60s). Used to size the slave's staging buffers.
static const uint16_t kHistFineMax = 720;
static const uint16_t kHistCoarseMax = 1440;
struct HistPointW { int16_t battery, solar, charger, dcdc, load, soc; };  // == app HistSample

struct HistReq {
    uint8_t magic0, magic1, version, pad_;  // 'V','Q'
    uint32_t masterId;                       // target master (ignored by others)
};
static const int kHistChunkPts = 18;         // 18*12 + 16 hdr = 232 B (< 250)
struct HistChunk {
    uint8_t magic0, magic1, version, ring;   // 'V','C'; ring 0 = fine, 1 = coarse
    uint32_t masterId;
    uint16_t fineTotal, coarseTotal;         // BOTH ring totals in every chunk, so the
                                             // slave knows to wait for coarse even while
                                             // fine (sent first) is still arriving
    uint16_t offset;                         // index of pts[0] within `ring`
    uint8_t count;                           // valid samples in pts[] (<= kHistChunkPts)
    uint8_t pad_;
    HistPointW pts[kHistChunkPts];
};

// ---- Firmware clone over ESP-NOW (OTA push) --------------------------------
// Either device clones its running app image to its paired peer: master->slave
// or slave->master. The master and slave run the SAME universal `s3` binary, so
// the transferred image boots on either board (the display path bails gracefully
// where there's no panel). The source with the newer firmware pushes; the target
// writes it into its OTA partition and reboots only if the whole image validates
// (esp_ota_set_boot_partition checks the appended SHA-256), so a dropped transfer
// is non-destructive.
//
// IMPORTANT: OTA frames carry their OWN protocol version (kOtaProto), NOT the
// telemetry kVersion, and never gate on kVersion. That's what lets a new-firmware
// source push to an old-firmware target across a future telemetry-format bump —
// the whole point of remote update. Keep these four structs frozen; bump kOtaProto
// only on an incompatible OTA-protocol change (which breaks cross-version OTA for
// that one transition).
//
// Discovery is a broadcast announce (like the Snapshot): the source broadcasts
// OtaAnnounce, a target that allows remote update replies unicast OtaAccept (so
// each side learns the other's MAC with no pre-shared roster), then the source
// streams OtaData stop-and-wait (one chunk outstanding, target ACKs each) and the
// target finalizes with OtaCtrl DONE/FAIL. masterId filters to the paired peer in
// both directions.
static const uint8_t kOtaProto = 1;
static const uint8_t kOtaAnnounceMagic1 = 'A';  // source -> broadcast: firmware on offer
static const uint8_t kOtaAcceptMagic1 = 'O';    // target -> source unicast: send it to me
static const uint8_t kOtaDataMagic1 = 'D';      // source -> target unicast: one chunk
static const uint8_t kOtaCtrlMagic1 = 'K';      // target -> source unicast: ack / done / fail
static const uint16_t kOtaChunk = 200;          // payload bytes per OtaData (last may be smaller)

enum OtaCtrlKind : uint8_t {
    OTA_ACK = 0,   // chunk `seq` written OK, send the next
    OTA_DONE = 1,  // whole image received + validated; target is rebooting
    OTA_FAIL = 2,  // validation/write failed; abort
    OTA_BUSY = 3,  // target can't accept right now (already updating)
};

struct OtaAnnounce {
    uint8_t magic0, magic1, otaProto, srcRole;  // 'V','A'; srcRole 0=master 1=slave (UI only)
    uint32_t masterId;      // paired-peer filter (shared id, both directions)
    uint32_t imageSize;     // exact app-image bytes to transfer
    uint16_t chunkTotal;    // number of OtaData chunks (ceil(imageSize/kOtaChunk))
    uint16_t chunkSize;     // == kOtaChunk (full chunk size; last chunk may be smaller)
    uint8_t sha8[8];        // head of the source app_elf_sha256 — target skips if it matches (same build)
};
struct OtaAccept {
    uint8_t magic0, magic1, otaProto, tgtRole;  // 'V','O'
    uint32_t masterId;
};
struct OtaData {
    uint8_t magic0, magic1, otaProto, pad_;  // 'V','D'
    uint32_t masterId;
    uint16_t seq;                            // 0-based chunk index
    uint16_t len;                            // valid bytes in data[] (<= kOtaChunk)
    uint8_t data[kOtaChunk];
};
struct OtaCtrl {
    uint8_t magic0, magic1, otaProto, kind;  // 'V','K'; kind = OtaCtrlKind
    uint32_t masterId;
    uint16_t seq;                            // for OTA_ACK: which chunk was written
    uint16_t pad_;
};
#pragma pack(pop)

inline bool validOta(const void* p, uint8_t magic1) {
    const uint8_t* b = (const uint8_t*)p;
    return b[0] == kMagic0 && b[1] == magic1 && b[2] == kOtaProto;
}

inline void fillStatsHeader(StatsFrame& f) {
    f.magic0 = kMagic0;
    f.magic1 = kStatsMagic1;
    f.version = kVersion;
}
inline bool validStatsHeader(const StatsFrame& f) {
    return f.magic0 == kMagic0 && f.magic1 == kStatsMagic1 && f.version == kVersion;
}

inline void fillHistReq(HistReq& r, uint32_t id) {
    r.magic0 = kMagic0; r.magic1 = kHistReqMagic1; r.version = kVersion; r.pad_ = 0; r.masterId = id;
}
inline bool validHistReq(const HistReq& r) {
    return r.magic0 == kMagic0 && r.magic1 == kHistReqMagic1 && r.version == kVersion;
}
inline void fillHistChunkHdr(HistChunk& c) {
    c.magic0 = kMagic0; c.magic1 = kHistChunkMagic1; c.version = kVersion;
}
inline bool validHistChunk(const HistChunk& c) {
    return c.magic0 == kMagic0 && c.magic1 == kHistChunkMagic1 && c.version == kVersion;
}

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
inline int16_t encWhole(bool valid, float x) {
    if (!valid) return kNA;
    return (int16_t)(x >= 0 ? x + 0.5f : x - 0.5f);
}
inline float decDeci(int16_t v) { return v == kNA ? 0.0f : v / 10.0f; }
inline float decCenti(int16_t v) { return v == kNA ? 0.0f : v / 100.0f; }
inline float decWhole(int16_t v) { return v == kNA ? 0.0f : (float)v; }

}  // namespace slavelink
