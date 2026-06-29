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
| 4 | Slaves + ESP-NOW transport (LilyGo board) | 🟡 protocol + master broadcaster + slave receive core done; display awaits board |
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
| Victron devices (test) | **BMV/SmartShunt** + **Orion XS 1400 DC-DC** + **SmartSolar MPPT** | BMV, Orion XS & solar decode verified vs VictronConnect. |

## Repository layout (actual)

```
vicmon/
├── platformio.ini          # envs: wroom, atoms3, atoms3-sim, master, slave, native
├── README.md               # usage / quick start
├── PROJECT_SPEC.md         # original brief
├── PROJECT_PLAN.md         # this file
├── lib/victron/            # SHARED, hardware-independent, host-testable
│   ├── tiny_aes.{h,c}      # AES-128 CTR+ECB (passes NIST KAT)
│   ├── BitReader.h         # LSB-first bit unpacking
│   ├── VictronDecrypt.*    # container parse + AES-CTR (little-endian) decrypt
│   ├── VictronParser.*     # record parsers (battery / dcdc / solar / ac-charger)
│   └── VictronTypes.h      # decoded structs + Record/AuxMode enums
├── lib/slavelink/          # SHARED master<->slave ESP-NOW wire format
│   └── SlaveLink.h         # versioned packed Snapshot + enc/dec helpers
├── src/
│   ├── wroom/main.cpp      # Phase-1 reference scanner (Serial output)
│   ├── master/             # headless master (builds for atoms3 + master envs)
│   │   ├── main.cpp        # BLE ingest, signal resolver, history, web app
│   │   ├── Registry.h      # DeviceSlot (key, type, latest values, mac, staleness)
│   │   ├── DeviceConfig.*  # NVS-backed device list (per profile)
│   │   ├── Signals.*       # signal roles, fields, resolver, NVS bindings
│   │   ├── Stats.*         # energy counters / trip stats (Today/Trip/Total)
│   │   └── Profiles.*      # ProfileManager (NVS, up to 4 profiles)
│   └── slave/main.cpp      # ESP-NOW receiver + channel acquisition (serial render; display TBD)
├── test/test_victron/      # native unit tests (pio test -e native)
├── test/test_slavelink/    # native wire-format round-trip tests
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

The two derived halves are passed through a **median-of-3 smoother**
(`updateDerivedSmoothing`, refreshed once per BLE poll) so they don't flicker
when devices advertise asynchronously; measured device readings stay raw.

**Profiles (`Profiles.*`).** Up to 4 named profiles, each with independent
devices, bindings and settings. Switching is immediate: `applyProfile()` reloads
the three config objects and clears runtime caches (registry, history, discovery).

**NVS namespaces** (profile 0 uses the original names for backward-compat;
profile *N*≥1 appends the id):
- devices: `vicmon` / `vicmon<N>`
- bindings: `vicsig2` / `vicsig2_<N>`  (the `2` was a one-time bump when the role enum changed)
- settings: `vicset` / `vicset<N>`  (battery capacity, idle deadband, TZ offset, alert thresholds)
- energy stats: `vicstat` / `vicstat<N>`  (Today/Trip/Total bucket blob)
- profiles index: `vicprof`  ·  WiFi STA creds: `vicwifi` (global, shared across profiles)
- history (LittleFS, not NVS): `/hist<N>.bin` per profile

**History.** A `HistRing` struct (buffer + cap + head/count + interval) with two
instances fed from one read each loop, holding int16 deci-units (−32768 = n/a)
for the 5 currents **plus SoC** (deci-%): **fine** 720 @ 5 s = 1 h, and **coarse**
1440 @ 60 s = 24 h. `GET /api/history?mins=N` (N ≤ 1440) returns the last N
minutes per series, reading the coarse ring when N > 60. The trend chart windows
are 1m/10m/1h/12h/24h with vertical scale marks (1/min ≤10m, 1/10min for 1h,
1/3h for 12h & 24h) and a 10-gradation left current axis; **SoC** is overlaid on
a right-hand 0–100 % axis (labelled every 10 %) as a dashed line, and **legend
entries toggle** each series on/off.

**History persistence (LittleFS).** Both buffers are written to a per-profile
file (`/hist<N>.bin`, versioned, chronological) every 5 min (`kHistSaveMs`) and
on a profile switch; loaded on boot / switch; deleted on profile wipe. There are
no timestamps, so a reboot's downtime gap just isn't drawn — reloaded samples
continue at the "now" edge. ~26 KB/save wear-levelled over the ~1.5 MB FS
partition (`default_8MB.csv`) → decades of flash life. Disabled under sim.

**Alerts + status LED (`buildAlerts`/`updateLed`).** Configurable thresholds
(SoC warn/critical, voltage low/high; 0 disables) plus "device quiet for 60 s"
(debounced past the 15 s display staleness so a few missed adverts don't flap)
raise warn/critical alerts, surfaced in `/api/panel` (`alerts[]`), as a banner
on the mimic, and on the AtomS3 onboard RGB LED (GPIO 35 via `neopixelWrite`):
red = critical, amber = warning, else green/cyan/dim for charging/discharging/idle.

**Config backup/restore.** `GET /api/config/export` downloads every profile
(devices + AES keys, bindings, settings) and the WiFi creds as JSON;
`POST /api/config/import` (raw JSON body, parsed with ArduinoJson) restores the
profiles in the file. UI on the Settings page. Protects the painful-to-re-enter
keys against `erase`/reflash and clones a second unit.

**Simulator (`atoms3-sim` env, `-D VICMON_SIM`).** Synthesizes a battery/solar/
DC-DC with a sped-up day cycle so the whole web UI/charts/stats/alerts can be
developed and demoed on the bench with no Victron device in range.

**Energy counters / trip stats (`Stats.*`).** Integrates the live currents into
Ah/Wh (`A·dt`, `A·V·dt` with battery V as the watt-hour reference) and tracks
extremes (min/max SoC & V, peak solar/load W, peak charge/discharge A, time
charging/discharging) over three scopes:
- **Today** — auto-resets at local midnight via NTP; falls back to "since boot"
  when offline. Rollover keyed off a `yyyymmdd` stamp.
- **Trip** — user-resettable journey/camp meter.
- **Total** — lifetime; reset only on explicit confirm.

Per source it logs solar / DC-DC / charger harvest plus load consumption and net
battery charged/discharged. Buckets are a per-profile NVS blob (`vicstat*`),
written at most every 5 min and on any reset (dt clamped to 30 s to drop the gap
after a stall/clock-jump). NTP is started when WiFi STA is configured; the TZ
offset (settings, minutes from UTC, default +600 = AEST) only shifts the daily
rollover. `GET /api/stats`; `POST /stats/reset?scope=today|trip|total`; the
**Stats** web page renders the three scopes with a toggle + reset.

At each midnight rollover the finished Today bucket is archived into a 14-day
ring of `DayRecord`s (solar/dcdc/charger/load/discharged Wh + SoC min/max), also
in the NVS blob. `/api/stats` returns a `days[]` array and the Stats page draws
a **last-7-days** bar chart (stacked Wh-in by source vs. Wh-out). Needs the NTP
clock — no clock ⇒ no rollover ⇒ no day records.

**Web app** (dark theme, served from flash):
- `/` **Mimic** — SVG energy-flow diagram (solar/charger/dcdc → battery → load,
  flow-coloured animated lines), battery detail (V/A/remaining-Ah/starter/TTG),
  mode banner, and a client-side trend chart (canvas, window 1/10/30/60 min).
  TTG is computed locally from instantaneous current + capacity (settles fast,
  unlike the BMV's multi-minute filter).
- `/devices` — add/edit/delete (key shown), live per-device summary, and
  "Discovered nearby" (adopt with name/MAC/RSSI; configured MACs filtered out).
- `/stats` **Stats** — Today / Trip / Total energy counters (Ah & Wh per
  source + load, net battery), extremes (min/max SoC & V, peaks, time
  charging/discharging), scope toggle and per-scope reset, plus a **last-7-days**
  energy bar chart.
- `/diag` **Diag** — per-device live decoded fields + the raw decrypted
  advertisement bytes (hex) + model id, for verifying parsers vs VictronConnect.
- `/bindings` ("Settings") — Profiles, Signal bindings, System settings
  (capacity, deadband, TZ offset), Alerts (thresholds), WiFi (join a network; AP
  stays up), OTA firmware upload, and Backup & restore.
- The mimic also shows secondary readings: DC-DC **input voltage** and solar
  **PV power + battery voltage** (the SmartSolar advert has no PV-array voltage).
- Reachable as **`http://vicmon.local/`** via mDNS on a joined network.
- API: `GET /api/panel` (incl. `alerts[]`), `GET /api/data` (legacy slave
  snapshot), `GET /api/history`, `GET /api/stats`, `GET /api/diag`,
  `GET /api/config/export`; POST `/add /edit /del /bind /capacity /alerts /wifi
  /stats/reset /profile/* /api/config/import /api/ota` (firmware upload).

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
  the duty moderate and pin the AP to channel 1. Currently 50% (`window 80 /
  interval 160`); 30% (`window 48`) is the very-safe fallback if the AP struggles.
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
- **ESP-NOW recv callback signature.** The installed arduino-esp32 uses the 2.x
  form `void(const uint8_t* mac, const uint8_t* data, int len)` — *not* the 3.x
  `esp_now_recv_info_t*`. Using the 3.x form fails to compile.
