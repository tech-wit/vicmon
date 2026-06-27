# Victron BLE Vehicle Monitor — Implementation Plan

Companion to `PROJECT_SPEC.md`. This plan records the architectural decisions
we've made and breaks the build into ordered, shippable phases.

## Key decisions

| Area | Decision | Why |
|---|---|---|
| Framework | **PlatformIO + Arduino-ESP32** | Best library ecosystem (LVGL, NimBLE), fast to start, right scale for this project. |
| BLE method | **Advertisement decryption only** ("Instant Readout") | No pairing/bonding, scales past the ~4-connection limit, lower power, degrades gracefully when a device leaves range. Covers ~95% of the spec's data points. GATT explicitly deferred. |
| Slave transport | **Both** — WiFi AP + HTTP for config/debug, ESP-NOW for runtime data | AP gives a captive config portal and a debuggable `GET /api/data`; ESP-NOW gives low-latency, low-power live updates without keeping the AP busy. |
| Config storage | Per-device AES keys + settings in **NVS flash**, entered once via setup AP captive portal | Matches spec's offline/no-internet requirement. |

## Hardware

| Role | Board | Notes |
|---|---|---|
| Dev / BLE bring-up | **ESP32 WROOM-32** (on hand) | Classic ESP32; BLE 4.2 + ESP-NOW + WiFi all work. Used for Phases 1–2 before any screen exists. |
| Master | **Guition JC3248W535** (ordered) | ESP32-S3, 16MB flash / 8MB PSRAM, 3.5" 320×480 IPS, capacitive touch. |
| Slave | **LilyGo T-Display-S3** (ordered) | ESP32-S3, 1.9" 320×170 color, two buttons. |
| Test device | **Victron BMV** (on hand) | Primary source for Phase-1 decryption work. |

## Tech stack

- **Arduino-ESP32** core via PlatformIO
- **NimBLE-Arduino** — BLE scanning (much lighter RAM/flash than stock Bluedroid; advertisement-listen only)
- **mbedTLS** (bundled with ESP32) — AES-128-CTR decryption
- **LVGL 9** — master + slave UI
- **ESP-NOW** (esp_now.h) — master→slave live data
- **ESPAsyncWebServer + DNSServer** — captive config portal + HTTP API
- **Preferences** (NVS) — config persistence

## Repository layout

```
vicmon/
├── platformio.ini            # one project, multiple [env:] targets
├── lib/
│   └── victron/              # SHARED, hardware-independent
│       ├── VictronDecrypt.*  # AES-CTR advertisement decryption
│       ├── VictronParser.*   # record-type parsers (shunt/MPPT/dcdc)
│       └── VictronTypes.h    # decoded structs + the wire struct sent over ESP-NOW
├── src/
│   ├── master/
│   │   ├── main.cpp
│   │   ├── BleScanner.*       # NimBLE scan + dispatch to parser
│   │   ├── DeviceRegistry.*   # MAC→{key, last value, staleness}
│   │   ├── Aggregator.*       # derived/system metrics
│   │   ├── ui/                # LVGL screens
│   │   ├── net/EspNowTx.*     # broadcast to slaves
│   │   └── net/ConfigPortal.* # AP + captive portal + GET /api/data
│   └── slave/
│       ├── main.cpp
│       ├── EspNowRx.*
│       ├── WifiPoller.*       # fallback: poll master HTTP
│       └── ui/                # LVGL screens
└── test/                      # native unit tests for decrypt/parse
```

Decryption + parsing live in `lib/victron/` so they can be unit-tested on the
host (`pio test -e native`) with captured advertisement bytes — no hardware in
the loop for the trickiest code.

## Victron advertisement format (the core of Phase 1)

Victron broadcasts a BLE manufacturer-specific advertisement, **company ID
`0x02E1`**. After the company ID the "Extra Manufacturer Data" record is:

```
Offset  Size  Field
0       2     Prefix                 = 0x10 0x00 (product advertisement)
2       2     Model ID               (uint16, little-endian)
4       1     Read-out type
5       2     Nonce / data counter   (uint16 LE) — AES-CTR initial counter
7       1     Key-check byte         — first byte of the AES key
8       N     Encrypted payload      (AES-128-CTR)
```

> ⚠️ Verified against live hardware (2026-06-27). The prefix is **2 bytes**, so
> the key byte is at offset **7** and ciphertext at **8** — an earlier draft of
> this doc had a 1-byte prefix and was off by one. There is **no device-type
> byte**; device type derives from the model ID (we key it off which configured
> key matched).

**Decryption:** AES-128 in **CTR** mode with a **little-endian** counter.
- Key = the 16-byte key from VictronConnect → device → *Product info → Encryption key* (32 hex chars).
- Counter = 128-bit little-endian, initialised to the 16-bit nonce at bytes 5–6;
  increments little-endian per 16-byte block.
- Verify `extra[7] == key[0]` before trusting a decrypt.

