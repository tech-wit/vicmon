// ESP-NOW broadcaster: packs the live panel into a Snapshot and broadcasts it to
// slaves ~4/s from a timer, plus a low-rate 7-day StatsFrame. Split out of
// main.cpp (P2). The panel/registry/stats core stays in main.cpp; this file only
// serializes and transmits it through the shared contract in app.h.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <cstring>

#include "app.h"

// The firmware-clone engine (master<->slave OTA push). Fed from the single
// ESP-NOW recv callback (below for the master role; via gRx's frame hook for the
// slave role), driven by serviceOta() from both loops. See lib/slavelink/OtaLink.h.
slavelink::OtaEngine gOta;

// ---- ESP-NOW broadcast to slaves -------------------------------------------
// Broadcasts a packed snapshot to FF:FF:FF:FF:FF:FF ~1/s. Connectionless, so any
// number of slaves can listen with no pairing and a dropped frame self-heals on
// the next send. Shares the radio with the AP + BLE; the AP is pinned to channel
// 1, and the broadcast peer uses channel 0 ("current channel") to follow it.

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// gEspNowOk + gSnapSeq are declared with the master-identity globals near the top
// (collectDash reads them before this section).
static slavelink::Snapshot gCachedSnap;
static volatile bool gSnapReady = false;
static esp_timer_handle_t gBcastTimer = nullptr;
static void broadcastTick(void*);  // defined after buildSnapshot

// Graph history pull: a slave unicasts a HistReq; we reply with the two trend
// rings as paced HistChunk frames (see serviceHistSend). Set from the recv cb.
static volatile bool gHistReqPending = false;
static uint8_t gHistReqMac[6] = {0};
static bool gHistSending = false, gHistPeerAdded = false;
static uint8_t gHistPeerMac[6] = {0};
static uint8_t gHistRing = 0;      // 0 fine, 1 coarse
static uint16_t gHistOffset = 0;

// Receive callback (master role). Only handles the tiny history request; the
// bulky reply is sent from the loop so we never block the WiFi task.
static void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (gOta.onFrame(mac, data, len)) return;  // OTA push frames (announce/accept/data/ctrl)
    if (len == (int)sizeof(slavelink::HistReq) && mac) {
        slavelink::HistReq r;
        memcpy(&r, data, sizeof(r));
        if (slavelink::validHistReq(r) && r.masterId == gMasterId) {
            memcpy(gHistReqMac, mac, 6);
            gHistReqPending = true;
        }
    }
}

void setupEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(&onEspNowRecv);  // answer slave history-pull requests
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcastMac, 6);
    peer.channel = 0;      // 0 = current WiFi channel (AP pinned to 1)
    peer.encrypt = false;  // broadcast can't be encrypted; telemetry only
    // The master always runs SoftAP (channel-pinned); transmit via the AP
    // interface. The default (STA) interface doesn't exist in AP-only mode, so
    // esp_now_send would fail silently and no slave would ever hear us.
    peer.ifidx = WIFI_IF_AP;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("ESP-NOW peer add failed");
        return;
    }
    gEspNowOk = true;
    // Transmit the cached snapshot ~4/s from a timer, independent of the loop.
    esp_timer_create_args_t ta = {};
    ta.callback = &broadcastTick;
    ta.name = "vbcast";
    if (esp_timer_create(&ta, &gBcastTimer) == ESP_OK)
        esp_timer_start_periodic(gBcastTimer, 250000);  // 250 ms
    Serial.println("ESP-NOW broadcaster ready (250ms tick)");
}