- **ESP-NOW channel follows the radio.** Peers must share a channel, and the
  SoftAP channel moves to the STA's channel when the master joins a router. Hence
  the slave hops channels to acquire rather than hard-pinning channel 1.
- **Cached values held 5 min** so the mimic isn't blank on join and the chart
  line is continuous (gaps were null samples from devices not heard at tick time).

## Open items / unverified

- **Orion XS (0x0F) and SolarCharger (0x01) parsers VERIFIED** against real
  hardware — the "4wd" profile runs an Orion XS + solar charger + a second BMV,
  and their values cross-check with VictronConnect (2026-06-28).
- **AcCharger (0x08) parser still UNVERIFIED** — no AC charger to test against
  (the new `/diag` page makes confirming it a 5-minute job once one is on hand).
- **Async advertisement skew** — the derived signals (load/charge from the
  energy balance) momentarily disagree when one contributing device advertises
  before another. Damped with a median-of-3 smoother on the derived outputs
  (adds ~2 polls of lag to a genuine change; single-sample glitches are
  outvoted). Inherent residual skew remains but is no longer visible as flicker.
- Slave firmware is a stub; ESP-NOW not implemented.
- Master LVGL display not started.

## Phases (remaining)

### Phase 3 — Master display *(Guition JC3248W535)*
- LVGL bring-up on the QSPI display + capacitive touch.
- Dark dashboard mirroring the mimic (big SoC, signed current, sources, mode).
- Reuse `Signals`/`DeviceConfig`/`Profiles` — they're display-independent.
- Touch + button navigation. **Risk:** QSPI/LVGL driver config (budget time).

