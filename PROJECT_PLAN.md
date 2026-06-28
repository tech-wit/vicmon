# Victron BLE Vehicle Monitor — Implementation Plan

Companion to `PROJECT_SPEC.md` (the original brief) and `README.md` (usage /
quick start). This document is the design + architecture + roadmap record so
development can resume cleanly when the display/slave hardware arrives.

## Status at a glance

| Phase | What | Status |
|---|---|---|
| 0 | Scaffold (PlatformIO envs, shared lib, tests) | ✅ done |
| 1 | BLE advertisement decryption core | ✅ done, verified on hardware |
| 2 | Aggregation + WiFi AP web app (grew well beyond the original scope) | ✅ done (headless on AtomS3) |
| 3 | Master LVGL display (Guition board) | ⛔ not started — awaiting board |
| 4 | Slaves + ESP-NOW transport (LilyGo board) | ⛔ not started — awaiting board |
| 5 | Vehicle integration (mounting, power, polish) + optional GATT | ⛔ not started |

The whole system currently runs **headless on an M5Stack AtomS3 Lite** acting as
the "master minus display": it scans Victron BLE, aggregates, and serves a full
web UI over its WiFi AP. Phases 3–4 add the physical screens on top of this
already-solid data + config layer.

## Key decisions

| Area | Decision | Why |
|---|---|---|
| Framework | **PlatformIO + Arduino-ESP32** | Best library ecosystem (LVGL, NimBLE), fast to start. |
| BLE method | **Advertisement decryption only** ("Instant Readout") | No pairing/bonding, scales past the ~4-connection limit, lower power, degrades gracefully out of range. GATT deferred. |
| Device matching | **By AES key**, not MAC | The key uniquely identifies a device (key-check byte + decrypt). Immune to Victron's resolvable/rotating MACs. MAC is learned for display only. |
| Slave transport | **Both** — WiFi AP + HTTP, and ESP-NOW (Phase 4) | AP gives the config UI + `GET /api/panel`; ESP-NOW will give low-latency slave updates. |
| Config storage | **NVS flash**, per-profile namespaces | Survives reboot and reflash; offline-first. |
| Profiles | **Multiple independent profiles** (Home / 4WD …) | One portable box, multiple rigs; immediate switch. |
| Panel data model | **Signal-binding layer** (logical signals → device fields) | Decouples "what hardware exists" from "what the display shows". |

## Hardware

| Role | Board | Notes |
|---|---|---|
| Dev master (current) | **M5Stack AtomS3 Lite** (ESP32-S3) | Native USB → `/dev/ttyACM0`, more reliable than the WROOM's CP210x. Runs the headless master firmware (`atoms3` env). |
| Original dev board | **ESP32 WROOM-32** | Used for Phase-1 bring-up; dropped off USB mid-session (CP210x). `wroom` env still builds the Phase-1 scanner. |
| Master (ordered) | **Guition JC3248W535** | ESP32-S3, 16MB/8MB PSRAM, 3.5" 320×480 IPS, cap touch. Phase 3. |
| Slave (ordered) | **LilyGo T-Display-S3** | ESP32-S3, 1.9" 320×170, two buttons. Phase 4. |
| Victron devices (test) | **BMV/SmartShunt** + **Orion XS 1400 DC-DC** | BMV decode verified vs VictronConnect. |

## Repository layout (actual)

```
vicmon/
├── platformio.ini          # envs: wroom, atoms3, master, slave, native
├── README.md               # usage / quick start
├── PROJECT_SPEC.md         # original brief
├── PROJECT_PLAN.md         # this file
├── lib/victron/            # SHARED, hardware-independent, host-testable
│   ├── tiny_aes.{h,c}      # AES-128 CTR+ECB (passes NIST KAT)
│   ├── BitReader.h         # LSB-first bit unpacking
│   ├── VictronDecrypt.*    # container parse + AES-CTR (little-endian) decrypt
│   ├── VictronParser.*     # record parsers (battery / dcdc / solar / ac-charger)
│   └── VictronTypes.h      # decoded structs + Record/AuxMode enums
├── src/
│   ├── wroom/main.cpp      # Phase-1 reference scanner (Serial output)
│   ├── master/             # headless master (builds for atoms3 + master envs)
│   │   ├── main.cpp        # BLE ingest, signal resolver, history, web app
│   │   ├── Registry.h      # DeviceSlot (key, type, latest values, mac, staleness)
│   │   ├── DeviceConfig.*  # NVS-backed device list (per profile)
│   │   ├── Signals.*       # signal roles, fields, resolver, NVS bindings
│   │   └── Profiles.*      # ProfileManager (NVS, up to 4 profiles)
│   └── slave/main.cpp      # stub (Phase 4)
├── test/test_victron/      # native unit tests (pio test -e native)
└── tools/monitor.py        # TTY-less serial reader (pio monitor needs a TTY)
```