static slavelink::Snapshot buildSnapshot() {
    using namespace slavelink;
    uint32_t now = millis();
    Snapshot s = {};
    fillHeader(s);
    s.masterId = gMasterId;
    s.flags = pairingActive() ? F_PAIRING : 0;

    PanelModel p = collectPanel(now);
    switch (p.mode) {
        case ChargeMode::Charging: s.mode = M_CHARGING; break;
        case ChargeMode::Discharging: s.mode = M_DISCHARGING; break;
        case ChargeMode::Idle: s.mode = M_IDLE; break;
        default: s.mode = M_UNKNOWN; break;
    }

    uint16_t v = 0;
    if (p.soc.valid) v |= V_SOC;
    if (p.battV.valid) v |= V_BATTV;
    if (p.battA.valid) v |= V_BATTA;
    if (p.solarA.valid) v |= V_SOLAR;
    if (p.chargerA.valid) v |= V_CHARGER;
    if (p.dcdcOutA.valid) v |= V_DCDC;
    if (p.loadA.valid) v |= V_LOAD;
    if (p.ttg.valid) v |= V_TTG;
    if (p.starterV.valid) v |= V_STARTERV;
    if (p.solarW.valid) v |= V_SOLARW;
    if (p.solarV.valid) v |= V_SOLARV;
    if (p.dcdcInV.valid) v |= V_DCDCINV;
    if (p.dcdcOutV.valid) v |= V_DCDCOUTV;
    if (p.consumed.valid) v |= V_CONSUMED;
    s.valid = v;

    s.soc_d = encDeci(p.soc.valid, p.soc.value);
    s.battV_cv = encCenti(p.battV.valid, p.battV.value);
    s.battA_da = encDeci(p.battA.valid, p.battA.value);
    s.solarA_da = encDeci(p.solarA.valid, p.solarA.value);
    s.chargerA_da = encDeci(p.chargerA.valid, p.chargerA.value);
    s.dcdcA_da = encDeci(p.dcdcOutA.valid, p.dcdcOutA.value);
    s.loadA_da = encDeci(p.loadA.valid, p.loadA.value);
    s.starterV_cv = encCenti(p.starterV.valid, p.starterV.value);
    s.ttg_min = p.ttg.valid ? (uint16_t)p.ttg.value : 0xFFFF;
    s.solarW_w = encWhole(p.solarW.valid, p.solarW.value);
    s.solarV_cv = encCenti(p.solarV.valid, p.solarV.value);
    s.dcdcInV_cv = encCenti(p.dcdcInV.valid, p.dcdcInV.value);
    s.dcdcOutV_cv = encCenti(p.dcdcOutV.valid, p.dcdcOutV.value);
    s.consumedAh_da = encDeci(p.consumed.valid, p.consumed.value);
    s.capacityAh = (uint16_t)(p.capacity > 0 ? p.capacity + 0.5f : 0);

    s.alertWorst = (uint8_t)p.alertWorst;
    s.profile = (uint8_t)gProfiles.active();
    s.seq = 0;  // stamped per actual transmit in broadcastTick()
    s.uptime_s = now / 1000;
    return s;
}

// The loop refreshes gCachedSnap (registry-owning thread), and a 250 ms esp_timer
// transmits it — so the broadcast rate (~4/s) is independent of the loop's ~2 s
// BLE-blocked cadence. Without this a channel-hopping slave rarely coincides with
// a send and can take a very long time to acquire. seq is stamped per transmit so
// the slave's drop detection stays correct.
// gCachedSnap is written whole by the loop and read+stamped by the esp_timer
// task; a spinlock makes the ~54-byte copy atomic so a tick can't transmit a
// half-updated frame.
static portMUX_TYPE gSnapMux = portMUX_INITIALIZER_UNLOCKED;

static void broadcastTick(void*) {
    if (!gEspNowOk) return;
    serviceHistSend();  // pump the paced history reply here (every 250ms, not the 2s loop)
    if (!gSnapReady) return;
    slavelink::Snapshot s;
    portENTER_CRITICAL(&gSnapMux);
    s = gCachedSnap;
    portEXIT_CRITICAL(&gSnapMux);
    // Stamp per-transmit fields on the local copy (kept fresh between loop builds).
    s.seq = ++gSnapSeq;
    s.uptime_s = millis() / 1000;
    s.flags = pairingActive() ? slavelink::F_PAIRING : 0;
    esp_err_t e = esp_now_send(kBroadcastMac, (const uint8_t*)&s, sizeof(s));
    static uint32_t lastErrLog = 0;
    if (e != ESP_OK && millis() - lastErrLog > 3000) {
        lastErrLog = millis();
        Serial.printf("[espnow] send err 0x%x\n", e);
    }
}

