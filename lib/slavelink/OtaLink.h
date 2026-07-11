#pragma once
#include "SlaveLink.h"

// Firmware clone over ESP-NOW: push the running app image to the paired peer
// (master<->slave, either direction). Single instance, shared by both roles.
//
// Wire protocol lives in SlaveLink.h (OtaAnnounce/Accept/Data/Ctrl, kOtaProto).
// This engine drives both ends:
//   SOURCE:  startPush() -> OFFERING (broadcast announce) -> on Accept, SENDING
//            (stop-and-wait OtaData, one chunk outstanding, resend on ack timeout)
//            -> on Ctrl DONE/FAIL -> DONE/FAILED.
//   TARGET:  hears Announce (if allowed + not already this build) -> RECEIVING
//            (Update.begin, write each chunk, ACK) -> on last byte, Update.end
//            validates the appended SHA-256 -> DONE (reboot) / FAILED (abort).
//
// The callbacks (onFrame) run on the WiFi task and only stash small state; all
// flash work (esp_partition_read on the source, Update.write/end on the target)
// happens in service() on the loop task. Because the transfer is stop-and-wait,
// only one OtaData is ever in flight, so a single-slot buffer needs no lock: the
// callback fills it only while empty, service clears it only when done.
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <Update.h>
#include <esp_now.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_app_format.h>
#include <cstdlib>
#include <cstring>

namespace slavelink {

class OtaEngine {
 public:
  enum Phase : uint8_t { IDLE, OFFERING, SENDING, RECEIVING, DONE, FAILED };

  // localId = the masterId that filters/tags our OTA frames (master: its own id;
  // slave: its paired master's id). role: 0 master, 1 slave (announce/UI only).
  // ifidx: the interface we transmit on (both roles run a SoftAP here -> AP).
  void begin(uint32_t localId, uint8_t role, wifi_interface_t ifidx = WIFI_IF_AP) {
    localId_ = localId;
    role_ = role;
    ifidx_ = ifidx;
    computeBuild();  // our version + monotonic build serial (for the newness comparison)
  }
  void setLocalId(uint32_t id) { localId_ = id; }
  void setAllowRemote(bool a) { allowRemote_ = a; }
  bool allowRemote() const { return allowRemote_; }
  // App-supplied version string (kFwVersion); call before begin() so it isn't
  // overwritten by the app-descriptor version.
  void setVersion(const char* v) { strncpy(localVer_, v ? v : "", kOtaVerLen - 1); localVer_[kOtaVerLen - 1] = 0; }

  // ---- build identity (for the UI + newness comparison) ----
  uint32_t buildSerial() const { return buildSerial_; }
  const char* localVersion() const { return localVer_; }
  const char* builtStr() const { return builtStr_; }        // "Mmm dd HH:MM"
  const char* peerVersion() const { return peerVer_; }      // the other unit's version, once known
  // "newer" / "older" / "same" of the peer relative to us (empty until a peer is known).
  const char* peerRel() const {
    if (peerSerial_ == 0) return "";
    return peerSerial_ > buildSerial_ ? "newer" : (peerSerial_ < buildSerial_ ? "older" : "same");
  }
  // The paired peer's version was heard recently (version beacon or a transfer).
  bool peerKnown() const { return peerHeardMs_ != 0 && (millis() - peerHeardMs_) < kPeerStaleMs; }

  // ---- user trigger (source) ----
  // Begin offering our running firmware to the paired peer. Computes the image
  // size + build hash, then broadcasts an announce until a target accepts (or the
  // offer window lapses). Safe to call while IDLE/DONE/FAILED; no-op if a transfer
  // is already engaged.
  bool startPush() {
    if (phase_ == OFFERING || phase_ == SENDING || phase_ == RECEIVING) return false;
    runPart_ = esp_ota_get_running_partition();
    if (!runPart_) { fail("no running partition"); return false; }
    imageSize_ = runningImageSize(runPart_);
    if (imageSize_ == 0) { fail("bad image header"); return false; }
    chunkTotal_ = (uint16_t)((imageSize_ + kOtaChunk - 1) / kOtaChunk);
    if (!runningAppSha8(sha8_)) memset(sha8_, 0, 8);
    havePeer_ = false;
    acceptPending_ = false;
    phase_ = OFFERING;
    offerStartMs_ = millis();
    lastAnnounceMs_ = 0;
    snprintf(status_, sizeof(status_), "offering %uKB", (unsigned)(imageSize_ / 1024));
    return true;
  }
  void cancel() {
    if (phase_ == RECEIVING) Update.abort();
    phase_ = IDLE;
    pullActive_ = false;
    snprintf(status_, sizeof(status_), "cancelled");
  }