**Decoded battery-monitor record (BMV/SmartShunt)** is bit-packed, little-endian.
Fields: time-to-go (min), voltage (0.01 V), alarm reason, aux value + 2-bit aux
mode (starter V / mid-point / temperature / none), current (signed, 0.001 A),
consumed Ah (0.1 Ah), SoC (0.1 %).

> Exact bit offsets for each record type will be taken from the authoritative
> sources rather than hand-rolled:
> - Victron "Extra Manufacturer Data" specification (official PDF)
> - `keshavdv/victron-ble` (Python reference implementation)
> - `wytr/VictronSolarDisplayEsp` (ESP reference from the spec)

## Shared data model (`VictronTypes.h`)

```cpp
struct BatteryData {
  float voltage;        // V
  float current;        // A, +charge / -discharge
  float soc;            // %
  uint16_t timeToGo;    // minutes, 0xFFFF = infinite
  // aux (one of):
  float starterVoltage; float temperature; float midVoltage;
};
struct SolarData  { float batteryV, batteryI, pvPower, yieldToday, loadI; uint8_t state; };
struct DcDcData   { float inputV, outputV, current; uint8_t state; };

// Wire format broadcast over ESP-NOW and mirrored by GET /api/data
struct __attribute__((packed)) MonitorPacket {
  uint8_t  version;
  uint32_t timestamp;
  uint8_t  battery_soc;     // %
  int16_t  battery_current; // 0.1 A
  int16_t  solar_current;   // 0.1 A
  uint8_t  dc_dc_status;    // enum
  uint8_t  flags;           // stale / charging / discharging / idle
};
```

## Phases

### Phase 0 — Scaffold
- PlatformIO project with `[env:wroom]`, `[env:master]`, `[env:slave]`, `[env:native]`.
- `lib/victron/` skeleton + a `native` test target.
- **Done when:** all four envs build empty.

### Phase 1 — BLE decryption core *(on WROOM-32 + BMV — highest risk, first)*
- NimBLE passive scan, filter company ID `0x02E1`.
- AES-CTR decrypt; verify key-byte.
- Parse battery-monitor record → `BatteryData`.
- Print live values to Serial; cross-check against the VictronConnect app.
- Host unit tests with captured advertisement bytes.
- **Done when:** Serial shows correct live SoC/V/current matching the app.

### Phase 2 — Aggregation + WiFi AP / API  *(AP folded in here, was Phase 4)*
- `DeviceRegistry`: per-device {key, type, last value, last-seen} with
  staleness/out-of-range flagging. *(done — `src/master/Registry.h`)*
- Parsers for SmartShunt, SmartSolar MPPT, DC-DC.
- `Aggregator` derives system status (charging / discharging / idle).
- **WiFi AP** + captive portal + `GET /api/data` JSON. *(AP + API + live
  registry done in `src/master/`; runs headless on the AtomS3.)*
- **Config portal -> NVS**: web form to add/list/remove devices (name, type,
  32-hex key); registry loads keys from NVS instead of the static table. *(next)*
- **Done when:** devices configured via the portal, tracked concurrently with
  stale-flagging, and served over the AP.

### Phase 3 — Master display *(Guition board)*
- LVGL bring-up on JC3248W535 (display + capacitive touch driver).
- Dark-theme dashboard: large SoC, signed charge/discharge current, solar input, system status.
- Touch + physical-button navigation between screens.
- **Done when:** live dashboard runs on the master from real BLE data.

### Phase 4 — Slaves & transport
- Master: ESP-NOW broadcast of `MonitorPacket` on each update.
- Slave (T-Display-S3): ESP-NOW receive + LVGL SoC/current screen; HTTP poll
  fallback against the Phase-2 `GET /api/data` if no ESP-NOW for N seconds;
  reconnection logic; button to force reconnect.
- **Done when:** slave mirrors master live; survives master reboot.
- *(WiFi AP + captive portal + HTTP API moved up to Phase 2.)*

### Phase 5 — Vehicle integration (+ optional GATT)
- Mounting, 12/24V→5V supply, vibration check, UI polish.
- *Optional:* GATT add-on for history/settings if advertisement data proves insufficient.

## Setup flow (matches spec)
1. Power on master → creates `Victron-Monitor-Setup` AP.
2. User connects phone/laptop → captive portal.
3. User enters AES key (+ friendly name) per Victron device.
4. Keys saved to NVS; master starts decrypting matching advertisements.
5. Slaves auto-join / receive ESP-NOW from master.

## Open risks / things to confirm during build
- **Bit offsets** of each decoded record — pin against the official spec + `victron-ble` early.
- **Aux mode** of your BMV (starter voltage vs temperature vs mid-point) changes which aux field is valid.
- **ESP-NOW + WiFi-AP coexistence** on the master (shared radio / channel pinning) — validate in Phase 4.
- **LVGL + QSPI display** driver config for the JC3248W535 — community configs exist; budget bring-up time.