// Called each loop: refresh the cached snapshot from live signals (the timer does
// the actual transmitting).
void sendSlaveBroadcast() {
    if (!gEspNowOk) return;
    slavelink::Snapshot s = buildSnapshot();
    portENTER_CRITICAL(&gSnapMux);
    gCachedSnap = s;
    gSnapReady = true;
    portEXIT_CRITICAL(&gSnapMux);
}

// Low-rate energy frame for a slave's Week page (see StatsFrame): the three
// resettable meters + the last-7 day Ah bars. Built on the loop task (owns
// gStats) and sent directly — it changes slowly, so a few sends a minute is fine.
static uint16_t ahU16(double x) { return x <= 0 ? 0 : (x >= 65535 ? 65535 : (uint16_t)(x + 0.5)); }
static uint32_t ahU32(double x) { return x <= 0 ? 0 : (uint32_t)(x + 0.5); }

static void fillMeter(slavelink::StatMeterW& m, const stats::Bucket& b) {
    m.inAh = ahU32(b.chargedAh);   m.outAh = ahU32(b.dischargedAh);
    m.solarAh = ahU32(b.solarAh);  m.dcdcAh = ahU32(b.dcdcAh);
    m.chargerAh = ahU32(b.chargerAh); m.loadAh = ahU32(b.loadAh);
    m.durSecs = b.durationSecs;
}

void sendStatsFrame() {
    if (!gEspNowOk) return;
    using namespace slavelink;
    StatsFrame f = {};
    fillStatsHeader(f);
    f.masterId = gMasterId;
    f.clockOk = currentLocalEpoch() != 0 ? 1 : 0;
    f.utcNow = currentUtcEpoch();                     // slave adopts this as its clock
    f.dayNow = gStats.bucket(stats::TODAY).dayStamp;  // current day key (axis labels)
    fillMeter(f.today, gStats.bucket(stats::TODAY));
    fillMeter(f.trip,  gStats.bucket(stats::TRIP));
    fillMeter(f.total, gStats.bucket(stats::TOTAL));
    int dc = (int)gStats.dayCount();
    int start = dc > 7 ? dc - 7 : 0, out = 0;
    for (int i = start; i < dc && out < 7; ++i) {
        const stats::DayRecord& r = gStats.day(i);
        f.daySolarAh[out] = ahU16(r.solarAh);
        f.dayDcdcAh[out] = ahU16(r.dcdcAh);
        f.dayChargerAh[out] = ahU16(r.chargerAh);
        f.dayLoadAh[out] = ahU16(r.loadAh);
        f.dayStamp[out] = r.dayStamp;
        ++out;
    }
    f.dayCount = (uint8_t)out;
    esp_now_send(kBroadcastMac, (const uint8_t*)&f, sizeof(f));
}

// ---- Graph history responder (paced) ---------------------------------------
static void ensureHistPeer(const uint8_t* mac) {
    if (gHistPeerAdded && memcmp(gHistPeerMac, mac, 6) == 0) return;
    if (gHistPeerAdded) { esp_now_del_peer(gHistPeerMac); gHistPeerAdded = false; }
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.encrypt = false; p.ifidx = WIFI_IF_AP;  // master is AP
    if (esp_now_add_peer(&p) == ESP_OK) { memcpy(gHistPeerMac, mac, 6); gHistPeerAdded = true; }
}

// Serialize one chunk of a ring (chronological order, oldest first) and unicast
// it. Returns false if the send queue is full (retry the same offset next time).
static bool sendHistChunk(uint8_t ring, const HistRing& r, uint16_t offset) {
    using namespace slavelink;
    HistChunk c = {};
    fillHistChunkHdr(c);
    c.ring = ring;
    c.masterId = gMasterId;
    c.fineTotal = (uint16_t)gFine.count;      // both totals in every chunk so the slave
    c.coarseTotal = (uint16_t)gCoarse.count;  // knows to wait for coarse (sent after fine)
    c.offset = offset;
    uint16_t remain = (uint16_t)r.count - offset;
    uint8_t n = remain > kHistChunkPts ? kHistChunkPts : (uint8_t)remain;
    c.count = n;
    for (uint8_t i = 0; i < n; ++i) {
        const HistSample& s = r.buf[(r.head + r.cap - r.count + offset + i) % r.cap];
        memcpy(&c.pts[i], &s, sizeof(HistPointW));  // HistSample and HistPointW share layout
    }
    return esp_now_send(gHistPeerMac, (const uint8_t*)&c, sizeof(c)) == ESP_OK;
}