  // ---- user trigger (target/pull) ----
  // Ask the paired peer to update us — used from the OUT-OF-DATE unit. Broadcasts
  // our build serial; a peer that is genuinely newer responds by pushing (see the
  // OtaPull handler). While the window is open we accept that push even if the
  // "allow remote update" toggle is off, since the user explicitly asked for it.
  bool startPull() {
    if (phase_ == OFFERING || phase_ == SENDING || phase_ == RECEIVING) return false;
    pullActive_ = true;
    pullUntilMs_ = millis() + kPullWindowMs;
    lastPullMs_ = 0;
    phase_ = IDLE;
    snprintf(status_, sizeof(status_), "requesting update from the paired device…");
    return true;
  }

  // ---- feed frames from the single ESP-NOW recv callback (both roles) ----
  // Returns true if the frame was an OTA frame (consumed); false otherwise so the
  // caller can fall through to its own handlers (Snapshot / HistReq / ...).
  bool onFrame(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == (int)sizeof(OtaAnnounce) && validOta(data, kOtaAnnounceMagic1)) {
      OtaAnnounce a; memcpy(&a, data, sizeof(a));
      onAnnounce(mac, a);
      return true;
    }
    if (len == (int)sizeof(OtaAccept) && validOta(data, kOtaAcceptMagic1)) {
      OtaAccept a; memcpy(&a, data, sizeof(a));
      if (phase_ == OFFERING && a.masterId == localId_ && mac && !acceptPending_) {
        memcpy(pendingMac_, mac, 6);
        notePeer(a.buildSerial, a.ver);
        acceptPending_ = true;
      }
      return true;
    }
    if (len == (int)sizeof(OtaData) && validOta(data, kOtaDataMagic1)) {
      OtaData d; memcpy(&d, data, sizeof(d));  // copy out — the rx buffer may be unaligned
      if (phase_ == RECEIVING && d.masterId == localId_ && !haveData_ &&
          d.len <= kOtaChunk) {
        dataSeq_ = d.seq;
        dataLen_ = d.len;
        memcpy(dataBuf_, d.data, d.len);
        haveData_ = true;  // hand off to service() (barrier: buffer filled first)
      }
      return true;
    }
    if (len == (int)sizeof(OtaCtrl) && validOta(data, kOtaCtrlMagic1)) {
      OtaCtrl c; memcpy(&c, data, sizeof(c));
      if (phase_ == SENDING && c.masterId == localId_) {
        if (c.kind == OTA_ACK) { ackSeq_ = c.seq; ackFlag_ = true; }
        else if (c.kind == OTA_DONE) ctrlDone_ = true;
        else if (c.kind == OTA_FAIL || c.kind == OTA_BUSY) ctrlFail_ = true;
      }
      return true;
    }
    if (len == (int)sizeof(OtaPull) && validOta(data, kOtaPullMagic1)) {
      OtaPull p; memcpy(&p, data, sizeof(p));
      // Serve the pull only from an idle unit that is genuinely newer than the
      // requester (strictly greater build serial) — never push same/older.
      if ((phase_ == IDLE || phase_ == DONE || phase_ == FAILED) &&
          p.masterId == localId_ && buildSerial_ > p.buildSerial)
        pullServeReq_ = true;  // service() calls startPush() (does flash reads on the loop)
      return true;
    }
    if (len == (int)sizeof(OtaHello) && validOta(data, kOtaHelloMagic1)) {
      OtaHello hlo; memcpy(&hlo, data, sizeof(hlo));
      if (hlo.masterId == localId_) notePeer(hlo.buildSerial, hlo.ver);  // learn the peer's version at rest
      return true;
    }
    return false;
  }