### Phase 4 — Slaves & ESP-NOW

**Done (hardware-free, builds + host-tested):**
- **Wire format** `lib/slavelink/SlaveLink.h` — versioned, packed `Snapshot`
  (~36 B, < 250 B ESP-NOW limit): mode, per-field `valid` bitfield, SoC/V/A,
  solar/charger/dcdc/load currents, starter V, TTG, worst-alert, active profile,
  seq + uptime. Fixed-point (deci/centi) encoders. Shared by master, slave, and
  the native test `test/test_slavelink` (round-trip + size + header/version).
- **Master broadcaster** (`setupEspNow`/`buildSnapshot`/`sendSlaveBroadcast`) —
  inits a broadcast peer (`FF:FF:FF:FF:FF:FF`, channel 0 = follow current) and
  sends the snapshot ~1/s from `loop()`. Connectionless; no pairing; self-healing.
  Runs on the current AtomS3.
- **Slave receive core** (`src/slave/main.cpp`) — ESP-NOW receive, **channel-hop
  acquisition** (sweeps 1–13 until a frame arrives, then locks; resumes sweeping
  if the link goes stale → immune to the AP channel moving when the master joins
  a router), sequence-gap counting, 5 s staleness, and a serial render. Builds
  for the `slave` env (LVGL/TFT deps deferred to the board profile).

**Remaining (needs the LilyGo T-Display-S3):**
- Replace `renderToSerial()` with the TFT/LVGL screen (big SoC, signed current,
  mode-coloured banner, sources, "disconnected" state).
- Optional HTTP poll fallback against `/api/panel`; on-screen link status.
- End-to-end channel-coexistence test with the real board.

### Phase 5 — Vehicle integration (+ optional GATT)
- Mounting, 12/24V→5V supply, vibration test, final polish.
- *Optional:* GATT add-on for history/settings if advertisements prove insufficient.

## Feature backlog (post-hardware / nice-to-have)

- **Alerts/thresholds** (low/crit SoC, low/high V, device offline) ✅ → mimic
  banner + AtomS3 RGB LED. *Still TODO:* high-temp (BMV aux temp) + charge-stalled
  checks; optional buzzer.
- **Config backup/restore** ✅ (export/import JSON of all profiles + keys).
- **Mock/sim mode** ✅ (`atoms3-sim` env) for hardware-free UI development.
- **OTA firmware updates** ✅ (web upload, `/api/ota`, Settings page).
- **mDNS** ✅ (`vicmon.local`).
- **Diag / raw-decode page** ✅ (`/diag`) for parser bring-up.
- **Real timestamps (NTP)** ✅ (daily-stats rollover) **+ persisted history**
  ✅ (LittleFS, per-profile). *Still TODO:* clock-aligned (timestamped) samples
  so reboot downtime gaps are drawn rather than elided.
- **Daily energy counters & min/max**, **trip stats** ✅ (`Stats.*`, `/stats`).
- **WebSocket push** to replace 1 s polling (pairs with the display work).
- **MQTT publish** for the Home profile (Home Assistant); 4WD stays standalone.
- **Web UI PIN**; **voltage-based SoC fallback** when no BMV.
