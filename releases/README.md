# Released firmware images

Built images committed to the repo, so a working unit can be flashed with no
PlatformIO checkout, no build, and no release-server API — clone or download the
directory and flash.

```
releases/v<version>/
  vicmon-<version>-esp32s3-full.bin   flash at 0x0   — blank board / recovery, WIPES config
  vicmon-<version>-firmware.bin       flash at 0x10000, or the web OTA, or the ESP-NOW clone
  SHA256SUMS                          verify with: cd releases/v<version> && sha256sum -c SHA256SUMS
  manifest.json                       ESP Web Tools descriptor
```

Which image to use, and why the full one erases your device keys, is in
[../docs/DEPLOY.md](../docs/DEPLOY.md). Read that before flashing the full image.

## Cutting one

```bash
./tools/release.sh --publish     # builds, packages into dist/, copies here
git add releases/v<version> && git commit
```

Without `--publish` the script only writes `dist/`, which is git-ignored — so an
experimental build can't wander into a commit.

## The cost of keeping them here

Each release is about **3 MB** (1.5 MB full + 1.5 MB app), and git history is
forever: binaries can't be pruned later without rewriting history. Twenty releases is
~60 MB. That is a fine trade for a handful of real releases and a poor one for every
development build, so publish deliberately — tagged versions, not every flash.

The same files are also attached to the matching **GitHub Release**, which costs the
repo nothing — Release assets do not live in git:

```bash
gh release create v<version> dist/vicmon-<version>-*.bin dist/SHA256SUMS dist/manifest.json
```

So if the history ever does get heavy, drop the in-tree copies and keep the Release
assets alone. The build side doesn't change; only where the files land.