  // ---- drive everything; call every loop (both roles). Non-blocking. ----
  void service() {
    // Periodic version beacon (both roles) so the paired peer's version is known at
    // rest — skipped mid-transfer to keep the air clear.
    if (!busy() && localId_ && millis() - lastHelloMs_ >= kHelloMs) { lastHelloMs_ = millis(); sendHello(); }
    if (pullServeReq_) { pullServeReq_ = false; startPush(); }  // a newer unit answering a pull
    if (announceRx_) { onAcceptAnnounce(); return; }  // a heard offer -> open RECEIVING
    switch (phase_) {
      case OFFERING:   serviceOffer();   break;
      case SENDING:    serviceSend();    break;
      case RECEIVING:  serviceReceive(); break;
      default: if (pullActive_) servicePull(); break;  // waiting for a newer peer to answer
    }
  }

  // ---- state for the loop / UI ----
  Phase phase() const { return phase_; }
  bool offering() const { return phase_ == OFFERING; }
  // Transfer engaged — the loop should suspend BLE/other heavy work while true.
  bool busy() const { return phase_ == SENDING || phase_ == RECEIVING; }
  uint8_t percent() const {
    if (phase_ == SENDING && chunkTotal_) return (uint8_t)((uint32_t)srcSeq_ * 100 / chunkTotal_);
    if (phase_ == RECEIVING && tgtTotal_) return (uint8_t)((uint32_t)tgtExpect_ * 100 / tgtTotal_);
    if (phase_ == DONE) return 100;
    return 0;
  }
  const char* statusText() const { return status_; }

 private:
  static const uint32_t kOfferMs = 180000;    // give up offering after 3 min, no taker
  static const uint32_t kAnnounceMs = 500;     // re-broadcast the offer this often
  static const uint32_t kAckWaitMs = 600;      // resend a chunk if unacked this long
  static const uint8_t kMaxRetry = 40;         // per-chunk resend cap before giving up
  static const uint32_t kFinalWaitMs = 12000;  // await target's DONE after the last chunk
  static const uint32_t kRecvTimeoutMs = 15000;// abort a receive stalled this long
  static const uint32_t kPullWindowMs = 30000; // how long a pull request keeps asking
  static const uint32_t kHelloMs = 10000;      // version-beacon broadcast interval
  static const uint32_t kPeerStaleMs = 40000;  // peer version considered stale after this

  void fail(const char* why) {
    phase_ = FAILED;
    snprintf(status_, sizeof(status_), "failed: %s", why);
  }

  // ---- target: an offer was heard (callback context — stash only, no flash) ----
  void onAnnounce(const uint8_t* mac, const OtaAnnounce& a) {
    if (phase_ != IDLE && phase_ != DONE && phase_ != FAILED) return;  // busy — ignore
    // Accept a push if we allow remote updates, OR if we explicitly asked (pull).
    if ((!allowRemote_ && !pullActive_) || a.masterId != localId_ || !mac) return;
    if (a.imageSize == 0 || a.chunkTotal == 0 || announceRx_) return;
    memcpy(pendingMac_, mac, 6);
    memcpy(annSha8_, a.sha8, 8);  // build-skip check deferred to the loop (flash read)
    notePeer(a.buildSerial, a.ver);
    tgtSize_ = a.imageSize;
    tgtTotal_ = a.chunkTotal;
    tgtSrcRole_ = a.srcRole;
    announceRx_ = true;  // service() opens the receive on the loop task
  }

  void serviceOffer() {
    uint32_t now = millis();
    if (acceptPending_) {  // a target accepted — engage the transfer
      acceptPending_ = false;
      if (!ensurePeer(pendingMac_)) { fail("peer add"); return; }
      memcpy(peerMac_, pendingMac_, 6);
      havePeer_ = true;
      srcSeq_ = 0; srcWaiting_ = false; srcRetry_ = 0;
      ackFlag_ = false; ctrlDone_ = false; ctrlFail_ = false;
      phase_ = SENDING;
      snprintf(status_, sizeof(status_), "sending to %s (%s) 0/%u",
               peerVer_[0] ? peerVer_ : "peer", peerRel(), chunkTotal_);
      return;
    }
    if (now - offerStartMs_ > kOfferMs) { phase_ = IDLE; snprintf(status_, sizeof(status_), "no taker"); return; }
    if (now - lastAnnounceMs_ >= kAnnounceMs) {
      lastAnnounceMs_ = now;
      sendAnnounce();
    }
  }

