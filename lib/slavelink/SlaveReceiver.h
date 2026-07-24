#pragma once
#include "SlaveLink.h"

// ESP-NOW receive + acquisition + pairing state machine, shared by the standalone
// slave firmware (src/slave) and the master firmware's slave role (dual-purpose).
// Kept out of SlaveLink.h so the native host test — which includes only the wire
// format — never pulls in the radio headers.
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <new>

namespace slavelink {

// Listens for the master broadcast, filters by the *paired* masterId (so several
// masters can share the air), hops channels to acquire, and runs two-sided
// pairing: it adopts a master only while that master is inviting (F_PAIRING) AND
// the app has opened an adopt window (startAdopt(), from a button/touch/serial).
// The paired id is stored in NVS so it survives reboot.
//
// Single instance — the ESP-NOW C callback dispatches through a static self
// pointer. Call begin() once, poll() every loop, and read the accessors for the
// UI. User actions come in via startAdopt() / unpair().
class Receiver {
 public:
  static const uint32_t kStaleMs = 5000;   // no frame this long -> disconnected
  static const uint32_t kAdoptMs = 30000;  // how long an adopt window stays open

  // manageWifi=true: own the radio — STA mode + channel-hop to find the master.
  // manageWifi=false: the caller owns WiFi (e.g. runs a config SoftAP on a fixed
  // channel); the receiver just listens on the current channel and never hops.
  void begin(const char* nvsNs = "vicslave", bool manageWifi = true) {
    self_ = this;
    ns_ = nvsNs;
    manageWifi_ = manageWifi;
    allocHistBuffers();  // slave-only: a master never calls begin(), so it never pays the ~26KB
    load();
    if (manageWifi_) {
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      esp_wifi_set_promiscuous(false);
      setChannel(1);
    }
    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(&Receiver::onRecvStatic);
    ok_ = true;
  }

  // Service acquisition + pairing timeouts + NVS commits. Call every loop.
  void poll() {
    serviceAcquisition();
    servicePairing();
    serviceHistory();
  }

  // User triggers (raised from a button, touch, or serial command).
  void startAdopt() { startAdopt_ = true; }  // open the adopt window
  void unpair() { unpairReq_ = true; }       // forget the current master

  // Optional peek at every received frame BEFORE the receiver's own dispatch, so
  // the app can layer extra frame types (e.g. OTA) on the single ESP-NOW recv
  // callback. Runs in the callback context — keep it minimal (stash + flag).
  void setFrameHook(void (*fn)(const uint8_t*, const uint8_t*, int)) { frameHook_ = fn; }

  // ---- state for the UI ----
  bool ok() const { return ok_; }
  uint32_t pairedMaster() const { return paired_; }
  bool isPaired() const { return paired_ != 0; }
  bool isAdopting() const { return adopting_; }
  uint32_t adoptSecsLeft() const {
    if (!adopting_) return 0;
    uint32_t e = millis() - adoptStartMs_;
    return e >= kAdoptMs ? 0 : (kAdoptMs - e) / 1000;
  }
  uint8_t channel() const { return channel_; }
  uint32_t drops() const { return drops_; }
  uint32_t heardMaster() const { return heardMaster_; }
  // A master is currently inviting pairing on our channel.
  bool heardInvite() const {
    return (millis() - lastAnyMs_) < kStaleMs && (heardFlags_ & F_PAIRING);
  }
  bool anyMasterHeard() const { return lastAnyMs_ != 0 && (millis() - lastAnyMs_) < kStaleMs; }
  bool live() const { return haveFrame_ && (millis() - lastRxMs_) <= kStaleMs; }
  bool haveSnapshot() const { return haveFrame_; }  // ever received (retain last-known when stale)
  const Snapshot& snapshot() const { return snap_; }
  // 7-day stats (Week page). Valid longer than the live window since it's low-rate.
  bool hasStats() const { return haveStats_ && (millis() - lastStatsMs_) < 30000; }
  bool everStats() const { return haveStats_; }  // retain last-known when stale
  const StatsFrame& stats() const { return stats_; }

