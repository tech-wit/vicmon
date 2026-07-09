// BLE ingestion: scans Victron advertisements, decrypts + parses each against
// the configured device keys into the registry, and tracks unconfigured devices
// for the "discovered" adopt list. Split out of main.cpp (P2). Runs on the loop
// task; holds the registry lock (app.h RegLock) for the decode pass.

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <cstring>
#include <string>

#include "app.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"

// ---- BLE ingestion ---------------------------------------------------------

// In debug-capture mode, stash up to 16 bytes of the (encrypted) advert payload
// so the diagnostics view / serial can show raw bytes for an unknown device.
static void captureRaw(Discovered& d, const uint8_t* raw, size_t rawLen) {
    if (!gDebugCapture || !raw) return;
    uint8_t n = rawLen > sizeof(d.raw) ? sizeof(d.raw) : (uint8_t)rawLen;
    memcpy(d.raw, raw, n);
    d.rawLen = n;
}

static void noteDiscovered(const char* mac, const char* name, uint16_t model, int rssi,
                           const uint8_t* raw, size_t rawLen) {
    uint32_t now = millis();
    for (size_t i = 0; i < gDiscN; ++i) {
        if (strncmp(gDisc[i].mac, mac, sizeof(gDisc[i].mac)) == 0) {
            gDisc[i].rssi = rssi;
            gDisc[i].model = model;
            gDisc[i].lastSeenMs = now;
            if (name && name[0]) strncpy(gDisc[i].name, name, sizeof(gDisc[i].name) - 1);
            captureRaw(gDisc[i], raw, rawLen);
            return;
        }
    }
    if (gDiscN < (sizeof(gDisc) / sizeof(gDisc[0]))) {
        Discovered& d = gDisc[gDiscN++];
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        if (name) strncpy(d.name, name, sizeof(d.name) - 1);
        d.model = model;
        d.rssi = rssi;
        d.lastSeenMs = now;
        captureRaw(d, raw, rawLen);
    }
}

int gScanVictron = 0, gScanDecoded = 0;  // per-scan diagnostics

static void ingest(NimBLEAdvertisedDevice* dev) {
    if (!dev->haveManufacturerData()) return;
    std::string md = dev->getManufacturerData();
    if (md.size() < 2 + 9) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(md.data());
    if (!(p[0] == 0xE1 && p[1] == 0x02)) return;  // Victron company id
    ++gScanVictron;

    const uint8_t* extra = p + 2;
    size_t extraLen = md.size() - 2;

    uint8_t out[32];
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        int n = victron::decrypt(extra, extraLen, s.key, out, sizeof(out));
        if (n < 0) continue;
        switch (s.type) {
            case victron::Record::BatteryMonitor:
                if (!victron::parseBatteryMonitor(out, n, s.battery)) return;
                break;
            case victron::Record::OrionXs:
                if (!victron::parseOrionXs(out, n, s.dcdc)) return;
                break;
            case victron::Record::SolarCharger:
                if (!victron::parseSolarCharger(out, n, s.solar)) return;
                break;
            case victron::Record::AcCharger:
                if (!victron::parseAcCharger(out, n, s.charger)) return;
                break;
            default:
                break;
        }
        s.everSeen = true;
        s.lastSeenMs = millis();
        ++gScanDecoded;
        s.rawLen = n > (int)sizeof(s.raw) ? sizeof(s.raw) : n;  // keep for /diag
        memcpy(s.raw, out, s.rawLen);
        s.modelId = victron::modelId(extra);
        strncpy(s.mac, dev->getAddress().toString().c_str(), sizeof(s.mac) - 1);
        if (dev->getName().size())
            strncpy(s.btname, dev->getName().c_str(), sizeof(s.btname) - 1);
        return;
    }
    // No configured key matched -> a device we could adopt (and, in debug mode,
    // capture the raw advert bytes of for troubleshooting).
    noteDiscovered(dev->getAddress().toString().c_str(), dev->getName().c_str(),
                   victron::modelId(extra), dev->getRSSI(), extra, extraLen);
}

void pollBle() {
    gScanVictron = 0;
    gScanDecoded = 0;
    NimBLEScanResults results = gScan->start(2 /*seconds*/, false);
    {
        // Hold the registry lock only for the decode pass, never across the ~2 s
        // scan above, so a config write from the web task waits at most one pass.
        RegLock lk;
        for (int i = 0; i < results.getCount(); ++i) {
            NimBLEAdvertisedDevice d = results.getDevice(i);
            ingest(&d);
        }
    }
    gScan->clearResults();
}