  void sendAnnounce() {
    OtaAnnounce a = {};
    a.magic0 = kMagic0; a.magic1 = kOtaAnnounceMagic1; a.otaProto = kOtaProto;
    a.srcRole = role_;
    a.masterId = localId_;
    a.imageSize = imageSize_;
    a.chunkTotal = chunkTotal_;
    a.chunkSize = kOtaChunk;
    memcpy(a.sha8, sha8_, 8);
    a.buildSerial = buildSerial_;
    strncpy(a.ver, localVer_, kOtaVerLen);
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ensurePeer(bcast);  // a slave source has no broadcast peer registered otherwise
    esp_now_send(bcast, (const uint8_t*)&a, sizeof(a));
  }

  void serviceSend() {
    uint32_t now = millis();
    if (ctrlFail_) { fail("target rejected"); return; }
    if (ctrlDone_) { phase_ = DONE; snprintf(status_, sizeof(status_), "done"); return; }

    if (srcSeq_ >= chunkTotal_) {  // all chunks acked — await the target's DONE
      if (now - srcSendMs_ > kFinalWaitMs) fail("no final ack");
      return;
    }
    if (srcWaiting_) {
      if (ackFlag_ && ackSeq_ == srcSeq_) {  // this chunk landed — advance
        ackFlag_ = false;
        srcSeq_++;
        srcWaiting_ = false;
        srcRetry_ = 0;
        snprintf(status_, sizeof(status_), "sending %u/%u", srcSeq_, chunkTotal_);
        return;
      }
      if (now - srcSendMs_ > kAckWaitMs) {  // ack timed out — resend the same chunk
        if (++srcRetry_ > kMaxRetry) { fail("chunk retries"); return; }
        sendChunk(srcSeq_);
        srcSendMs_ = now;
      }
      return;
    }
    // Send the next chunk.
    sendChunk(srcSeq_);
    srcWaiting_ = true;
    srcSendMs_ = now;
  }

  void sendChunk(uint16_t seq) {
    OtaData d = {};
    d.magic0 = kMagic0; d.magic1 = kOtaDataMagic1; d.otaProto = kOtaProto;
    d.masterId = localId_;
    d.seq = seq;
    uint32_t off = (uint32_t)seq * kOtaChunk;
    uint32_t remain = imageSize_ > off ? imageSize_ - off : 0;
    uint16_t n = remain > kOtaChunk ? kOtaChunk : (uint16_t)remain;
    d.len = n;
    if (n && esp_partition_read(runPart_, off, d.data, n) != ESP_OK) { fail("flash read"); return; }
    esp_now_send(peerMac_, (const uint8_t*)&d, sizeof(d));
  }

  // ---- target ----
  void serviceReceive() {
    uint32_t now = millis();
    if (rebooting_) {  // sent DONE — give the ack a moment to leave, then reboot
      if (now - rebootMs_ > 400) { delay(50); ESP.restart(); }
      return;
    }
    if (haveData_) {
      uint16_t seq = dataSeq_, len = dataLen_;
      if (seq == tgtExpect_) {
        if (Update.write(dataBuf_, len) != len) { Update.abort(); sendCtrl(OTA_FAIL, seq); fail("write"); haveData_ = false; return; }
        tgtExpect_++;
        tgtBytes_ += len;
        tgtLastMs_ = now;
        sendCtrl(OTA_ACK, seq);
        snprintf(status_, sizeof(status_), "receiving %u/%u", tgtExpect_, tgtTotal_);
        if (tgtBytes_ >= tgtSize_) {  // whole image in — validate + commit
          if (Update.end(true)) {
            sendCtrl(OTA_DONE, seq);
            phase_ = DONE;
            snprintf(status_, sizeof(status_), "done — rebooting");
            rebooting_ = true; rebootMs_ = now;
          } else {
            sendCtrl(OTA_FAIL, seq);
            fail("validate");
          }
        }
      } else if (seq < tgtExpect_) {
        sendCtrl(OTA_ACK, seq);  // duplicate — our ack was lost; re-ack
      }
      // seq > tgtExpect_ (gap): ignore; source resends on its ack timeout.
      haveData_ = false;
      return;
    }
    if (now - tgtLastMs_ > kRecvTimeoutMs) { Update.abort(); fail("stalled"); }
  }