  // ---- Graph history pull ----
  bool haveMasterMac() const { return haveMasterMac_; }
  bool histActive() const { return histActive_; }
  bool historyReady() const { return histReady_; }
  // Backlog-pull progress 0..100 (received chunks / expected chunks).
  uint8_t histPercent() const {
    if (!fineGot_ || !coarseGot_) return 0;  // master (no staging allocated)
    uint16_t need = (fineTotal_ + kHistChunkPts - 1) / kHistChunkPts +
                    (coarseTotal_ + kHistChunkPts - 1) / kHistChunkPts;
    if (need == 0) return 0;
    uint16_t got = 0;
    for (uint16_t i = 0; i < kFineChunks; ++i) got += fineGot_[i];
    for (uint16_t i = 0; i < kCoarseChunks; ++i) got += coarseGot_[i];
    uint32_t p = (uint32_t)got * 100 / need;
    return p > 100 ? 100 : (uint8_t)p;
  }
  uint16_t fineCount() const { return fineTotal_; }
  uint16_t coarseCount() const { return coarseTotal_; }
  const HistPointW& finePoint(uint16_t i) const { return fineStage_ ? fineStage_[i] : kNaPoint(); }
  const HistPointW& coarsePoint(uint16_t i) const { return coarseStage_ ? coarseStage_[i] : kNaPoint(); }

  // Start (or restart) a full history pull from our paired master. No-op until
  // the master's MAC is known (learned from a received frame). Call once the link
  // is live; poll() re-sends if it stalls, and stageChunk() fills a per-chunk
  // bitmap so a re-send only backfills the gaps.
  void requestHistory() {
    if (!haveMasterMac_ || paired_ == 0) return;
    allocHistBuffers();  // safety: normally already done in begin()
    if (!fineStage_ || !coarseStage_ || !fineGot_ || !coarseGot_) return;  // OOM
    fineTotal_ = coarseTotal_ = 0;
    memset(fineGot_, 0, kFineChunks);      // pointer now — size explicitly, NOT sizeof(ptr)
    memset(coarseGot_, 0, kCoarseChunks);
    HistPointW na;  // init staging to n/a so not-yet-received points render as gaps
    na.battery = na.solar = na.charger = na.dcdc = na.load = na.soc = -32768;
    for (uint16_t i = 0; i < kHistFineMax; ++i) fineStage_[i] = na;
    for (uint16_t i = 0; i < kHistCoarseMax; ++i) coarseStage_[i] = na;
    histReady_ = false;
    histActive_ = true;
    histAttempts_ = 0;
    lastChunkMs_ = 0;
    sendHistReq();
  }

 private:
  static const uint32_t kHopMs = 250;
  static const uint8_t kMaxChannel = 13;

  void setChannel(uint8_t ch) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    channel_ = ch;
  }

  void load() {
    Preferences p;
    p.begin(ns_, true);
    paired_ = p.getUInt("master", 0);
    p.end();
  }
  void save(uint32_t id) {
    Preferences p;
    p.begin(ns_, false);
    p.putUInt("master", id);
    p.end();
    paired_ = id;
  }

