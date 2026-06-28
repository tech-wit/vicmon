# Vicmon — Victron BLE Vehicle Monitor

A standalone ESP32 system that monitors Victron Smart Bluetooth devices (battery
monitors, DC-DC chargers, solar chargers) in a vehicle and presents them on a
live web dashboard — no Home Assistant, no internet, no Bluetooth pairing.

It reads Victron's encrypted **"Instant Readout"** BLE advertisements, decrypts
them locally with each device's key, aggregates everything, and serves a dark
**energy-flow "mimic"** UI plus a configurable signal/profile system over its own
WiFi access point.

> **Status:** the full master logic + web app run **headless on an M5Stack
> AtomS3 Lite** today (Phases 1–2). The physical LVGL display and ESP-NOW slaves
> (Phases 3–4) are pending their boards. See `PROJECT_PLAN.md` for the roadmap
> and architecture, and `PROJECT_SPEC.md` for the original brief.

## What works now

- Decrypts & parses Victron advertisements (BMV/SmartShunt verified vs VictronConnect; Orion XS DC-DC, MPPT, AC charger parsers present but scaling unverified).
- **Mimic dashboard** — battery centre with SoC fill, solar/charger/DC-DC source nodes and a load, animated flow lines coloured by charge/discharge, battery detail (V, A, remaining Ah, starter V, time-to-go) and a mode banner.
- **Trend chart** — server-logged history (continuous, survives client disconnects) with 1/10/30/60-minute windows.
- **Devices** — add / edit / delete by AES key; live per-device summary; "Discovered nearby" list (with Bluetooth name, MAC, RSSI) to adopt new devices.
- **Signals** — bind logical panel signals (battery SoC/V/A, solar, charger, DC-DC, load) to device fields, including **derived** charge/load from the energy balance.
- **Profiles** — multiple independent setups (e.g. Home vs 4WD), switched instantly.
- **WiFi** — always runs its AP; can also join an existing network.
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

Build envs: `atoms3` (headless master, current dev target), `master` (Guition,
Phase 3), `slave` (Phase 4 stub), `wroom` (Phase-1 reference scanner), `native`
(host tests).

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