> Decryption + parsing live in `lib/victron/` so they're unit-tested on the host
> (`pio test -e native`) with no hardware in the loop.

## Architecture — master firmware

**Data flow:** NimBLE active scan → `ingest()` filters Victron company id `0x02E1`
→ tries each configured device's key (`VictronDecrypt`) → `VictronParser` fills
the matching `DeviceSlot` in `DeviceConfig` → `Signals` resolves logical signals
→ `/api/panel` (mimic) and the history ring buffer consume them. Unmatched
Victron adverts go to a discovery list.

**Signal model (`Signals.*`).** Logical *roles* the UI/display consume
(BatterySOC, BatteryV, BatteryA, BatteryConsumed, BatteryStarterV, BatteryTTG,
SolarA, SolarW, ChargerA, DcDcInA, DcDcOutA, LoadA) are each *bound* to a device
+ field, persisted in NVS. `resolveField()` reads the live registry. Derived
sentinels (energy balance; `bat` = net battery current, `src` = solar + dcdc-out
+ charger):
- `(charge_only)` = `max(0, bat − src)` — charge not explained by measured sources.
- `(load_only)` / `(derived)` = `max(0, src − bat)` — load (battery flow offset by sources).

**Profiles (`Profiles.*`).** Up to 4 named profiles, each with independent
devices, bindings and settings. Switching is immediate: `applyProfile()` reloads
the three config objects and clears runtime caches (registry, history, discovery).

**NVS namespaces** (profile 0 uses the original names for backward-compat;
profile *N*≥1 appends the id):
- devices: `vicmon` / `vicmon<N>`
- bindings: `vicsig2` / `vicsig2_<N>`  (the `2` was a one-time bump when the role enum changed)
- settings: `vicset` / `vicset<N>`  (battery capacity, idle deadband)
- profiles index: `vicprof`  ·  WiFi STA creds: `vicwifi` (global, shared across profiles)

**History.** Server-side ring buffer, 720 samples @ 5 s = 60 min, stored as
int16 deci-amps (−32768 = n/a), sampled every loop regardless of any client.
`GET /api/history?mins=N` returns the last N minutes per series.

