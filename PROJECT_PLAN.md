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
| 4 | Slaves + ESP-NOW transport | ✅ done + verified on hardware — masterId filtering, two-sided pairing, slave config AP, graph-history sync, **wireless OTA clone** (push/pull, version-aware, auto-reboot); **LilyGo T-Display-S3 display done** (compact 7-page renderer incl. Environment, 2-button nav, one universal image w/ runtime board-detect) |
| — | **M5Stack M5Capsule** (headless peripheral board) | ✅ done + verified — BM8563 RTC clock source, buzzer SoC-critical alarm, microSD daily-CSV history log, power-hold; positive board-detect via the RTC |
| — | **Environment sensing** (Unit ENV Pro / BME688) | ✅ done + verified — temperature / humidity / pressure / gas on Grove Port A, in the history rings, web Environment chart, **Guition LCD Environment page** (2 dual-axis charts) and **LilyGo Environment page** (one pair at a time, B cycles), both sharing the Graph zoom window; SD CSV and the ESP-NOW snapshot; pairing on the Capsule button with a flashing LED |
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
favour. The `s3` image compiles in every backend and picks one at boot from
`detectBoard()`: `BOARD_GUITION` (AXS15231B panel), `BOARD_LILYGO` (ST7789 +
buttons), and `BOARD_M5CAPSULE` (headless — RTC/buzzer/microSD peripherals,
detected via its BM8563 RTC). Any other S3 runs headless.

**Memory (no-PSRAM boards).** The M5Capsule (StampS3, no PSRAM) has ~58 KB free
heap out of the box, which was too little to serve the large Settings/Devices
pages. Fixed by streaming those pages as small chunked pieces (no big contiguous
String, no `send()` copy) and by reclaiming static RAM — the slave-only
history-pull staging (~26 KB) is now heap-allocated only in the slave role, the
AsyncTCP task stack is trimmed 16→10 KB, mDNS is started only in STA mode
(~6 KB), and NimBLE is configured scan-only. Net ≈ 40 KB reclaimed (≈58→96 KB
free). The two remaining giants (WiFi ~55 KB, BLE controller ~48 KB) are fixed by
the precompiled Arduino/IDF core. Live memory is on the web `/diag` page and via
the `mem` / `tasks` serial commands.

**Memory after environment support (fw 0.7.x).** Adding four env channels spent a
good chunk of that reclaimed headroom on the Capsule: the history rings grew ~17 KB
(`HistSample` 12→20 B across 720+1440 samples) and the LCD Environment page's
`DashData` arrays another ~3.8 KB, taking free heap **96 KB → ~65 KB**. Still safe —
the chunked page serving keeps the largest single allocation at 3.8 KB, and
`webtest` confirms both heavy pages still build (worst dip ~37 KB free while
rendering Settings) — but the margin is thinner than the paragraph above implies.
The cheapest ~5.8 KB back would be dropping env from the *fine* (5 s) ring, which it
does not need. Note the slave-role graph staging grew in step (26 KB → 43 KB).

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
  nothing). Wire format `lib/slavelink/SlaveLink.h` (**v6**; v3 +solar W/V, dc-dc V,
  consumed Ah, v4 capacity, v5 master clock, **v6 environment** — temp/humidity/
  pressure/gas + `V_ENV`/`V_ENVGAS` validity bits), shared receiver
  `lib/slavelink/SlaveReceiver.h`. A version bump forces both ends onto the same
  firmware — flash master **and** slave together, or the link goes silent (all
  telemetry frame types gate on `kVersion`). The **OTA-clone frames deliberately do
  NOT**, which is what lets a newer unit push across a telemetry bump to rescue an
  old one — that is how v6 was rolled out to a 0.7.0 Capsule wirelessly.
  `HistSample`/`HistPointW` also grew 12→20 B for v6, so `kHistChunkPts` dropped
  18→11 to keep `HistChunk` under the 250 B ESP-NOW payload limit (the native test
  `test_fits_espnow` now asserts this for **every** frame type, not just Snapshot),
  and the LittleFS history file version went `kHistVer` 2→3 so stale files are
  discarded rather than misread.
- **Extra frames** (same file, dispatched by length + magic): a low-rate
  **StatsFrame** (`V T`) carries the Today/Trip/Total energy meters + runtime-day
  bars for the slave's Week page **and the master's UTC clock** (`utcNow`, v5) so
  the slave shows real dates/time without its own clock; a **HistReq/HistChunk**
  pair (`V Q` / `V C`) lets a slave *pull* the master's full trend rings on connect;
  and an **OTA-clone** quartet (`V A`/`V O`/`V D`/`V K` — announce/accept/data/ctrl,
  in `lib/slavelink/OtaLink.h`) pushes a firmware image master↔slave (see below).
