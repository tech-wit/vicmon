#!/usr/bin/env bash
# Package the universal `s3` image for deployment on machines with no PlatformIO
# checkout — see docs/DEPLOY.md for what to do with the output.
#
# Produces, in dist/:
#   vicmon-<ver>-esp32s3-full.bin  merged bootloader+table+otadata+app, flash at 0x0.
#                                  Spans the NVS partition (0x9000..0xe000), so it
#                                  WIPES config/keys — blank boards and recovery only.
#   vicmon-<ver>-firmware.bin      app image alone: the web UI's OTA upload, the
#                                  ESP-NOW clone, or esptool at 0x10000. Keeps NVS.
#   SHA256SUMS                     checksums for both.
#   manifest.json                  ESP Web Tools descriptor for the full image.
set -euo pipefail
cd "$(dirname "$0")/.."

PIO=${PIO:-.piovenv/bin/pio}
PY=${PY:-.piovenv/bin/python}
CORE=${PLATFORMIO_CORE_DIR:-$HOME/.platformio}
ESPTOOL=${ESPTOOL:-$CORE/packages/tool-esptoolpy/esptool.py}
BOOT_APP0=${BOOT_APP0:-$CORE/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin}
BUILD=.pio/build/s3
OUT=dist

for f in "$PIO" "$PY" "$ESPTOOL" "$BOOT_APP0"; do
    [ -e "$f" ] || { echo "release: missing $f (set PIO/PY/ESPTOOL/BOOT_APP0 or PLATFORMIO_CORE_DIR)" >&2; exit 1; }
done

VER=$(sed -n 's/.*kFwVersion = "\([^"]*\)".*/\1/p' src/master/main.cpp)
[ -n "$VER" ] || { echo "release: could not read kFwVersion from src/master/main.cpp" >&2; exit 1; }
GITDESC=$(git describe --always --dirty 2>/dev/null || echo "no-git")
case "$GITDESC" in
    *-dirty) echo "release: WARNING working tree is dirty ($GITDESC) — the image will not match any commit" >&2 ;;
esac

echo "release: building $VER ($GITDESC)"
"$PIO" run -e s3

mkdir -p "$OUT"
FULL="$OUT/vicmon-$VER-esp32s3-full.bin"
APP="$OUT/vicmon-$VER-firmware.bin"

# Offsets and flash settings must match what `pio run -t upload` uses for env:s3
# (esp32s3, dio, 80m, 8MB) — check with `pio run -e s3 -t upload -v` if the board
# config changes. No --fill-flash-size: padding out to 8MB would blank the
# filesystem and coredump regions too.
"$PY" "$ESPTOOL" --chip esp32s3 merge_bin -o "$FULL" \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0000 "$BUILD/bootloader.bin" \
    0x8000 "$BUILD/partitions.bin" \
    0xe000 "$BOOT_APP0" \
    0x10000 "$BUILD/firmware.bin"
cp "$BUILD/firmware.bin" "$APP"

( cd "$OUT" && sha256sum "$(basename "$FULL")" "$(basename "$APP")" > SHA256SUMS )

# ESP Web Tools manifest: browser installs of the full image from a page that
# ships esp-web-tools. Served over HTTPS (or localhost) next to the .bin.
cat > "$OUT/manifest.json" <<JSON
{
  "name": "Vicmon",
  "version": "$VER",
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [{ "path": "$(basename "$FULL")", "offset": 0 }]
    }
  ]
}
JSON

echo
echo "release: $VER -> $OUT/"
ls -l "$OUT"
cat <<EOF

Blank board / recovery (ERASES config + keys — export the config JSON first):
  esptool.py --chip esp32s3 write_flash 0x0 $FULL

Update an existing unit, keeping its config:
  web UI  -> Network -> Firmware update (OTA) -> upload $(basename "$APP")
  esptool -> esptool.py --chip esp32s3 write_flash 0x10000 $APP
  no PC   -> Network -> System -> push/pull the clone over ESP-NOW
EOF