**Web app** (dark theme, served from flash):
- `/` **Mimic** — SVG energy-flow diagram (solar/charger/dcdc → battery → load,
  flow-coloured animated lines), battery detail (V/A/remaining-Ah/starter/TTG),
  mode banner, and a client-side trend chart (canvas, window 1/10/30/60 min).
  TTG is computed locally from instantaneous current + capacity (settles fast,
  unlike the BMV's multi-minute filter).
- `/devices` — add/edit/delete (key shown), live per-device summary, and
  "Discovered nearby" (adopt with name/MAC/RSSI; configured MACs filtered out).
- `/bindings` ("Settings") — Profiles, Signal bindings, System settings
  (capacity, deadband), and WiFi (join an existing network; AP always stays up).
- API: `GET /api/panel`, `GET /api/data` (legacy slave snapshot),
  `GET /api/history`; POST `/add /edit /del /bind /capacity /wifi /profile/*`.

## Victron advertisement format (verified on hardware, 2026-06-27)

Manufacturer-specific advertisement, **company ID `0x02E1`**. After the company
ID the "Extra Manufacturer Data" record is:

```
Offset  Size  Field
0       2     Prefix          = 0x10 0x00 (product advertisement) ← 2 bytes!
2       2     Model ID        (uint16, little-endian)
4       1     Read-out type
5       2     Nonce / counter (uint16 LE) — AES-CTR initial counter
7       1     Key-check byte  — first byte of the AES key
8       N     Encrypted payload (AES-128-CTR, LITTLE-ENDIAN counter)
```

- Key = 16-byte key from VictronConnect → device → *Product info → Encryption key*.
- Verify `extra[7] == key[0]` before trusting a decrypt.
- **No device-type byte** — type comes from the model ID; we associate it with
  whichever configured key matched.
- Battery-monitor record (bit-packed, LSB-first): TTG (min), voltage (0.01 V),
  alarm, aux + 2-bit aux mode (starter V / mid / temp / none), current
  (signed 0.001 A), consumed Ah (0.1 Ah), SoC (0.1 %).

Authoritative refs: Victron "Extra Manufacturer Data" PDF; `keshavdv/victron-ble`;
`wytr/VictronSolarDisplayEsp`.

## Design decisions & gotchas (learned the hard way)

- **2-byte prefix / LE counter.** The container prefix is 2 bytes, so the key
  byte is at offset 7 and ciphertext at 8, and the CTR counter is little-endian.
  An early 1-byte/big-endian assumption decoded nothing.
- **WiFi + BLE coexistence.** BLE scan at ~99% duty made the AP unjoinable. Keep
  duty ~30% (`window 48 / interval 160`) and pin the AP to channel 1.
- **Inline SVG self-closing.** Unquoted attributes before `/>` (e.g.
  `stroke-width=3/>`) make the HTML parser swallow the slash so elements nest and
  don't render. Quote SVG attributes and put a space before `/>`.
- **Active scan for names.** Victron puts the friendly name in the scan response,
  not the advertisement — passive scan shows "unnamed". Use `setActiveScan(true)`.
- **AtomS3 native USB.** Opening the CDC port doesn't reset the chip; the boot
  banner is missed. `pio device monitor` needs a TTY — use `tools/monitor.py`.
  Build flags `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1`.
- **NVS persists across reflash.** Only `pio run -t erase` wipes config. Profiles
  /devices/settings survive firmware updates.
- **Cached values held 5 min** so the mimic isn't blank on join and the chart
  line is continuous (gaps were null samples from devices not heard at tick time).

## Open items / unverified

- **Orion XS input/output current scaling is UNVERIFIED** — engine-off shows ~0
  input (consistent). Needs an **engine-running reading** to confirm scale, then
  the energy balance / derived load tightens up.
- **SolarCharger (0x01) and AcCharger (0x08) parsers are best-effort/UNVERIFIED**
  — no hardware to test against yet.
- Slave firmware is a stub; ESP-NOW not implemented.
- Master LVGL display not started.

## Phases (remaining)

### Phase 3 — Master display *(Guition JC3248W535)*
- LVGL bring-up on the QSPI display + capacitive touch.
- Dark dashboard mirroring the mimic (big SoC, signed current, sources, mode).
- Reuse `Signals`/`DeviceConfig`/`Profiles` — they're display-independent.
- Touch + button navigation. **Risk:** QSPI/LVGL driver config (budget time).

### Phase 4 — Slaves & ESP-NOW
- Master: ESP-NOW broadcast of a packed snapshot on each update; consider a
  **WebSocket** push for the local UI/display while here.
- Slave (T-Display-S3): ESP-NOW receive + small SoC/current screen; HTTP poll
  fallback against `/api/panel`; reconnection; force-reconnect button.
- Validate ESP-NOW + WiFi-AP coexistence (channel pinning).

### Phase 5 — Vehicle integration (+ optional GATT)
- Mounting, 12/24V→5V supply, vibration test, final polish.
- *Optional:* GATT add-on for history/settings if advertisements prove insufficient.

## Feature backlog (post-hardware / nice-to-have)

- **Alerts/thresholds** (low SoC, low/high V, high temp, charge stalled) → mimic
  red + AtomS3 RGB LED + optional buzzer.
- **OTA firmware updates** (web upload / ArduinoOTA) — flash once mounted.
- **mDNS** (`vicmon.local`) on STA WiFi.
- **Real timestamps (NTP) + persisted history** (LittleFS) for longer, clock-aligned charts.
- **Daily energy counters & min/max**, **trip stats**.
- **WebSocket push** to replace 1 s polling (pairs with the display work).
- **MQTT publish** for the Home profile (Home Assistant); 4WD stays standalone.
- **Web UI PIN**; **voltage-based SoC fallback** when no BMV.
