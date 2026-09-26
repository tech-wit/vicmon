// Phase 1 dev firmware (ESP32 family): scan for Victron BLE advertisements,
// decrypt with the configured per-device key, parse the record, and print live
// values to Serial.
//
// Scanning is SYNCHRONOUS: each loop() does a blocking scan, then processes and
// prints all results from the single loop task. This avoids the garbled output
// you get when printing from the async BLE callback while loop() also prints.
//
// Build/flash:  pio run -e atoms3 -t upload   (then tools/monitor.py)

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <string>

#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "VictronTypes.h"

// Per-device AES keys (VictronConnect -> device -> Product info -> Encryption key).
// Device type is associated with the key (the advertisement has no type byte).
struct VictronKey {
    const char* name;
    victron::Record type;
    uint8_t key[16];
};
// Fill these in with your own devices before flashing: the key is the 32 hex
// characters from VictronConnect -> the device -> Product info -> Encryption key.
// Left as zeros on purpose — a real key committed here would be compiled into
// every image built from this repo. This scanner is a Phase-1 diagnostic; the
// real app (src/master/) stores keys in NVS, entered through its web UI.
static const VictronKey kKeys[] = {
    {"BMV", victron::Record::BatteryMonitor,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {"OrionXS", victron::Record::OrionXs,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};
static const size_t kNumKeys = sizeof(kKeys) / sizeof(kKeys[0]);

static NimBLEScan* gScan = nullptr;

static void hexDump(const uint8_t* d, int n) {
    for (int i = 0; i < n; ++i) Serial.printf("%02x", d[i]);
}

static const char* auxModeName(victron::AuxMode m) {
    switch (m) {
        case victron::AuxMode::StarterVoltage: return "starter";
        case victron::AuxMode::Midpoint: return "midpoint";
        case victron::AuxMode::Temperature: return "temp";
        default: return "none";
    }
}

static void processDevice(NimBLEAdvertisedDevice* dev, int& victron, int& decoded) {
    if (!dev->haveManufacturerData()) return;
    std::string md = dev->getManufacturerData();
    if (md.size() < 2 + 8) return;  // company id + minimum extra data

    const uint8_t* p = reinterpret_cast<const uint8_t*>(md.data());
    if (!(p[0] == 0xE1 && p[1] == 0x02)) return;  // Victron company id (LE)
    ++victron;

    const uint8_t* extra = p + 2;
    size_t extraLen = md.size() - 2;
    std::string addr = dev->getAddress().toString();

    // Raw, trustworthy dump (single task, no interleaving).
    Serial.printf("  %s model=0x%04x key=0x%02x md=", addr.c_str(),
                  victron::modelId(extra), extra[7]);
    for (size_t i = 0; i < md.size(); ++i) Serial.printf("%02x", (uint8_t)md[i]);
    Serial.println();

    // Try each configured key (key[0] sanity check selects the device).
    uint8_t out[32];
    const VictronKey* matched = nullptr;
    int n = -1;
    for (size_t i = 0; i < kNumKeys; ++i) {
        n = victron::decrypt(extra, extraLen, kKeys[i].key, out, sizeof(out));
        if (n >= 0) { matched = &kKeys[i]; break; }
    }
    if (!matched) return;
    ++decoded;

    if (matched->type == victron::Record::BatteryMonitor) {
        victron::BatteryData b;
        if (!victron::parseBatteryMonitor(out, n, b)) return;
        Serial.printf("    -> [%s] ", matched->name);
        if (b.voltageValid) Serial.printf("V=%.2fV ", b.voltage);
        if (b.currentValid) Serial.printf("I=%.3fA ", b.current);
        if (b.socValid) Serial.printf("SoC=%.1f%% ", b.soc);
        if (b.ttgValid) Serial.printf("TTG=%umin ", b.timeToGoMin);
        if (b.consumedValid) Serial.printf("Consumed=%.1fAh ", b.consumedAh);
        Serial.printf("aux(%s)=", auxModeName(b.auxMode));
        if (b.auxValid) Serial.printf("%.2f", b.auxValue); else Serial.printf("--");
        Serial.println();
    } else if (matched->type == victron::Record::OrionXs) {
        victron::DcDcData d;
        if (!victron::parseOrionXs(out, n, d)) return;
        Serial.printf("    -> [%s] state=%u Vin=%.2f Vout=%.2f Iin=%.1f Iout=%.1f decrypted=",
                      matched->name, d.deviceState, d.inputVoltage, d.outputVoltage,
                      d.inputCurrent, d.outputCurrent);
        hexDump(out, n);  // keep raw until layout confirmed against the app
        Serial.println();
    } else {
        Serial.printf("    -> [%s] (%d bytes) decrypted=", matched->name, n);
        hexDump(out, n);
        Serial.println();
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nVicmon Phase 1: Victron BLE scanner (synchronous)");

    NimBLEDevice::init("");
    gScan = NimBLEDevice::getScan();
    gScan->setActiveScan(false);  // Victron data rides in the passive advertisement
    gScan->setInterval(100);
    gScan->setWindow(99);
}

void loop() {
    NimBLEScanResults results = gScan->start(4 /*seconds*/, false);
    int total = results.getCount();
    int victron = 0, decoded = 0;
    for (int i = 0; i < total; ++i) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        processDevice(&d, victron, decoded);
    }
    Serial.printf("[scan] devices=%d victron=%d decoded=%d\n", total, victron, decoded);
    gScan->clearResults();
}
