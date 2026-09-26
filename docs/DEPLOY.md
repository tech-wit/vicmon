# Deploying firmware

Four ways to get firmware onto a unit, cheapest first. Only the last one needs a
PlatformIO checkout.

| Situation | Method | Tools needed |
|---|---|---|
| Unit is paired to another Vicmon that already has the new firmware | **ESP-NOW clone** | none — no cable, no computer |
| Unit is running and you can reach its AP | **Web OTA upload** | a browser |
| Blank board, or a unit you cannot reach | **esptool with a prebuilt image** | `esptool` (or a browser with WebSerial) |
| You are developing | **PlatformIO** | the repo + `.piovenv` |

## 1. Clone over ESP-NOW (no computer)

Flash one unit, then hand the image to the others over the air.

On the unit that already has the new firmware: **Network → System → Firmware clone →
*Send my firmware to the paired device***. The target must have *Allow this device to
be updated remotely* ticked. Or work from the other end: on the out-of-date unit,
***Update this device from the paired device***, which only fetches an image the peer
confirms is newer.

Both are also on the touchscreen (Diag → Firmware). The target reboots into the new
image only if the whole thing validates against its embedded SHA-256, so an
interrupted transfer is harmless — it just keeps running what it had.

## 2. Web OTA upload (a browser)

Join the unit's AP and open **Network → Firmware update (OTA)**. Pick a
`vicmon-<version>-firmware.bin` (the plain app image) and upload. The unit reboots
when it finishes; reload after ~10 s.

This keeps NVS, so devices, keys, profiles, bindings and pairing all survive.

## 3. Prebuilt image with esptool (a blank board)

Built images are committed in **[`releases/`](../releases/)** — `releases/v0.7.32/`
and so on — so there is nothing to build and nothing to fetch from a release server.
Flash them over USB. **No PlatformIO needed.**

```bash
pipx install esptool          # or: pip install --user esptool
```

**A blank or unrecoverable board** — flash the merged full image at offset 0:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 vicmon-0.7.32-esp32s3-full.bin
```

> ⚠️ **The full image erases your configuration.** It spans the NVS partition
> (0x9000–0xe000) and pads it with 0xFF, so device keys, profiles, bindings, pairing
> and the clock all go. Download the config backup JSON first (Network → Backup &
> restore) and restore it afterwards. This is the *first flash / recovery* path, not
> the update path.

**An existing unit you want to keep configured** — write just the app:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x10000 vicmon-0.7.32-firmware.bin
```

That touches only the app partition, so NVS is untouched.

### No install at all: flash from the browser

[esptool-js](https://espressif.github.io/esptool-js/) runs in Chrome or Edge over
WebSerial. Connect the board, add the `-full.bin` at offset `0x0`, and flash. Same
caveat: the full image clears NVS.

The release also ships a `manifest.json` for
[ESP Web Tools](https://esphome.github.io/esp-web-tools/), if you want an
"install" button on a page of your own. It has to be served over HTTPS (or
localhost) next to the `.bin`.

## 4. PlatformIO (development)

```bash
.piovenv/bin/pio run -e s3 -t upload --upload-port /dev/ttyACM0
```

`env:s3` is the only image that matters — one universal build for every ESP32-S3
board, which picks its panel and peripherals at boot.

## Cutting a release

```bash
./tools/release.sh
```

It builds `env:s3`, reads the version straight out of `kFwVersion`, and writes to
`dist/`:

| File | What it is | Flash at |
|---|---|---|
| `vicmon-<ver>-esp32s3-full.bin` | bootloader + partition table + otadata + app, merged | `0x0` — **wipes NVS** |
| `vicmon-<ver>-firmware.bin` | the app image on its own | `0x10000`, or the web OTA, or the ESP-NOW clone |
| `SHA256SUMS` | checksums for both | — |
| `manifest.json` | ESP Web Tools descriptor | — |

The script warns if the working tree is dirty, since the image then matches no commit.

`dist/` is git-ignored, so an experimental build cannot wander into a commit. Adding
`--publish` also copies the four files into `releases/v<version>/`, which **is**
committed — that is what makes the images downloadable straight from the repo.

To publish:

```bash
# bump kFwVersion in src/master/main.cpp first
./tools/release.sh --publish
git add releases/v0.7.32 && git commit -m "Release 0.7.32"
git tag v0.7.32 && git push && git push --tags
```

Each release costs ~3 MB of history, permanently — see
[`releases/README.md`](../releases/README.md). Publish tagged versions, not every
development build. If it gets heavy, attach the same four files to a release on the
Forgejo host instead and stop committing them; only the destination changes.

### Flash layout (8MB, `default_8MB.csv`)

| Offset | Size | Partition |
|---|---|---|
| `0x0000` | — | bootloader |
| `0x8000` | 0x3000 | partition table |
| `0x9000` | 0x5000 | **nvs** — devices, keys, profiles, bindings, pairing, clock |
| `0xe000` | 0x2000 | otadata |
| `0x10000` | 0x330000 | app0 |
| `0x340000` | 0x330000 | app1 (OTA target) |
| `0x670000` | 0x180000 | spiffs/LittleFS — the persisted trend history |
| `0x7f0000` | 0x10000 | coredump |

NVS sits at the same offset across the partition tables in use, which is why config
survives an ordinary reflash. LittleFS is formatted on first mount if it is empty, so
a fresh board needs no filesystem image — it just starts with no history.
