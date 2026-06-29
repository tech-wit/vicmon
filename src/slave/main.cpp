// Slave firmware (LilyGo T-Display-S3) — Phase 4.
//
// Receives the master's ESP-NOW broadcast (see lib/slavelink/SlaveLink.h) and
// renders it. The display half is stubbed (`renderToSerial`) until the
// T-Display-S3 board arrives — the ESP-NOW + acquisition + staleness logic below
// is the real, board-independent core and compiles today.
//
// Channel acquisition: ESP-NOW peers must share a WiFi channel, and the master's
// SoftAP channel can move (it follows the STA channel when the master also joins
// a router). So instead of hard-pinning, the slave hops channels until it hears
// a frame, then locks on; if the master goes quiet it resumes hopping.
//
// Build:  pio run -e slave
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "SlaveLink.h"

static volatile bool gHaveFrame = false;
static slavelink::Snapshot gSnap;
static volatile uint32_t gLastRxMs = 0;
static volatile uint16_t gLastSeq = 0;
static volatile uint32_t gDrops = 0;  // gaps in the sequence counter

static uint8_t gChannel = 1;          // current listen channel
static uint32_t gLastHopMs = 0;
static bool gLocked = false;

static const uint32_t kStaleMs = 5000;    // no frame this long -> "disconnected"
static const uint32_t kHopMs = 250;       // dwell per channel while acquiring
static const uint8_t kMaxChannel = 13;    // 2.4 GHz channels to sweep

// ESP-NOW receive callback (arduino-esp32 2.x signature: sender MAC + payload).
// Keep the data copy minimal — render happens in loop().
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    (void)mac;
    if (len != (int)sizeof(slavelink::Snapshot)) return;
    slavelink::Snapshot s;
    memcpy(&s, data, sizeof(s));
    if (!slavelink::validHeader(s)) return;

    if (gHaveFrame) {
        uint16_t expected = (uint16_t)(gLastSeq + 1);
        if (s.seq != expected) ++gDrops;  // missed (or master rebooted)
    }
    gLastSeq = s.seq;
    gSnap = s;
    gHaveFrame = true;
    gLastRxMs = millis();
    gLocked = true;  // we heard the master on this channel
}

static void setChannel(uint8_t ch) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    gChannel = ch;
}

// Sweep channels until a frame arrives (gLocked), then hold. If the link goes
// stale, resume sweeping so we re-find the master if its channel moved.
static void serviceAcquisition() {
    uint32_t now = millis();
    bool stale = !gHaveFrame || (now - gLastRxMs) > kStaleMs;
    if (stale) gLocked = false;
    if (gLocked) return;
    if (now - gLastHopMs < kHopMs) return;
    gLastHopMs = now;
    uint8_t next = gChannel >= kMaxChannel ? 1 : gChannel + 1;
    setChannel(next);
}

// ---- presentation (serial stand-in for the T-Display-S3) -------------------

static const char* modeStr(uint8_t m) {
    switch (m) {
        case slavelink::M_CHARGING: return "CHARGING";
        case slavelink::M_DISCHARGING: return "DISCHARGING";
        case slavelink::M_IDLE: return "IDLE";
        default: return "UNKNOWN";
    }
}

static void renderToSerial() {
    uint32_t now = millis();
    bool live = gHaveFrame && (now - gLastRxMs) <= kStaleMs;
    if (!live) {
        Serial.printf("[slave] acquiring… (listening ch %u, drops=%lu)\n", gChannel,
                      (unsigned long)gDrops);
        return;
    }
    const slavelink::Snapshot& s = gSnap;
    auto has = [&](uint16_t f) { return (s.valid & f) != 0; };
    char soc[12], bv[12], ba[12];
    snprintf(soc, sizeof(soc), has(slavelink::V_SOC) ? "%.0f%%" : "--",
             slavelink::decDeci(s.soc_d));
    snprintf(bv, sizeof(bv), has(slavelink::V_BATTV) ? "%.2fV" : "--",
             slavelink::decCenti(s.battV_cv));
    snprintf(ba, sizeof(ba), has(slavelink::V_BATTA) ? "%+.1fA" : "--",
             slavelink::decDeci(s.battA_da));
    const char* alert = s.alertWorst >= 2 ? " !!CRIT" : (s.alertWorst == 1 ? " !warn" : "");
    Serial.printf("[slave] ch%u P%u %-11s SoC %s  %s  %s  solar %.1fA dcdc %.1fA load %.1fA%s\n",
                  gChannel, s.profile, modeStr(s.mode), soc, bv, ba,
                  slavelink::decDeci(s.solarA_da), slavelink::decDeci(s.dcdcA_da),
                  slavelink::decDeci(s.loadA_da), alert);
    // TODO(Phase 4): draw the above on the T-Display-S3 (TFT_eSPI/LVGL) — big SoC,
    // signed current, mode-coloured banner, sources, and a "disconnected" state.
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nVicmon slave: ESP-NOW receiver");

    // STA mode (not associated) so we can park on an arbitrary channel and listen.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(false);
    setChannel(gChannel);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed; restarting");
        delay(1000);
        ESP.restart();
    }
    esp_now_register_recv_cb(onRecv);
    Serial.println("Listening for master broadcast…");
}

void loop() {
    serviceAcquisition();
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw >= 1000) {
        lastDraw = millis();
        renderToSerial();
    }
    delay(20);
}