  // Minimal work in the callback: note the frame; accept data only from our
  // paired master; defer NVS writes to servicePairing() via adoptId_.
  void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (frameHook_) frameHook_(mac, data, len);  // app peek (OTA) — OTA frame sizes
                                                  // don't match ours, so we still fall through
    // Low-rate 7-day stats frame (Week page) — separate type, our master only.
    if (len == (int)sizeof(StatsFrame)) {
      StatsFrame f;
      memcpy(&f, data, sizeof(f));
      if (validStatsHeader(f) && paired_ != 0 && f.masterId == paired_) {
        stats_ = f;
        haveStats_ = true;
        lastStatsMs_ = millis();
      }
      return;
    }
    // Graph history chunk (pull reply) — stage it, our master only.
    if (len == (int)sizeof(HistChunk)) {
      HistChunk c;
      memcpy(&c, data, sizeof(c));
      if (validHistChunk(c) && histActive_ && paired_ != 0 && c.masterId == paired_)
        stageChunk(c);
      return;
    }
    if (len != (int)sizeof(Snapshot)) return;
    Snapshot s;
    memcpy(&s, data, sizeof(s));
    if (!validHeader(s)) return;
    uint32_t now = millis();
    heardMaster_ = s.masterId;
    heardFlags_ = s.flags;
    lastAnyMs_ = now;
    bool invite = (s.flags & F_PAIRING) != 0;
    // Adopt while the user's window is open and a master is inviting. Allow this
    // even when already paired, so hitting Pair re-homes to a NEW master without a
    // manual Unpair first; skip our current master so we don't needlessly re-adopt.
    if (adopting_ && invite && s.masterId != paired_) adoptId_ = s.masterId;
    if (paired_ != 0 && s.masterId == paired_) {
      if (haveFrame_) {
        uint16_t expected = (uint16_t)(lastSeq_ + 1);
        if (s.seq != expected) ++drops_;
      }
      lastSeq_ = s.seq;
      snap_ = s;
      haveFrame_ = true;
      lastRxMs_ = now;
      locked_ = true;
      if (mac) { memcpy(masterMac_, mac, 6); haveMasterMac_ = true; }  // for the history pull
    }
  }

  // Stage one received history chunk into the pull buffers + mark it received.
  void stageChunk(const HistChunk& c) {
    uint16_t cap = (c.ring == 0) ? kHistFineMax : kHistCoarseMax;
    HistPointW* dst = (c.ring == 0) ? fineStage_ : coarseStage_;
    uint8_t* got = (c.ring == 0) ? fineGot_ : coarseGot_;
    if (!dst || !got) return;      // staging not allocated (never happens in slave role)
    fineTotal_ = c.fineTotal;      // learn BOTH totals from any chunk, so a completed
    coarseTotal_ = c.coarseTotal;  // fine ring doesn't prematurely mark us "ready"
    if (c.offset + c.count > cap || c.count > kHistChunkPts) return;
    uint16_t idx = c.offset / kHistChunkPts;
    bool isNew = !got[idx];
    memcpy(&dst[c.offset], c.pts, c.count * sizeof(HistPointW));
    got[idx] = 1;
    lastChunkMs_ = millis();
    // Forward progress (a chunk we didn't have) refreshes the give-up budget, so a
    // weak-but-alive link that keeps trickling in new chunks never exhausts its
    // retries and resets to zero — the cap only trips on a genuinely stalled link.
    if (isNew) histAttempts_ = 0;
    recomputeHistReady();
  }

  static bool ringComplete(uint16_t total, const uint8_t* got, uint16_t maxChunks) {
    uint16_t need = (total + kHistChunkPts - 1) / kHistChunkPts;  // chunks required
    if (need > maxChunks) need = maxChunks;
    for (uint16_t i = 0; i < need; ++i) if (!got[i]) return false;
    return true;
  }
  void recomputeHistReady() {
    // total==0 => that ring is trivially complete (master has no samples yet).
    bool fineDone = ringComplete(fineTotal_, fineGot_, kFineChunks);
    bool coarseDone = ringComplete(coarseTotal_, coarseGot_, kCoarseChunks);
    if (fineDone && coarseDone) { histReady_ = true; histActive_ = false; }
  }
  static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len) {
    if (self_) self_->onRecv(mac, data, len);
  }

  void serviceAcquisition() {
    if (!manageWifi_) {
      // The caller's SoftAP owns the channel; just track it for the UI, no hop.
      uint8_t pc = 0; wifi_second_chan_t sc;
      if (esp_wifi_get_channel(&pc, &sc) == ESP_OK && pc) channel_ = pc;
      return;
    }
    uint32_t now = millis();
    if (paired_ != 0) {
      bool stale = !haveFrame_ || (now - lastRxMs_) > kStaleMs;
      if (stale) locked_ = false;
    } else {
      // Hold the channel only while an *inviting* master is being heard here, so
      // we don't park on a silent/non-inviting master and miss the one pairing.
      bool invite = (heardFlags_ & F_PAIRING) && (now - lastAnyMs_) < kStaleMs;
      locked_ = adopting_ && invite;
    }
    if (locked_) return;
    if (now - lastHopMs_ < kHopMs) return;
    lastHopMs_ = now;
    setChannel(channel_ >= kMaxChannel ? 1 : channel_ + 1);
  }

  void sendHistReq() {
    ensureHistPeer();
    HistReq r;
    fillHistReq(r, paired_);
    esp_now_send(masterMac_, (const uint8_t*)&r, sizeof(r));
    lastHistReqMs_ = millis();
  }
  void ensureHistPeer() {
    if (histPeerAdded_) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, masterMac_, 6);
    p.channel = 0;      // current channel
    p.encrypt = false;
    p.ifidx = manageWifi_ ? WIFI_IF_STA : WIFI_IF_AP;
    if (esp_now_add_peer(&p) == ESP_OK) histPeerAdded_ = true;
  }
  // Resilient, resumable pull: re-send the request whenever progress stalls — a
  // lost request (no chunk at all) is nudged quickly, a mid-transfer stall a bit
  // slower. The master restarts from offset 0 but our per-chunk bitmap keeps all
  // prior progress, so each pass only backfills the gaps and it always converges.
  void serviceHistory() {
    if (!histActive_ || histReady_) return;
    uint32_t idle = millis() - (lastChunkMs_ ? lastChunkMs_ : lastHistReqMs_);
    uint32_t timeout = lastChunkMs_ ? kHistStallMs : kHistReqLostMs;
    if (idle < timeout) return;
    if (histAttempts_ >= kHistMaxAttempts) { histActive_ = false; return; }
    ++histAttempts_;
    sendHistReq();
  }

  void servicePairing() {
    if (unpairReq_) {
      unpairReq_ = false;
      save(0);
      adopting_ = false;
      haveFrame_ = false;
    }
    if (startAdopt_) {
      startAdopt_ = false;
      adopting_ = true;
      adoptStartMs_ = millis();
    }
    if (adopting_ && (millis() - adoptStartMs_) > kAdoptMs) adopting_ = false;
    uint32_t adopt = adoptId_;
    if (adopt != 0) {
      adoptId_ = 0;
      save(adopt);
      adopting_ = false;
      haveFrame_ = false;      // fresh seq/staleness for the new master
      drops_ = 0;
      haveMasterMac_ = false;  // re-learn the new master's MAC before any history pull
      histActive_ = false;     // drop any in-flight/complete pull from the old master
      histReady_ = false;
      // Drop the ESP-NOW unicast peer for the OLD master, else ensureHistPeer()
      // (guarded by histPeerAdded_) never registers the NEW master's MAC and every
      // HistReq esp_now_send fails silently — the history sync stalls until a
      // reboot clears the flag. masterMac_ still holds the old MAC here.
      if (histPeerAdded_) { esp_now_del_peer(masterMac_); histPeerAdded_ = false; }
    }
  }

  inline static Receiver* self_ = nullptr;
  void (*frameHook_)(const uint8_t*, const uint8_t*, int) = nullptr;
  const char* ns_ = "vicslave";
  bool ok_ = false;
  bool manageWifi_ = true;

  // received-frame state
  volatile bool haveFrame_ = false;
  Snapshot snap_ = {};
  volatile uint32_t lastRxMs_ = 0;
  volatile uint16_t lastSeq_ = 0;
  volatile uint32_t drops_ = 0;
  volatile uint32_t lastAnyMs_ = 0;
  volatile uint32_t heardMaster_ = 0;
  volatile uint8_t heardFlags_ = 0;
  StatsFrame stats_ = {};
  volatile bool haveStats_ = false;
  volatile uint32_t lastStatsMs_ = 0;

  // pairing state
  uint32_t paired_ = 0;
  volatile uint32_t adoptId_ = 0;
  volatile bool startAdopt_ = false;
  volatile bool unpairReq_ = false;
  volatile bool adopting_ = false;  // read in the RX callback, written in loop
  uint32_t adoptStartMs_ = 0;

  // channel acquisition
  uint8_t channel_ = 1;
  uint32_t lastHopMs_ = 0;
  volatile bool locked_ = false;  // written in both the RX callback and loop

  // Graph history pull (staging buffers + per-chunk received bitmaps).
  static const uint16_t kFineChunks = (kHistFineMax + kHistChunkPts - 1) / kHistChunkPts;
  static const uint16_t kCoarseChunks = (kHistCoarseMax + kHistChunkPts - 1) / kHistChunkPts;
  static const uint32_t kHistReqLostMs = 4000;  // no chunk yet -> request likely lost, resend
  static const uint32_t kHistStallMs = 8000;    // mid-transfer stall -> nudge a resend
  static const uint8_t kHistMaxAttempts = 20;   // plenty of passes; each backfills gaps
  // Graph-pull staging (~26KB) is heap-allocated ONLY in the slave role (from
  // begin()/allocHistBuffers), so a master (esp. the no-PSRAM M5Capsule) keeps that
  // RAM free — it never calls begin() and these stay null.
  HistPointW* fineStage_ = nullptr;
  HistPointW* coarseStage_ = nullptr;
  uint8_t* fineGot_ = nullptr;
  uint8_t* coarseGot_ = nullptr;
  void allocHistBuffers() {
    if (fineStage_) return;
    fineStage_ = new (std::nothrow) HistPointW[kHistFineMax];
    coarseStage_ = new (std::nothrow) HistPointW[kHistCoarseMax];
    fineGot_ = new (std::nothrow) uint8_t[kFineChunks]();
    coarseGot_ = new (std::nothrow) uint8_t[kCoarseChunks]();
  }
  // Shared "no data" point returned by finePoint/coarsePoint before staging exists.
  static const HistPointW& kNaPoint() {
    static const HistPointW na = { -32768, -32768, -32768, -32768, -32768, -32768 };
    return na;
  }
  volatile uint16_t fineTotal_ = 0, coarseTotal_ = 0;
  volatile bool histActive_ = false;
  volatile bool histReady_ = false;
  uint8_t histAttempts_ = 0;
  uint32_t lastHistReqMs_ = 0;
  volatile uint32_t lastChunkMs_ = 0;  // last chunk arrival (drives the resilient retry)
  uint8_t masterMac_[6] = {0};
  volatile bool haveMasterMac_ = false;
  bool histPeerAdded_ = false;
};

}  // namespace slavelink
#endif  // ESP32