- **Firmware clone (OTA) — verified on hardware (2026-07-11):** either device can
  clone its running app image to the paired peer over ESP-NOW, in **either direction
  of initiation**:
  - **Push** — from the unit that has the new firmware (Settings → System, LCD Diag →
    Firmware, or `POST /api/ota/push`). Discovery is a broadcast announce (no
    pre-shared MAC roster); the target — if its *allow remote update* flag is on and
    it isn't already running that build — replies unicast.
  - **Pull** — from the out-of-date unit (`POST /api/ota/pull`, same LCD screen). It
    broadcasts its version and a paired peer answers **only if strictly newer**, so a
    pull can never fetch a same/older image.
  The source then streams the image stop-and-wait (one chunk ACKed at a time, resend
  on timeout) into the target's OTA partition; the target validates the whole image
  (`Update.end` → `esp_ota_set_boot_partition` checks the appended SHA-256) and then
  **auto-reboots** into it — a dropped transfer is non-destructive (it keeps the old
  firmware). **Version awareness:** newness is compared by the `kFwVersion` string,
  parsed numerically (so `0.4.10` > `0.4.2`) — NOT by build date/time, which
  arduino-esp32 on PlatformIO ships frozen in the precompiled app descriptor
  (identical across builds), so bumping `kFwVersion` is the sole "increment". Each
  unit also broadcasts a periodic **version beacon** (~4 s), so the paired device's
  version + relative age (newer/older/same) show at rest — in the transfer status, on
  the web System card, and the LCD Firmware screen. The OTA frames carry their **own**
  protocol version (`kOtaProto`), independent of the telemetry `kVersion`, so a
  new-firmware source can update an old-firmware target across a telemetry-format bump.
  Works because the master and slave run the **same** universal `s3` binary.
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
| Master w/ display | **Guition JC3248W535** | ESP32-S3, 16MB/8MB PSRAM, 3.5" 320×480 IPS, cap touch. ✅ verified. |
| Display w/ buttons | **LilyGo T-Display-S3** | ESP32-S3, 1.9" 320×170, two buttons. ✅ verified. |
| Headless peripheral | **M5Stack M5Capsule** (StampS3) | ESP32-S3FN8, no PSRAM. BM8563 RTC (I2C 0x51 on SDA=8/SCL=10) as clock source, buzzer GPIO2, microSD SPI (SCK14/MOSI12/MISO39/CS11), power-hold GPIO46, WS2812 GPIO21 (**v1.1: LED power rail GPIO38 must be driven HIGH** or it stays dark and silently ignores every write), button GPIO42 (active LOW). ✅ verified. |
| Environment sensor | **M5Stack Unit ENV Pro** (BME688) | On the Capsule's Grove Port A — `Wire1`, SDA=GPIO13/SCL=GPIO15 @100 kHz, I2C 0x77. Temperature / humidity / pressure / gas resistance. ✅ verified. |
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
│   │   ├── display_lilygo.cpp # LilyGo ST7789 7-page renderer (incl. Environment) + 2-button nav (VICMON_HAS_LILYGO)
│   │   ├── capsule.cpp     # M5Capsule peripherals: RTC, buzzer, microSD log, power-hold, button+LED task (VICMON_HAS_M5CAPSULE)
│   │   ├── env_sensor.cpp  # Unit ENV Pro (BME688) on Grove Port A: non-blocking forced-mode sampling (VICMON_HAS_ENVPRO)
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
│   └── gfx_{dash,flow,graph,env,week,settings}.cpp  # one file per page (env = 2 dual-axis charts)
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

The two derived halves are passed through a **min-over-4-samples** filter
(`updateDerivedSmoothing`, refreshed once per BLE poll) — a value only shows once
every recent sample agrees ("assume zero until stable"), so out-of-step device
adverts don't flicker charge/load; measured device readings stay raw.

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
- **Never use `window.confirm()` (or alert/prompt) in the web UI.** The device
  runs a captive-portal DNS, so a phone joining its AP opens the UI in the OS
  captive-portal WebView (Android CaptivePortalLogin / iOS CNA) — and those
  routinely suppress native dialogs, returning `false` with nothing shown. Every
  confirm-guarded action then *silently did nothing* while unguarded ones worked,
  which is exactly how "Switch to Slave" looked broken. Use the two-step
  `cfm(btn, fn)` helper in `systemCard()` instead: first tap arms + relabels,
  second acts, auto-disarms after 4 s.
