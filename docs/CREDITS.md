# Credits, references & third-party code

Everything Vicmon leans on, whether it ships in the firmware, was used to work out
a format, or was a reference implementation to check against.

## Protocol references

The BLE decode was written from the specification and cross-checked against two
existing implementations — neither is vendored, both were read.

| | |
|---|---|
| Victron **"Extra Manufacturer Data"** specification | The authoritative advertisement format: company ID `0x02E1`, the record header, AES-128-CTR, and the per-record bit layouts. |
| [`keshavdv/victron-ble`](https://github.com/keshavdv/victron-ble) | Python reference implementation. The clearest cross-check for field order, scaling and the "not available" sentinels. |
| [`wytr/VictronSolarDisplayEsp`](https://github.com/wytr/VictronSolarDisplayEsp) | ESP implementation; the starting point named in `PROJECT_SPEC.md`, and a reference for doing this on an ESP32 at all. |

## Vendored source (in this repo)

| | |
|---|---|
| [`kokke/tiny-AES-c`](https://github.com/kokke/tiny-AES-c) | The Unlicense (public domain). `lib/victron/tiny_aes.*` is a trimmed CTR-only derivation. |
| **Adafruit GFX bitmap fonts** — `FreeSans9pt7b`, `FreeSansBold12/18/24pt7b` | `lib/guition/fonts/`, in Adafruit's GFX font format, from the [Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library) font set; those in turn are converted from **GNU FreeFont** (URW++ Nimbus Sans). Used by both the Guition and LilyGo renderers. |

## Libraries linked into the firmware

All pulled by PlatformIO — see `platformio.ini`.

| | |
|---|---|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) (h2zero) | The BLE stack. Configured scan-only, with the peripheral/broadcaster roles compiled out to reclaim heap. |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) + [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) (ESP32Async fork) | The web server behind the whole app and its APIs. The maintained fork, not the original me-no-dev repos. |
| [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) (moononournation) | Display driver + primitives for both panels: AXS15231B QSPI on the Guition, ST7789 over the S3 LCD8 bus on the LilyGo. Pinned to ~1.5.9 (see `platformio.ini` for why). |
| [ArduinoJson](https://arduinojson.org/) (bblanchon) | Config import/export. |
| [BME68x Sensor library](https://github.com/boschsensortec/Bosch-BME68x-Library) (Bosch Sensortec, BSD-3) | The Unit ENV Pro's BME688. Bosch's own driver rather than a hand-rolled one: the compensation maths is a long chain of calibration polynomials that is easy to get subtly and silently wrong. Deliberately *not* BSEC2, which is a closed-source blob. |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) / ESP-IDF (Espressif) | Framework, WiFi, ESP-NOW, NVS, LittleFS, the `Update` OTA library and `esp_timer`. |

## Used during development, not shipped

| | |
|---|---|
| [`me-processware/JC3248W535-Driver`](https://github.com/me-processware/JC3248W535-Driver) | The known-good baseline for bringing up the Guition panel. `src/gfxref/` is its Basic example run verbatim, kept as the reference env — when the panel misbehaves, prove it there first. |
| [LVGL 9](https://lvgl.io/) | Evaluated as the display toolkit and **dropped** (anti-aliased fonts fringe on RGB565, plus an unexplained WiFi-SoftAP heap crash). Kept as the `lvglref` env so the decision can be re-tested rather than re-argued. |
| [PlatformIO](https://platformio.org/) | Build, dependency management, upload, unit tests. |
| [esptool](https://github.com/espressif/esptool) / [esptool-js](https://github.com/espressif/esptool-js) / [ESP Web Tools](https://esphome.github.io/esp-web-tools/) | Flashing prebuilt images, including from a browser. See [DEPLOY.md](DEPLOY.md). |
| [Unity](https://github.com/ThrowTheSwitch/Unity) (via PlatformIO) | The host-side unit tests in `test/`. |

## Hardware documentation

M5Stack's schematics and pinouts for the **M5Capsule** (StampS3), **Unit ENV Pro** and
**AtomS3**; LilyGo's for the **T-Display-S3**. Every pin in the Hardware table of
`PROJECT_PLAN.md` was confirmed on the bench rather than trusted from a datasheet —
several were wrong or board-revision-specific (the Capsule v1.1 LED power rail on
GPIO38 being the memorable one).

## Licensing

Vicmon itself is **MIT** — see [`../LICENSE`](../LICENSE).

The vendored and linked pieces keep their own terms, all compatible with that:

| Component | Terms |
|---|---|
| `lib/victron/tiny_aes.*` (from kokke/tiny-AES-c) | The Unlicense — public domain |
| `lib/guition/fonts/*.h` (Adafruit GFX, from GNU FreeFont) | GPL **with the font exception**, which explicitly allows embedding a font in a program without affecting that program's licence |
| Bosch BME68x driver | BSD-3-Clause |
| NimBLE-Arduino, AsyncTCP, ESPAsyncWebServer, Arduino_GFX, ArduinoJson | Permissive (MIT / Apache-2.0 / BSD-3) |
| arduino-esp32 / ESP-IDF | LGPL-2.1 / Apache-2.0 per component |

The linked libraries are fetched by PlatformIO rather than redistributed here — with
one exception: the prebuilt images under [`../releases/`](../releases/) contain them
compiled in, so those binaries carry those libraries' terms too.
