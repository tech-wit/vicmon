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
| 3 | Master display (Guition board) | ✅ done on hardware — Arduino_GFX dashboard, 5 pages, touch nav (LVGL dropped, see below) |
| 4 | Slaves + ESP-NOW transport | ✅ done + verified on hardware — masterId filtering, two-sided pairing, slave config AP; LilyGo display driver still TBD |
| 5 | Vehicle integration (mounting, power, polish) + optional GATT | ⛔ not started |

**One app, one codebase (2026-07-08).** `src/master/` is *the* application, and the
**master/slave role** is chosen at runtime from an NVS flag. Any board can be a
master (BLE scan + WiFi AP + web + ESP-NOW broadcast) or a slave (ESP-NOW receiver
+ its own config AP), switchable from the screen, the AP web page, or serial. The
former standalone `src/slave` was folded in and deleted.

**One universal image (`env:s3`).** All ESP32-S3 boards now build from a single
artifact: the AXS15231B display driver + OPI PSRAM are compiled in and used only
where the hardware is present. A board with PSRAM brings up the panel; any other S3
fails the framebuffer allocation and runs headless — the OPI-PSRAM boot init bails
out gracefully (verified on a no-PSRAM AtomS3). Built on the 8MB partition (fits
every S3; a 16MB board uses its first 8MB), and NVS (config/keys/pairing) sits at a
fixed offset shared across partition tables, so it survives re-provisioning. This
also makes an **ESP-NOW OTA "clone my image to any S3 slave"** path viable — the
old per-board envs (`master`/`atoms3`/`headless`/`lilygo`) are retired in its
favour. (`BOARD_GUITION` still gates the display code; `BOARD_LILYGO` reserved.)

### ESP-NOW master ↔ slave (Phase 4, verified)

- **Filtering:** every frame carries a stable `masterId` (low 3 bytes of the
  factory MAC). A paired slave accepts only its master, so several masters can
  share the air. AP SSID is also per-device (`Vicmon-<mac3>`).
- **Two-sided pairing:** the master opens a 60 s window (Diag/Tune *Pair* button,
  web `/api/pair`, or serial `pair`); the slave adopts it only when the user also
  acts on the slave (button / web / serial), then stores it in NVS.
- **Transmit:** a 250 ms `esp_timer` broadcasts a cached snapshot (~4/s) so the
  rate is independent of the loop's ~2 s BLE scan; the broadcast peer uses
  `ifidx = WIFI_IF_AP` (the master is AP-only — the original STA default sent
  nothing). Wire format `lib/slavelink/SlaveLink.h` (v5; +solar W/V, dc-dc V,
  consumed & capacity Ah, master clock), shared receiver `lib/slavelink/SlaveReceiver.h`.
  A version bump forces both ends onto the same firmware — flash master **and**
  slave together, or the link goes silent (all frame types gate on `kVersion`).
- **Extra frames** (same file, dispatched by length + magic): a low-rate
  **StatsFrame** (`V T`) carries the Today/Trip/Total energy meters + runtime-day
  bars for the slave's Week page **and the master's UTC clock** (`utcNow`, v5) so
  the slave shows real dates/time without its own clock; a **HistReq/HistChunk**
  pair (`V Q` / `V C`) lets a slave *pull* the master's full trend rings on connect.
