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
  }

  // User triggers (raised from a button, touch, or serial command).
  void startAdopt() { startAdopt_ = true; }  // open the adopt window
  void unpair() { unpairReq_ = true; }       // forget the current master

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
  const Snapshot& snapshot() const { return snap_; }

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
  void onRecv(const uint8_t*, const uint8_t* data, int len) {
    if (len != (int)sizeof(Snapshot)) return;
    Snapshot s;
    memcpy(&s, data, sizeof(s));
    if (!validHeader(s)) return;
    uint32_t now = millis();
    heardMaster_ = s.masterId;
    heardFlags_ = s.flags;
    lastAnyMs_ = now;
    bool invite = (s.flags & F_PAIRING) != 0;
    if (adopting_ && paired_ == 0 && invite) adoptId_ = s.masterId;
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
    }
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
      haveFrame_ = false;  // fresh seq/staleness for the new master
    }
  }

  inline static Receiver* self_ = nullptr;
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
};

}  // namespace slavelink
#endif  // ESP32