- **Capsule v1.1 gates its RGB LED behind GPIO38.** The LED is on GPIO21, but on
  the v1.1 board (Stamp-S3A) an electronic power switch on **GPIO38 must be driven
  HIGH** first or the LED is unpowered and ignores every write — indistinguishable
  by eye from a dead LED, a wrong pin or a broken driver. M5Unified's pin table
  lists GPIO21 and says nothing about the rail; check the board-revision docs.
- **Widening a struct that is also a wire format: grep every brace-init.**
  `HistSample` grew from 6 to 10 channels; a `HistSample s{a,b,c,d,e,f}` in the
  slave's `applyPulledHistory()` kept compiling and silently *value-initialised*
  the four new members to 0, so every synced history point read 0 hPa / 0 °C
  instead of "n/a" and wrecked the chart's auto-scale. A `static_assert` on size
  guards the memcpy path but is blind to a partial aggregate init. Copy whole
  points, don't list fields.
- **Overlapping LCD hit-tests fail silently.** The Diag menu's pinned "Restart"
  button overlapped the 5th menu row on a master, and its hit-test ran first, so
  every tap meant for "Switch to Slave" returned Restart. Fixed by making Restart
  an ordinary row and paginating the menu. When adding a fixed control to a list
  screen, check it against the *longest* list the screen can show.
- **Lay LCD rows out from measured text, not guessed offsets.** The Guition
  Environment header put each live value a fixed 90 px after its label; FreeSans is
  proportional and "Temperature" is ~100 px, so the reading printed over the label
  (0.7.27). `textW()` (getTextBounds) is the shared helper — use it whenever one
  string follows another on a row.
- **A slave's own facts must not depend on hearing its master.** `collectSlaveDash()`
  returned early with no snapshot, and the unit-local fields (firmware version, AP,
  IP, uptime, OTA status, the older/newer-master hint) were filled only after that
  return. A slave paired across wire versions never gets a snapshot, so its LCD
  Firmware page went blank on precisely the screen used to push the fix (0.7.28).
  Fill what this unit knows about itself first, then what the link provides.
- **Every ring a page draws from must be in the backlog sync.** The 60 s env ring
  (`gEnvFine`, what the 1m/10m/1h Environment windows draw) was persisted on the
  master but never sent to slaves, so on a slave those windows filled live at one
  point a minute and were empty for an hour after every boot — while the 5-min ring
  synced fine and 12h/24h looked right. Wire v9 sends it as chunk ring 3 (0.7.30).

## Open items / unverified

- **Orion XS (0x0F) and SolarCharger (0x01) parsers VERIFIED** against real
  hardware — the "4wd" profile runs an Orion XS + solar charger + a second BMV,
  and their values cross-check with VictronConnect (2026-06-28).
- **AcCharger (0x08) parser still UNVERIFIED** — no AC charger to test against
  (the new `/diag` page makes confirming it a 5-minute job once one is on hand).
- **Async advertisement skew** — the derived signals (load/charge from the
  energy balance) momentarily disagree when one contributing device advertises
  before another. Damped by a **min-over-4-samples** filter on the derived outputs
  ("assume zero until every recent sample agrees"), so out-of-step device adverts
  no longer flicker charge/load. Residual skew remains but isn't visible.
- Master display + touch: verified on hardware (the Guition ships as the master
  panel and has been driven live all through Phase 3/4, incl. OTA from the LCD).
- Display brightness **is** persisted (NVS `vicdisp/bright`) — restored on boot.
- **LCD Environment pages rendered but not yet eyeballed** (fw 0.7.2). The data
  is confirmed on both boards (live values + a zeros-free history backlog over
  ESP-NOW) and neither crashes, but the *layouts* have not been looked at —
  particularly the pressure axis labels ("1019.3"), the widest thing on either
  page, in the Guition's 52 px gutter and the LilyGo's 34 px one.
- **Unit ENV Pro is M5Capsule-only by design.** Grove Port A pins differ per
  board (Capsule/StampS3/Dial/DinMeter = SDA 13/SCL 15; CoreS3/AtomS3/Cardputer =
  SDA 2/SCL 1), and anything that isn't a Guition/LilyGo/Capsule is `HW_HEADLESS`
  with no way to know which pair to use. Extending it means probing pin pairs the
  way the sensor address is already probed — cheap, but it means driving I2C on an
  unidentified board.

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