  // Broadcast our pull request until a newer peer answers or the window lapses.
  void servicePull() {
    uint32_t now = millis();
    if (now > pullUntilMs_) { pullActive_ = false; snprintf(status_, sizeof(status_), "no newer firmware offered"); return; }
    if (now - lastPullMs_ >= kAnnounceMs) { lastPullMs_ = now; sendPull(); }
  }
  void sendPull() {
    OtaPull p = {};
    p.magic0 = kMagic0; p.magic1 = kOtaPullMagic1; p.otaProto = kOtaProto;
    p.role = role_;
    p.masterId = localId_;
    p.buildSerial = buildSerial_;
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ensurePeer(bcast);
    esp_now_send(bcast, (const uint8_t*)&p, sizeof(p));
  }
  void sendHello() {
    OtaHello h = {};
    h.magic0 = kMagic0; h.magic1 = kOtaHelloMagic1; h.otaProto = kOtaProto;
    h.role = role_;
    h.masterId = localId_;
    h.buildSerial = buildSerial_;
    strncpy(h.ver, localVer_, kOtaVerLen);
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ensurePeer(bcast);
    esp_now_send(bcast, (const uint8_t*)&h, sizeof(h));
  }
  // Record the paired peer's version/build (from a beacon, announce or accept).
  void notePeer(uint32_t serial, const char* ver) {
    peerSerial_ = serial;
    memcpy(peerVer_, ver, kOtaVerLen); peerVer_[kOtaVerLen - 1] = 0;
    peerHeardMs_ = millis();
  }

  // Open a receive in response to a heard announce (loop task: does Update.begin).
  void onAcceptAnnounce() {
    announceRx_ = false;
    pullActive_ = false;  // our pull was answered — stop requesting
    // Already running this exact build? Ignore silently (no reflash). Done here on
    // the loop task, not the recv callback, since it reads the app desc from flash.
    uint8_t mine[8];
    if (runningAppSha8(mine) && memcmp(mine, annSha8_, 8) == 0) return;
    if (!ensurePeer(pendingMac_)) { fail("peer add"); return; }
    memcpy(peerMac_, pendingMac_, 6);
    if (!Update.begin(tgtSize_)) { fail("begin"); return; }
    tgtExpect_ = 0; tgtBytes_ = 0; tgtLastMs_ = millis();
    haveData_ = false; rebooting_ = false;
    phase_ = RECEIVING;
    snprintf(status_, sizeof(status_), "receiving %s (%s) 0/%u",
             peerVer_[0] ? peerVer_ : "peer", peerRel(), tgtTotal_);
    sendAccept();  // tell the source to start streaming
  }

  void sendAccept() {
    OtaAccept a = {};
    a.magic0 = kMagic0; a.magic1 = kOtaAcceptMagic1; a.otaProto = kOtaProto;
    a.tgtRole = role_;
    a.masterId = localId_;
    a.buildSerial = buildSerial_;
    strncpy(a.ver, localVer_, kOtaVerLen);
    esp_now_send(peerMac_, (const uint8_t*)&a, sizeof(a));
  }

  void sendCtrl(uint8_t kind, uint16_t seq) {
    OtaCtrl c = {};
    c.magic0 = kMagic0; c.magic1 = kOtaCtrlMagic1; c.otaProto = kOtaProto;
    c.kind = kind;
    c.masterId = localId_;
    c.seq = seq;
    esp_now_send(peerMac_, (const uint8_t*)&c, sizeof(c));
  }

