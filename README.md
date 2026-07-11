# Vicmon — Victron BLE Vehicle Monitor

A standalone ESP32 system that monitors Victron Smart Bluetooth devices (battery
monitors, DC-DC chargers, solar chargers) in a vehicle and presents them on a
live web dashboard — no Home Assistant, no internet, no Bluetooth pairing.

It reads Victron's encrypted **"Instant Readout"** BLE advertisements, decrypts
them locally with each device's key, aggregates everything, and serves a dark
**energy-flow "mimic"** UI plus a configurable signal/profile system over its own
WiFi access point.

> **Status:** running on hardware end to end. The **Guition JC3248W535** is the
> touch master (BLE + WiFi AP + web app + an on-screen dashboard), and **ESP-NOW
> slave displays** mirror it. One firmware runs on every board; master vs slave is
> a runtime NVS flag. See `PROJECT_PLAN.md` for the architecture and `PROJECT_SPEC.md`
> for the original brief.

## What works now

- Decrypts & parses Victron advertisements (BMV/SmartShunt, Orion XS DC-DC and SmartSolar MPPT all verified vs VictronConnect; AC charger parser present but unverified).
- **Mimic dashboard** — battery centre with SoC fill, solar/charger/DC-DC source nodes and a load, animated flow lines coloured by charge/discharge, battery detail (V, A, remaining Ah, starter V, time-to-go) and a mode banner.
- **Trend chart** — server-logged history (continuous, survives client disconnects **and reboots** via LittleFS) with 1m/10m/1h/12h/24h windows and per-window scale marks (fine 5 s/1 h + coarse 60 s/24 h buffers). SoC is overlaid on a right-hand 0–100 % axis; click legend entries to show/hide each series.
- **Alerts** — configurable low/critical SoC and low/high voltage thresholds plus device-offline detection; shown as a mimic banner and on the onboard RGB LED (red/amber/green).
- **Config backup/restore** — download all profiles (devices, keys, bindings, settings) as JSON and restore from one; protects keys against erase/reflash and clones a second unit.
- **Simulator build** (`atoms3-sim`) — synthetic battery/solar/DC-DC so the whole UI can be developed without any Victron device.
- **Energy counters & trip stats** — Today / Trip / Total amp-hours & watt-hours per source (solar, DC-DC, charger) plus load and net battery, with min/max SoC & voltage, peak power and time charging/discharging. Today auto-resets at local midnight (NTP); Trip and Total reset on demand. Persisted in NVS, with a **last-7-days** energy bar chart.
- **Devices** — add / edit / delete by AES key; live per-device summary; "Discovered nearby" list (with Bluetooth name, MAC, RSSI) to adopt new devices.
- **Signals** — bind logical panel signals (battery SoC/V/A, solar, charger, DC-DC, load) to device fields, including **derived** charge/load from the energy balance.
- **Profiles** — multiple independent setups (e.g. Home vs 4WD), switched instantly.
- **Diagnostics** — `/diag` page shows each device's live decoded fields plus the raw decrypted advertisement bytes, for verifying parsers against VictronConnect.
- **OTA updates** — flash a new `firmware.bin` over WiFi from the Settings page.
- **WiFi** — always runs its AP; can also join an existing network, reachable at `vicmon.local` (mDNS).
- Config persists in NVS (survives reboot **and** reflash).

## Hardware

- **Now:** M5Stack AtomS3 Lite (ESP32-S3, native USB) as the headless master.
- **Coming:** Guition JC3248W535 (3.5" touch master display), LilyGo T-Display-S3 (slave).
- Any Victron device with **"Instant readout via Bluetooth" enabled** in VictronConnect.

## Quick start

PlatformIO is used via a project virtualenv (`.piovenv/`, git-ignored):

```bash
python3 -m venv .piovenv && .piovenv/bin/pip install platformio   # first time

# host unit tests (decrypt/parse) — no hardware needed
.piovenv/bin/pio test -e native

# build + flash the headless master to the AtomS3 (native USB → /dev/ttyACM0)
.piovenv/bin/pio run -e atoms3 -t upload --upload-port /dev/ttyACM0

# read the serial log (pio's own monitor needs an interactive TTY)
.piovenv/bin/python tools/monitor.py --seconds 20
```

One app (`src/master/`) builds for every board; the display driver is picked per
board at build time, the **master/slave role at runtime** (NVS flag — switch it
from the screen, the AP web page, or serial `role`). Build envs:
`master` (Guition, touch display), `headless` (bare ESP32-S3, no display),
`atoms3` (M5Stack AtomS3 headless), `lilygo` (T-Display-S3, display driver TBD —
builds headless), `guition` (bench dashboard demo), `wroom` (Phase-1 scanner),
`native` (host tests).

**Slaves (ESP-NOW):** a slave receives the master's ~4/s broadcast and shows it on
its screen (or serial, if headless) plus its own config AP. Pairing is two-sided:
open the master's 60 s window (Diag/Tune *Pair*, or web/serial `pair`), then adopt
on the slave (button / web / serial). A paired slave filters to its master's id,
so several masters can coexist.

## Using it

1. Power the master. It starts a WiFi AP **`Vicmon-Master`** (password `vicmon1234`).
2. Join that network and open **`http://192.168.4.1/`** (a captive-portal prompt usually pops up).
3. Go to **Settings → Profiles**, create/name a profile (e.g. "4WD").
4. Go to **Devices → Add device**: enter a name, type, and the 32-hex **encryption key**.
   - Get the key from **VictronConnect → the device → ⚙ → Product info →
     Encryption data**. A key only ever decrypts its own device, so there's no
     ambiguity about which device it belongs to.
   - Or use **Discovered nearby** to see Victron devices in range (name/MAC/RSSI)
     and pre-fill the form.
5. The **Mimic** comes alive as devices are heard. Tune **Settings → System
   settings** (battery capacity for remaining-Ah and time-to-full; idle deadband).

## How it works (short version)

Victron broadcasts AES-128-CTR-encrypted advertisements (company id `0x02E1`).
The master listens (active scan), decrypts each with the matching device key,
parses the bit-packed record, and stores the latest values per device. A
**signal-binding** layer maps those device fields onto logical panel signals the
UI consumes via `GET /api/panel`; a ring buffer feeds `GET /api/history`. The
container/decryption details and the full module architecture are documented in
`PROJECT_PLAN.md`.

## Notes

- **No data without a key.** Only the advertisement *header* (model id, name,
  MAC, signal strength) is cleartext; all readings are AES-encrypted.
- **Config survives reflash** — only a deliberate `pio run -t erase` clears NVS.
- **Matching is by key, not MAC** — robust against Victron's rotating addresses.

## Credits / references

- Victron "Extra Manufacturer Data" specification (advertisement format)
- [`keshavdv/victron-ble`](https://github.com/keshavdv/victron-ble) — Python reference
- [`wytr/VictronSolarDisplayEsp`](https://github.com/wytr/VictronSolarDisplayEsp) — ESP reference
- AES from the public-domain [`kokke/tiny-AES-c`](https://github.com/kokke/tiny-AES-c)
