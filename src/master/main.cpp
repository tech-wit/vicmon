// Master firmware (headless dev build on AtomS3; Guition display added in
// Phase 3). Scans Victron BLE advertisements, keeps an NVS-backed registry of
// devices and their latest values, and serves them over a WiFi access point:
//   - captive config portal (root page): add/list/remove devices + live data
//   - GET /api/data  -> JSON snapshot for slaves / debugging
//
// Build/flash:  pio run -e atoms3 -t upload   (then tools/monitor.py)

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include <string>

#include "DeviceConfig.h"
#include "Registry.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "VictronTypes.h"

static const char* kApSsid = "Vicmon-Master";
static const char* kApPass = "vicmon1234";  // >= 8 chars; change before the field

static DeviceConfig gConfig;
static NimBLEScan* gScan = nullptr;
static AsyncWebServer gServer(80);
static DNSServer gDns;

// ---- type <-> string helpers ----------------------------------------------

static victron::Record parseType(const String& t) {
    if (t == "dcdc") return victron::Record::OrionXs;
    return victron::Record::BatteryMonitor;
}
static const char* typeName(victron::Record r) {
    switch (r) {
        case victron::Record::OrionXs: return "dcdc";
        case victron::Record::BatteryMonitor: return "battery";
        default: return "?";
    }
}

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
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
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
    NimBLEScanResults results = gScan->start(2 /*seconds*/, false);
    for (int i = 0; i < results.getCount(); ++i) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        ingest(&d);
    }
    gScan->clearResults();
}

// ---- API / portal ----------------------------------------------------------

// Derives overall system status from battery current (charge positive).
static const char* systemStatus(const DeviceSlot* bmv, uint32_t now) {
    if (!bmv || bmv->stale(now) || !bmv->battery.currentValid) return "unknown";
    float i = bmv->battery.current;
    if (i > 0.5f) return "charging";
    if (i < -0.5f) return "discharging";
    return "idle";
}

static String buildJson() {
    uint32_t now = millis();
    const DeviceSlot* bmv = nullptr;
    const DeviceSlot* orion = nullptr;
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        if (s.type == victron::Record::BatteryMonitor) bmv = &s;
        else if (s.type == victron::Record::OrionXs) orion = &s;
    }

    String j = "{";
    j += "\"system_status\":\"" + String(systemStatus(bmv, now)) + "\",";
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

static String rootPage() {
    uint32_t now = millis();
    String h =
        "<!doctype html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'><title>Vicmon Master</title>"
        "<style>body{font-family:sans-serif;margin:1em}table{border-collapse:collapse}"
        "td,th{border:1px solid #ccc;padding:4px 8px}</style></head><body>"
        "<h2>Vicmon Master</h2>";

    const DeviceSlot* bmv = nullptr;
    for (size_t i = 0; i < gConfig.count(); ++i) {
        if (gConfig.slots()[i].type == victron::Record::BatteryMonitor) bmv = &gConfig.slots()[i];
    }
    h += "<p><b>Status:</b> " + String(systemStatus(bmv, now)) + "</p>";

    h += "<h3>Devices</h3><table><tr><th>Name</th><th>Type</th><th>State</th><th></th></tr>";
    for (size_t i = 0; i < gConfig.count(); ++i) {
        DeviceSlot& s = gConfig.slots()[i];
        h += "<tr><td>" + String(s.name) + "</td><td>" + typeName(s.type) + "</td><td>";
        if (s.stale(now)) {
            h += "stale";
        } else if (s.type == victron::Record::BatteryMonitor) {
            h += String(s.battery.soc, 1) + "% " + String(s.battery.voltage, 2) + "V";
        } else if (s.type == victron::Record::OrionXs) {
            h += "out " + String(s.dcdc.outputVoltage, 2) + "V";
        } else {
            h += "ok";
        }
        h += "</td><td><form method=post action=/del style=margin:0>"
             "<input type=hidden name=name value='" + String(s.name) + "'>"
             "<button>delete</button></form></td></tr>";
    }
    h += "</table>";

    h += "<h3>Add device</h3><form method=post action=/add>"
         "Name <input name=name required> "
         "Type <select name=type><option value=battery>battery</option>"
         "<option value=dcdc>dcdc</option></select> "
         "Key (32 hex) <input name=key pattern='[0-9a-fA-F]{32}' size=34 required> "
         "<button>add</button></form>";

    h += "<h3>Live JSON</h3><pre id=d>loading...</pre>"
         "<script>setInterval(async()=>{let r=await fetch('/api/data');"
         "document.getElementById('d').textContent=JSON.stringify(await r.json(),null,2);"
         "},1000);</script></body></html>";
    return h;
}

static void handleAdd(AsyncWebServerRequest* req) {
    String name = req->hasParam("name", true) ? req->getParam("name", true)->value() : "";
    String type = req->hasParam("type", true) ? req->getParam("type", true)->value() : "";
    String key = req->hasParam("key", true) ? req->getParam("key", true)->value() : "";
    uint8_t k[16];
    if (name.length() && DeviceConfig::parseHexKey(key, k)) {
        gConfig.add(name.c_str(), parseType(type), k);
        gConfig.save();
    }
    req->redirect("/");
}

static void handleDel(AsyncWebServerRequest* req) {
    if (req->hasParam("name", true)) {
        gConfig.remove(req->getParam("name", true)->value().c_str());
        gConfig.save();
    }
    req->redirect("/");
}

static void setupServer() {
    gServer.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildJson());
    });
    gServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", rootPage());
    });
    gServer.on("/add", HTTP_POST, handleAdd);
    gServer.on("/del", HTTP_POST, handleDel);
    gServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(200, "text/html", rootPage());  // captive portal
    });
    gServer.begin();
}

// ---- Arduino entry points --------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nVicmon Master (headless): BLE + WiFi AP");

    gConfig.begin();
    Serial.printf("Loaded %u device(s) from NVS\n", (unsigned)gConfig.count());

    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/4);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP '%s' up at http://%s/  (pass: %s)\n", kApSsid,
                  ip.toString().c_str(), kApPass);

    gDns.start(53, "*", ip);
    setupServer();

    NimBLEDevice::init("");
    gScan = NimBLEDevice::getScan();
    gScan->setActiveScan(false);
    // Keep BLE duty cycle low so the WiFi AP gets enough radio airtime to stay
    // joinable (window/interval ~= 30%). Victron advertises ~1/s, so this still
    // catches every device.
    gScan->setInterval(160);
    gScan->setWindow(48);
}

void loop() {
    gDns.processNextRequest();
    pollBle();  // blocks ~3s per scan

    uint32_t now = millis();
    Serial.print("[state]");
    for (size_t i = 0; i < gConfig.count(); ++i) {
        const DeviceSlot& s = gConfig.slots()[i];
        Serial.printf(" %s=%s", s.name, s.stale(now) ? "stale" : "ok");
        if (!s.stale(now) && s.type == victron::Record::BatteryMonitor) {
            Serial.printf("(%.1f%%,%.2fV)", s.battery.soc, s.battery.voltage);
        } else if (!s.stale(now) && s.type == victron::Record::OrionXs) {
            Serial.printf("(out %.2fV)", s.dcdc.outputVoltage);
        }
    }
    Serial.println();
}
