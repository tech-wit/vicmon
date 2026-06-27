// Master firmware (headless dev build on AtomS3; Guition display added in
// Phase 3). Scans Victron BLE advertisements, keeps a registry of the latest
// values, and serves them over a WiFi access point:
//   - captive config portal (root page)
//   - GET /api/data  -> JSON snapshot for slaves / debugging
//
// Build/flash:  pio run -e atoms3 -t upload   (then tools/monitor.py)

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include <string>

#include "Registry.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "VictronTypes.h"

// ---- Configuration ---------------------------------------------------------

static const char* kApSsid = "Vicmon-Master";
static const char* kApPass = "vicmon1234";  // >= 8 chars; change before the field

// Monitored devices (name, type, AES key). Phase 2 will load these from NVS.
static DeviceSlot gDevices[] = {
    {"BMV", victron::Record::BatteryMonitor,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {"OrionXS", victron::Record::OrionXs,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};
static const size_t kNumDevices = sizeof(gDevices) / sizeof(gDevices[0]);

// ---- Globals ---------------------------------------------------------------

static NimBLEScan* gScan = nullptr;
static AsyncWebServer gServer(80);
static DNSServer gDns;

// ---- BLE ingestion ---------------------------------------------------------

static void ingest(NimBLEAdvertisedDevice* dev) {
    if (!dev->haveManufacturerData()) return;
    std::string md = dev->getManufacturerData();
    if (md.size() < 2 + 9) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(md.data());
    if (!(p[0] == 0xE1 && p[1] == 0x02)) return;  // Victron company id

    const uint8_t* extra = p + 2;
    size_t extraLen = md.size() - 2;

    uint8_t out[32];
    for (size_t i = 0; i < kNumDevices; ++i) {
        DeviceSlot& s = gDevices[i];
        int n = victron::decrypt(extra, extraLen, s.key, out, sizeof(out));
        if (n < 0) continue;  // not this device's key

        if (s.type == victron::Record::BatteryMonitor) {
            if (!victron::parseBatteryMonitor(out, n, s.battery)) return;
        } else if (s.type == victron::Record::OrionXs) {
            if (!victron::parseOrionXs(out, n, s.dcdc)) return;
        }
        s.everSeen = true;
        s.lastSeenMs = millis();
        return;
    }
}

static void pollBle() {
    NimBLEScanResults results = gScan->start(3 /*seconds*/, false);
    for (int i = 0; i < results.getCount(); ++i) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        ingest(&d);
    }
    gScan->clearResults();
}

// ---- API / portal ----------------------------------------------------------

// Builds the aggregated snapshot shared by GET /api/data and the root page.
static String buildJson() {
    uint32_t now = millis();
    const DeviceSlot* bmv = nullptr;
    const DeviceSlot* orion = nullptr;
    for (size_t i = 0; i < kNumDevices; ++i) {
        if (gDevices[i].type == victron::Record::BatteryMonitor) bmv = &gDevices[i];
        else if (gDevices[i].type == victron::Record::OrionXs) orion = &gDevices[i];
    }

    String j = "{";
    if (bmv && !bmv->stale(now)) {
        j += "\"battery_soc\":" + String(bmv->battery.soc, 1) + ",";
        j += "\"battery_voltage\":" + String(bmv->battery.voltage, 2) + ",";
        j += "\"battery_current\":" + String(bmv->battery.current, 2) + ",";
        j += "\"battery_stale\":false,";
    } else {
        j += "\"battery_stale\":true,";
    }
    j += "\"solar_current\":0.0,";  // no MPPT configured yet
    if (orion && !orion->stale(now)) {
        j += "\"dc_dc_status\":" + String(orion->dcdc.deviceState ? "\"active\"" : "\"off\"") + ",";
        j += "\"dc_dc_output_voltage\":" + String(orion->dcdc.outputVoltage, 2) + ",";
        j += "\"dc_dc_stale\":false,";
    } else {
        j += "\"dc_dc_status\":\"unknown\",\"dc_dc_stale\":true,";
    }
    j += "\"timestamp\":" + String(now / 1000);
    j += "}";
    return j;
}

static void handleRoot(AsyncWebServerRequest* req) {
    String html = "<!doctype html><html><head><meta name=viewport "
                  "content='width=device-width,initial-scale=1'>"
                  "<title>Vicmon Master</title></head><body style='font-family:sans-serif'>"
                  "<h2>Vicmon Master</h2><pre id=d>loading...</pre>"
                  "<script>setInterval(async()=>{let r=await fetch('/api/data');"
                  "document.getElementById('d').textContent="
                  "JSON.stringify(await r.json(),null,2);},1000);</script>"
                  "</body></html>";
    req->send(200, "text/html", html);
}

static void setupServer() {
    gServer.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildJson());
    });
    gServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) { handleRoot(req); });
    // Captive-portal: send everything else to the root page.
    gServer.onNotFound([](AsyncWebServerRequest* req) { handleRoot(req); });
    gServer.begin();
}

// ---- Arduino entry points --------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nVicmon Master (headless): BLE + WiFi AP");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP '%s' up at http://%s/  (pass: %s)\n", kApSsid,
                  ip.toString().c_str(), kApPass);

    gDns.start(53, "*", ip);  // captive portal: resolve all names to us
    setupServer();

    NimBLEDevice::init("");
    gScan = NimBLEDevice::getScan();
    gScan->setActiveScan(false);
    gScan->setInterval(100);
    gScan->setWindow(99);
}

void loop() {
    gDns.processNextRequest();
    pollBle();  // blocks ~3s per scan

    uint32_t now = millis();
    Serial.print("[state]");
    for (size_t i = 0; i < kNumDevices; ++i) {
        const DeviceSlot& s = gDevices[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
        if (!s.stale(now) && s.type == victron::Record::BatteryMonitor) {
            Serial.printf("(%.1f%%,%.2fV)", s.battery.soc, s.battery.voltage);
        } else if (!s.stale(now) && s.type == victron::Record::OrionXs) {
            Serial.printf("(out %.2fV)", s.dcdc.outputVoltage);
        }
    }
    Serial.println();
}