- **Channel:** the master AP is channel 1; a slave running a config AP is pinned
  to ch1 (a SoftAP can't channel-hop), which matches the offline/no-router setup.
- **Slave UI (display + web):** a slave shows the **same** dashboard/web app as a
  master, sourced from the received frame — the mimic + a Graph. The Graph builds
  live from received frames *and* **pulls the master's full history rings** once
  the link is up (unicast HistReq → paced HistChunk reply, resumable via a
  per-chunk bitmap, ~10 s, with a "syncing NN%" overlay). When the link goes
  stale the slave keeps the **last-known** values (banner shows "STALE") rather
  than blanking. Weekly meters come from the StatsFrame.
  Master-only surfaces are hidden by role: on the LCD the Settings > Tune screen
  shows link status + AP details + brightness/timezone/screen-flip + Pair (no
  profiles / alert tunables), Bind is hidden, and Diag is Link + Switch-to-Master;
  the web nav drops Devices/Diag and the web Settings page shows only the cards a
  slave owns — System (pair / unpair / switch role), its config AP, and OTA — hiding
  profiles, panel-signal bindings, system tunables, alerts, WiFi-join and backup.
  **Stats/Week is shown on the slave** (fed from the
  StatsFrame; its clock-set controls are hidden since the slave takes the master's
  time), and the slave mimic reads capacity from the snapshot so remaining-Ah +
  time-to-full/go match the master. Parity: pair / switch-role / debug are reachable
  from the screen **and** the AP (web `/api/pair|/api/role|/api/debug`, serial `pair`/`role`).

The system also runs **headless** (no display) on a bare ESP32-S3 / AtomS3, in
either role — it scans + serves the web UI, or receives + serves a config AP.

## Key decisions

| Area | Decision | Why |
|---|---|---|
| Framework | **PlatformIO + Arduino-ESP32** | Best library ecosystem (NimBLE, GFX), fast to start. |
| Master display UI | **Arduino_GFX direct-draw** (LVGL dropped) | LVGL works on the AXS15231B panel but its anti-aliased fonts fringe on 16-bit colour (grainy) with no gain over crisp 1-bit GFX fonts, plus an unexplained WiFi-SoftAP heap crash. Direct-draw is simpler, smaller, stable. LVGL kept as the `lvglref` reference env. |
| BLE method | **Advertisement decryption only** ("Instant Readout") | No pairing/bonding, scales past the ~4-connection limit, lower power, degrades gracefully out of range. GATT deferred. |
| Device matching | **By AES key**, not MAC | The key uniquely identifies a device (key-check byte + decrypt). Immune to Victron's resolvable/rotating MACs. MAC is learned for display only. |
| Slave transport | **Both** — WiFi AP + HTTP, and ESP-NOW (Phase 4) | AP gives the config UI + `GET /api/panel`; ESP-NOW will give low-latency slave updates. |
| Config storage | **NVS flash**, per-profile namespaces | Survives reboot and reflash; offline-first. |
| Profiles | **Multiple independent profiles** (Home / 4WD …) | One portable box, multiple rigs; immediate switch. |
| Panel data model | **Signal-binding layer** (logical signals → device fields) | Decouples "what hardware exists" from "what the display shows". |

## Hardware

| Role | Board | Notes |
|---|---|---|
| Dev master (current) | **M5Stack AtomS3 Lite** (ESP32-S3) | Native USB → `/dev/ttyACM0`, more reliable than the WROOM's CP210x. Runs the universal `s3` image (headless — no PSRAM). |
| Original dev board | **ESP32 WROOM-32** | Used for Phase-1 bring-up; dropped off USB mid-session (CP210x). `wroom` env still builds the Phase-1 scanner. |
| Master (ordered) | **Guition JC3248W535** | ESP32-S3, 16MB/8MB PSRAM, 3.5" 320×480 IPS, cap touch. Phase 3. |
| Slave (ordered) | **LilyGo T-Display-S3** | ESP32-S3, 1.9" 320×170, two buttons. Phase 4. |
| Victron devices (test) | **BMV/SmartShunt** + **Orion XS 1400 DC-DC** + **SmartSolar MPPT** | BMV, Orion XS & solar decode verified vs VictronConnect. |

## Repository layout (actual)

```
vicmon/
├── platformio.ini          # envs: s3 (universal S3 image), atoms3-sim, guition, gfxref, lvglref, wroom, native
├── README.md               # usage / quick start
├── PROJECT_SPEC.md         # original brief
├── PROJECT_PLAN.md         # this file
├── lib/victron/            # SHARED, hardware-independent, host-testable
│   ├── tiny_aes.{h,c}      # AES-128 CTR+ECB (passes NIST KAT)
│   ├── BitReader.h         # LSB-first bit unpacking
│   ├── VictronDecrypt.*    # container parse + AES-CTR (little-endian) decrypt
│   ├── VictronParser.*     # record parsers (battery / dcdc / solar / ac-charger)
│   └── VictronTypes.h      # decoded structs + Record/AuxMode enums
├── lib/slavelink/          # SHARED master<->slave ESP-NOW code
│   ├── SlaveLink.h         # packed wire formats: Snapshot(v5) + StatsFrame(+clock) + HistReq/HistChunk
│   └── SlaveReceiver.h     # ESP-NOW rx + acquisition + pairing + history-pull state machine
├── src/
│   ├── wroom/main.cpp      # Phase-1/2 reference scanner (Serial output; wroom/atoms3 envs)
│   ├── master/             # THE app — master OR slave at runtime; display per board flag
│   │   ├── app.h           # shared contract: board seam, shared types, extern state, prototypes
│   │   ├── main.cpp        # setup/loop, role branch, signal resolver, history, stats, alerts, slave role
│   │   ├── web.cpp         # WiFi-AP web app: pages, JSON, handlers, setupServer (+ registry mutex)
│   │   ├── web_assets.h    # embedded HTML/CSS/JS (mimic / energy / diag pages)
│   │   ├── display.cpp     # Guition dashboard glue: display task, DashData collection, touch (VICMON_DISPLAY)
│   │   ├── espnow.cpp      # ESP-NOW broadcaster + StatsFrame + history-pull responder
│   │   ├── ble_ingest.cpp  # BLE scan → decrypt/parse → registry (loop task, under the registry mutex)
│   │   ├── Registry.h      # DeviceSlot (key, type, latest values, mac, staleness)
│   │   ├── DeviceConfig.*  # NVS-backed device list (per profile)
│   │   ├── Signals.*       # signal roles, fields, resolver, NVS bindings
│   │   ├── Stats.*         # energy meters (Today/Trip/Total) + run-time odometer / day records
│   │   └── Profiles.*      # ProfileManager (NVS, up to 4 profiles)
│   ├── guition/main.cpp    # standalone GFX dashboard demo (synthetic data, no WiFi/BLE)
│   ├── gfxref/main.cpp     # known-good AXS15231B reference baseline
│   └── lvglref/main.cpp    # LVGL 9 reference/fallback on the real panel (not shipped)
├── lib/guition/            # master display driver: GuitionDisplay/Touch + the split dashboard
│   ├── GfxDashboard.{h,cpp} # public API + tab bar + page-descriptor registry / dispatch
│   ├── gfx_internal.h      # shared palette / layout / primitives + per-page render decls
│   ├── gfx_common.cpp      # shared primitives (gtext / numOr / modeColor / ttgLabel)
│   └── gfx_{dash,flow,graph,week,settings}.cpp  # one file per page
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
- energy stats: `vicstat` / `vicstat<N>`  (Today/Trip/Total buckets + day records + run-seconds odometer)
- profiles index: `vicprof`  ·  WiFi STA creds: `vicwifi`  (global)
- device role: `vicrole`  ·  slave's paired master id: `vicslave`  ·  custom AP name/pass: `vicap`  (all global)
- screen flip: `vicdisp/flip`  ·  rough clock snapshot (master only): `vicclock/utc`  (both global)
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
- **Today** — resets at the day rollover (see below); user-resettable too.
- **Trip** — user-resettable journey/camp meter.
- **Total** — lifetime; reset only on explicit confirm.

Per source it logs solar / DC-DC / charger harvest plus load consumption and net
battery charged/discharged. Buckets are a per-profile NVS blob (`vicstat*`),
written at most every 5 min and on any reset (dt clamped to 30 s to drop the gap
after a stall/clock-jump).

**Day rollover — no clock required.** A persisted **run-seconds odometer** rolls
Today over every 24 h of run-time (dayStamp = a run-day index), so the history
works with no clock at all. If a clock *is* set — NTP (WiFi STA) or a **manual
time** entered on the AP (HH:MM/AM-PM, or the browser clock; date doesn't matter)
— it rolls at local midnight instead and labels become dates. `currentLocalEpoch`
prefers NTP, then the manual clock, else 0 → run-days. TZ offset shifts the
calendar rollover only.

**Clock persistence + transmission.** The manual clock is RAM-only, so the master
snapshots the current UTC epoch to NVS (`vicclock/utc`) every 60 s (and on any
manual set) and restores it at boot — the time survives reboots to a rough order
(it lags by the save interval + downtime; NTP corrects it exactly if it syncs).
Flash wear is a non-issue: NVS appends rewrites into a 4 KB page (~126 entries)
and only erases on compaction (~1 erase per ~126 writes, wear-levelled) → decades
at this cadence. **Master-only** — a slave never writes the clock; it adopts the
master's `utcNow` from the StatsFrame (re-syncing on >5 s drift), so `saveClock()`
is a no-op off the master role.

Each rollover archives the finished Today bucket into a 14-day ring of
`DayRecord`s (per-source **Ah** + SoC min/max). The **Energy** view (web + TFT
Week tab) shows resettable **Today / Trip / Total** cards — big **net-in / net-out
Ah**, source split, duration — plus a runtime-day Ah bar chart (always a 7-day
frame). Reset per-meter: long-press a card on the TFT, or a button on the AP.
`GET /api/stats` (`run_day`, buckets, `days[]`); `POST /stats/reset?scope=`;
`POST /api/time` (epoch or `h`/`m`).

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
- Master display: firmware complete + soak-tested, but the touch controls
  (Settings profile-switch / tunable -/+, Graph zoom pill) are validated by
  construction, **not yet physically tap-tested** on the panel.
- Display brightness is not persisted across reboots (resets to 100%).

## Phases (remaining)

### Phase 3 — Master display *(Guition JC3248W535)* — ✅ DONE (2026-07-08)
Shipped on hardware. See the `vicmon-guition-display` memory for the full driver
notes, pins, and gotchas. Summary:
- **Arduino_GFX direct-draw dashboard, NOT LVGL.** LVGL was brought up and works
  on this AXS15231B QSPI panel (partial-blit + RGB565 swap), but its anti-aliased
  fonts fringe on the 16-bit panel (grainy) with no upside over crisp 1-bit GFX
  fonts, and it hit a WiFi-SoftAP heap-corruption we never root-caused. Kept only
  as a reference env (`lvglref`). The direct-draw path is simpler, smaller, stable.
- **Driver** in `lib/guition/` (`GuitionDisplay` canvas+PWM backlight, `GuitionTouch`,
  `GfxDashboard`). Full-frame PSRAM canvas; whole-frame `flush()` (QSPI has no
  partial DMA). GPIO 35 = OPI PSRAM pin — must not drive it (status LED disabled).
- **5 pages**, bottom tab bar, touch nav: Dash (mimic), Mimic (energy diagram),
  Graph (SoC + battery-A trend, 1m/10m/1h/12h/24h windows matching the web chart's
  fine/coarse ring selection), Week (7-day stacked energy, mirrors the web day
  chart), Settings (profile switch + brightness/battery-cap/deadband/timezone/
  screen-flip/alert-threshold tunables via touch; WiFi + profile CRUD stay web-only).
- **Threading:** display + touch on a dedicated FreeRTOS task reading a
  mutex-protected `DashData` snapshot the loop publishes, so the ~2s blocking BLE
  scan can't stall touch. Registry/NVS writes are deferred to the loop task.
- Reuses `Signals`/`DeviceConfig`/`Profiles`/`Stats` — display-independent.

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