  bool ensurePeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;  // current channel
    p.encrypt = false;
    p.ifidx = ifidx_;
    esp_err_t e = esp_now_add_peer(&p);
    return e == ESP_OK || e == ESP_ERR_ESPNOW_EXIST;
  }

  // Walk the running app image header to get its exact byte length (partition may
  // be larger than the image). Standard esp_image layout: header, then N segments
  // (each an 8-byte header + data), a 1-byte checksum padded to 16, and an
  // optional 32-byte SHA-256 when hash_appended.
  // Our version string + a monotonic build serial, from the running app's
  // descriptor (the toolchain embeds __DATE__/__TIME__ there every build, so no
  // manual bumping is needed for the newness comparison).
  void computeBuild() {
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(esp_ota_get_running_partition(), &d) != ESP_OK) return;
    buildSerial_ = parseBuildSerial(d.date, d.time);
    snprintf(builtStr_, sizeof(builtStr_), "%.11s %.5s", d.date, d.time);  // "Mmm dd HH:MM"
    if (localVer_[0] == 0) { strncpy(localVer_, d.version, kOtaVerLen - 1); localVer_[kOtaVerLen - 1] = 0; }
  }
  // Fold the compile date "Mmm dd yyyy" + time "hh:mm:ss" into a strictly
  // increasing 32-bit serial (approx seconds since 2020) — comparable, not a true
  // epoch. Later build => larger value, which is all the newness check needs.
  static uint32_t parseBuildSerial(const char* date, const char* time) {
    static const char* kMon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mon = 0;
    for (int i = 0; i < 12; ++i) if (strncmp(date, kMon + i * 3, 3) == 0) { mon = i + 1; break; }
    int day = atoi(date + 4), year = atoi(date + 7);
    int hh = atoi(time), mm = atoi(time + 3), ss = atoi(time + 6);
    if (year < 2020) year = 2020;
    uint32_t days = (uint32_t)(year - 2020) * 372 + (uint32_t)(mon > 0 ? mon - 1 : 0) * 31 + (uint32_t)(day > 0 ? day - 1 : 0);
    return days * 86400u + (uint32_t)hh * 3600u + (uint32_t)mm * 60u + (uint32_t)ss;
  }

  // Head of the running app's ELF SHA-256 (build fingerprint) — used to skip a
  // push of the identical build. esp_ota_get_partition_description is available
  // across arduino-esp32 versions (unlike esp_app_get_description).
  static bool runningAppSha8(uint8_t out[8]) {
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(esp_ota_get_running_partition(), &d) != ESP_OK) return false;
    memcpy(out, d.app_elf_sha256, 8);
    return true;
  }

  static uint32_t runningImageSize(const esp_partition_t* part) {
    esp_image_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) return 0;
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) return 0;
    uint32_t off = sizeof(esp_image_header_t);
    for (int i = 0; i < hdr.segment_count; ++i) {
      esp_image_segment_header_t sh;
      if (esp_partition_read(part, off, &sh, sizeof(sh)) != ESP_OK) return 0;
      off += sizeof(esp_image_segment_header_t) + sh.data_len;
      if (off > part->size) return 0;  // malformed
    }
    off += 1;                       // checksum byte
    off = (off + 15) & ~((uint32_t)15);  // pad to 16
    if (hdr.hash_appended) off += 32;    // appended SHA-256
    return off;
  }

  uint32_t localId_ = 0;
  uint8_t role_ = 0;
  wifi_interface_t ifidx_ = WIFI_IF_AP;
  bool allowRemote_ = true;
  volatile Phase phase_ = IDLE;
  char status_[48] = "idle";

  // build identity
  uint32_t buildSerial_ = 0;
  char localVer_[kOtaVerLen] = {0};
  char builtStr_[24] = {0};
  uint32_t peerSerial_ = 0;
  char peerVer_[kOtaVerLen] = {0};
  uint32_t peerHeardMs_ = 0;
  uint32_t lastHelloMs_ = 0;

  // source
  const esp_partition_t* runPart_ = nullptr;
  uint32_t imageSize_ = 0;
  uint16_t chunkTotal_ = 0;
  uint8_t sha8_[8] = {0};
  uint8_t peerMac_[6] = {0};
  bool havePeer_ = false;
  uint32_t offerStartMs_ = 0, lastAnnounceMs_ = 0;
  volatile bool acceptPending_ = false;
  uint8_t pendingMac_[6] = {0};
  uint16_t srcSeq_ = 0;
  bool srcWaiting_ = false;
  uint32_t srcSendMs_ = 0;
  uint8_t srcRetry_ = 0;
  volatile bool ackFlag_ = false, ctrlDone_ = false, ctrlFail_ = false;
  volatile uint16_t ackSeq_ = 0;

  // target
  uint32_t tgtSize_ = 0;
  uint16_t tgtTotal_ = 0, tgtExpect_ = 0;
  uint32_t tgtBytes_ = 0, tgtLastMs_ = 0;
  uint8_t tgtSrcRole_ = 0;
  uint8_t annSha8_[8] = {0};
  volatile bool announceRx_ = false;
  volatile bool haveData_ = false;
  volatile uint16_t dataSeq_ = 0, dataLen_ = 0;
  uint8_t dataBuf_[kOtaChunk];
  bool rebooting_ = false;
  uint32_t rebootMs_ = 0;

  // pull (target-initiated)
  volatile bool pullActive_ = false;   // we're asking a newer peer to update us
  volatile bool pullServeReq_ = false; // we're newer and a peer asked — start a push
  uint32_t pullUntilMs_ = 0, lastPullMs_ = 0;
};

}  // namespace slavelink
#endif  // ESP32