// Called each master loop: begin a pull on request, then gently paced-send the
// fine then coarse ring as unicast chunks (a small batch per cycle, so ~30 s for
// the full ~120 chunks) — never contending with the BLE scan / AP / broadcast.
void serviceHistSend() {
    if (gHistReqPending) {
        gHistReqPending = false;
        ensureHistPeer(gHistReqMac);
        Serial.printf("[hist] req from ..%02X:%02X peer=%d fine=%u coarse=%u\n",
                      gHistReqMac[4], gHistReqMac[5], gHistPeerAdded,
                      (unsigned)gFine.count, (unsigned)gCoarse.count);
        if (gHistPeerAdded) { gHistSending = true; gHistRing = 0; gHistOffset = 0; }
    }
    if (!gHistSending) return;
    static uint8_t tick = 0;
    if (++tick & 1) return;  // pace: send on every other 250ms tick (gentler = less loss)
    const int kBatch = 6;  // chunks per send (queue usually fills after ~4)
    for (int i = 0; i < kBatch; ++i) {
        const HistRing& r = (gHistRing == 0) ? gFine : gCoarse;
        if (gHistOffset >= (uint16_t)r.count) {  // this ring done
            if (gHistRing == 0) { gHistRing = 1; gHistOffset = 0; continue; }
            gHistSending = false; Serial.println("[hist] send complete"); break;
        }
        if (!sendHistChunk(gHistRing, r, gHistOffset)) break;  // queue full -> next tick
        uint16_t remain = (uint16_t)r.count - gHistOffset;
        gHistOffset += remain > slavelink::kHistChunkPts ? slavelink::kHistChunkPts : remain;
    }
}

// ---- firmware clone (OTA push) glue ----------------------------------------
// In the slave role the single ESP-NOW recv callback belongs to gRx (Receiver);
// this hook lets OTA frames reach the engine there. In the master role the frames
// arrive via onEspNowRecv above. Either way everything funnels into gOta.
static void otaFrameHook(const uint8_t* mac, const uint8_t* data, int len) {
    gOta.onFrame(mac, data, len);
}

static bool loadOtaAllow() {
    Preferences p;
    p.begin("vicota", true);
    bool a = p.getBool("allow", true);  // default on — a dropped transfer is non-destructive
    p.end();
    return a;
}
void saveOtaAllow(bool allow) {
    Preferences p;
    p.begin("vicota", false);
    p.putBool("allow", allow);
    p.end();
    gOta.setAllowRemote(allow);
}

// Init the OTA engine for this boot's role. localId = the masterId that tags/filters
// our OTA frames: the master uses its own id; a slave uses its paired master's id so
// both ends share it. Both roles transmit over the SoftAP interface.
void setupOta(uint8_t role) {
    uint32_t localId = (role == ROLE_SLAVE) ? gRx.pairedMaster() : gMasterId;
    gOta.setVersion(kFwVersion);  // before begin() so it isn't overwritten by the app-desc version
    gOta.begin(localId, role, WIFI_IF_AP);
    gOta.setAllowRemote(loadOtaAllow());
    if (role == ROLE_SLAVE) gRx.setFrameHook(&otaFrameHook);  // master funnels via onEspNowRecv
}

// Call each loop (both roles): keep a slave's filter id current (it can pair/unpair
// at runtime) and pump the transfer state machine.
void serviceOta() {
    if (gRole == ROLE_SLAVE) gOta.setLocalId(gRx.pairedMaster());
    gOta.service();
}