**Done + hardware-verified** — see the full **"ESP-NOW master ↔ slave (Phase 4,
verified)"** section near the top of this doc for the detail. In short: versioned
packed wire format (`lib/slavelink/SlaveLink.h`, Snapshot v5 + StatsFrame +
HistReq/HistChunk + the OTA quartet), a 250 ms timer broadcaster, a shared receiver
with pairing/filtering/acquisition, graph-history sync, the dual-role one-app model,
the slave config AP + full web app, and the **wireless OTA firmware clone**. The
former standalone `src/slave` was folded into the master app and deleted; all S3
boards build the one universal `s3` image. `test/test_slavelink` covers the wire
format on the host.

**LilyGo T-Display-S3 display (P3): ✅ done + hardware-verified 2026-07-15.**
- **Panel bring-up.** ST7789 320×170 on the S3 LCD_CAM 8-bit parallel bus + backlight
  + both buttons proven via the standalone `lilygoref` env (`src/lilygoref/`). Verified
  pins: POWER_ON=15, BL=38, DC=7 CS=6 WR=8 RD=9, D0..D7=39,40,41,42,45,46,47,48, RST=5,
  buttons BOOT=0/KEY=14. Panel: `Arduino_ST7789(bus,5,rot,IPS,170,320,35,0,35,0)` over
  `Arduino_ESP32LCD8`; landscape rot=3.
- **Driver in the app** (`lib/lilygo/`, `src/master/display_lilygo.cpp`). A canvas-buffered
  compact renderer with page parity to the Guition build — Dashboard, Power Flow, Graph,
  Environment, Week, Status, Settings — fed from the same per-role `collectDashForRole()`.
  The Environment page shows ONE channel pair at a time (B cycles temp+humidity /
  pressure+gas, hold-B cycles the shared zoom); 320x170 cannot fit four axis gutters
  legibly, which is why it differs from the Guition's stacked pair of charts. Two-button
  nav (A next/prev via short/long, B page-action/secondary) polled in a dedicated 60Hz
  task so it stays responsive under BLE load. FreeSans/FreeSansBold fonts; charge green /
  discharge red state colouring; shared `ttgLabel`. Settings tab cycles items (B) with
  press-and-hold to select (brightness, 180° flip, pair, role toggle, restart).
- **One universal image + runtime board-detect.** The `s3` env compiles BOTH backends
  (`BOARD_GUITION` + `BOARD_LILYGO`); `detectBoard()` picks the panel at boot by the DC
  level on GPIO4 (Guition I²C-SDA pull-up ~3.3V vs LilyGo battery divider ~2.3V, threshold
  2800mV; NVS "vicboard" override). Keeps OTA-clone working across board types — a cloned
  image drives whichever panel it lands on.

### Phase 5 — Vehicle integration (+ optional GATT)
- Mounting, 12/24V→5V supply, vibration test, final polish.
- *Optional:* GATT add-on for history/settings if advertisements prove insufficient.

## Feature backlog (post-hardware / nice-to-have)

- **Alerts/thresholds** (low/crit SoC, low/high V, device offline) ✅ → mimic
  banner + AtomS3 RGB LED. *Still TODO:* high-temp (BMV aux temp) + charge-stalled
  checks; optional buzzer.
- **Config backup/restore** ✅ (export/import JSON of all profiles + keys).
- **Mock/sim mode** ✅ (`atoms3-sim` env) for hardware-free UI development.
- **OTA firmware updates** ✅ (web upload, `/api/ota`, Settings page) **+ wireless
  master↔slave clone over ESP-NOW** ✅ **hardware-verified** (push/pull, version-aware,
  auto-reboot; `/api/ota/{push,pull}`, LCD Diag → Firmware, `lib/slavelink/OtaLink.h`).
- **mDNS** ✅ (`vicmon.local`).
- **Diag / raw-decode page** ✅ (`/diag`) for parser bring-up.
- **Real timestamps (NTP)** ✅ (daily-stats rollover) **+ persisted history**
  ✅ (LittleFS, per-profile). *Still TODO:* clock-aligned (timestamped) samples
  so reboot downtime gaps are drawn rather than elided.
- **Daily energy counters & min/max**, **trip stats** ✅ (`Stats.*`, `/stats`).
- **WebSocket push** to replace 1 s polling (pairs with the display work).
- **MQTT publish** for the Home profile (Home Assistant); 4WD stays standalone.
- **Web UI PIN**; **voltage-based SoC fallback** when no BMV.
