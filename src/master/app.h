#pragma once
// Shared application contract for the master firmware. main.cpp used to be a
// single ~3000-line translation unit; it is being split into cohesive .cpp
// files (web / espnow / ble_ingest / display) that all include this header for
// the cross-module state, types and function prototypes. Symbols used within
// only one .cpp stay `static` in that file and are NOT declared here.

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "DeviceConfig.h"
#include "Profiles.h"
#include "Registry.h"
#include "Signals.h"
#include "OtaLink.h"
#include "SlaveLink.h"
#include "SlaveReceiver.h"
#include "Stats.h"
#include "VictronTypes.h"

// Board selection: the master/slave ROLE is chosen at runtime (NVS flag), and so
// now is the DISPLAY hardware. The universal `s3` image compiles BOTH display
// backends in and picks the right one at boot (detectBoard(), below) so ONE image
// — and the OTA firmware-clone that pushes it master->slave — serves every board:
//   • Guition JC3248W535 — AXS15231B QSPI panel + I2C touch
//   • LilyGo T-Display-S3 — ST7789 8-bit-parallel panel + two buttons
// Legacy single-backend flags still work for the standalone reference envs:
// BOARD_GUITION / BOARD_LILYGO compile just one. Any compiled backend defines
// VICMON_DISPLAY, which guards all rendering; none = headless (bare S3 / AtomS3).
#if defined(BOARD_GUITION)
  #define VICMON_HAS_GUITION 1
#endif
#if defined(BOARD_LILYGO)
  #define VICMON_HAS_LILYGO 1
#endif
#if defined(VICMON_HAS_GUITION) || defined(VICMON_HAS_LILYGO)
  #define VICMON_DISPLAY 1
#endif
#ifdef VICMON_HAS_GUITION
  #include <GuitionDisplay.h>
  #include <GuitionTouch.h>
  #include <GfxDashboard.h>
#endif
#ifdef VICMON_HAS_LILYGO
  #include <LilygoDisplay.h>
#endif

// Runtime-detected display hardware, set by detectBoard() in setup() BEFORE the
// role branch (so both master and slave bring-ups see it). The universal image
// probes for the Guition's I2C touch controller to tell the two boards apart; a
// single-backend build resolves at compile time. Overridable via NVS ("vicboard").
enum HwBoard : uint8_t { HW_HEADLESS = 0, HW_GUITION = 1, HW_LILYGO = 2 };
extern uint8_t gHwBoard;
void detectBoard();

// ---- device role (Master / Slave) ------------------------------------------
enum { ROLE_MASTER = 0, ROLE_SLAVE = 1 };

// Battery charge state, from the resolved battery current vs the idle deadband.
enum class ChargeMode { Unknown, Charging, Discharging, Idle };

// ---- shared types ----------------------------------------------------------

// Victron devices seen but not configured (no matching key).
struct Discovered {
    char mac[20] = {0};
    char name[24] = {0};  // BLE advertised (friendly) name, if any
    uint16_t model = 0;
    int rssi = 0;
    uint32_t lastSeenMs = 0;
    uint8_t raw[16] = {0};  // encrypted advert payload, captured in debug mode
    uint8_t rawLen = 0;     // 0 = not captured
};

// One canonical snapshot of the live panel, resolved from the registry ONCE per
// consumer. The three serializers — the ESP-NOW Snapshot (buildSnapshot), the web
// panel JSON (buildPanelJson) and the LCD DashData (collectDash) — all read from
// this, so which signal/derived-sentinel/secondary-field a value comes from lives
// in exactly one place and can't drift between them.
struct PanelModel {
    sig::Resolved soc, battV, battA, consumed, starterV, ttg;
    sig::Resolved solarA, solarW, solarV;
    sig::Resolved chargerA;
    sig::Resolved dcdcInA, dcdcOutA, dcdcInV, dcdcOutV;
    sig::Resolved loadA;
    bool loadDerived = false;
    ChargeMode mode = ChargeMode::Unknown;
    int alertWorst = 0;
    float capacity = 0;  // battery Ah (0 = unknown)
};

// One time-series sample + ring buffer. Two rings (fine 5 s/1 h, coarse 60 s/24 h)
// are written by the loop (sampleHistory) and read by the display (collectHistory)
// and the web history JSON (buildHistoryJson).
struct HistSample {
    int16_t battery, solar, charger, dcdc, load;  // deci-amps, -32768 = n/a
    int16_t soc;                                   // deci-percent (0..1000), -32768 = n/a
};
struct HistRing {
    HistSample* buf = nullptr;
    size_t cap = 0, head = 0, count = 0;
    uint32_t intervalMs = 0, lastMs = 0;
    void init(HistSample* b, size_t c, uint32_t iv) { buf = b; cap = c; intervalMs = iv; }
    void clear() { head = count = 0; lastMs = 0; }
    bool due(uint32_t now) const { return count == 0 || now - lastMs >= intervalMs; }
    void push(const HistSample& s, uint32_t now) {
        lastMs = now;
        buf[head] = s;
        head = (head + 1) % cap;
        if (count < cap) ++count;
    }
};

// ---- shared globals (defined in main.cpp) ----------------------------------

extern char kApSsid[24];        // Vicmon-<mac3> (or custom), filled at boot
extern char kApPass[24];        // default vicmon1234 (or custom)
extern const char* kFwVersion;

// Guards structural mutation of the device registry (gConfig add/remove/clear,
// gSignals/profile reload) done on the AsyncTCP web task against the loop task
// iterating the same slots (pollBle->ingest, resolveField, collectPanel, ...).
// Recursive so the loop can hold it while servicing a deferred profile switch
// that re-locks. Created in setup() before the server/tasks start; the RegLock
// guard no-ops until then (single-threaded), so it is always safe to use.
extern SemaphoreHandle_t gRegMux;
struct RegLock {
    RegLock()  { if (gRegMux) xSemaphoreTakeRecursive(gRegMux, portMAX_DELAY); }
    ~RegLock() { if (gRegMux) xSemaphoreGiveRecursive(gRegMux); }
    RegLock(const RegLock&) = delete;
    RegLock& operator=(const RegLock&) = delete;
};

extern DeviceConfig gConfig;
extern sig::SignalMap gSignals;
extern ProfileManager gProfiles;
extern stats::Stats gStats;
extern NimBLEScan* gScan;
extern AsyncWebServer gServer;
extern DNSServer gDns;

extern Discovered gDisc[12];
extern size_t gDiscN;

extern uint32_t gMasterId;              // stable per-chip id (low 32b of efuse MAC)
extern volatile uint32_t gPairUntilMs;  // pairing window closes at this millis (0 = closed)
extern volatile bool gDebugCapture;     // capture raw bytes of unknown adverts
extern bool gEspNowOk;                  // ESP-NOW radio up
extern volatile uint16_t gSnapSeq;      // broadcast sequence counter (esp_timer task)

extern uint8_t gRole;                   // ROLE_MASTER / ROLE_SLAVE
extern volatile bool gRoleReq;          // request: toggle role + reboot
extern slavelink::Receiver gRx;         // ESP-NOW receiver, used only in slave role
extern slavelink::OtaEngine gOta;       // firmware-clone engine (master<->slave OTA push)

extern int gScanVictron, gScanDecoded;  // per-scan diagnostics

extern bool gFsOk;

extern float gBattCapacity;   // Ah, 0 = unknown
extern float gDeadband;       // A; |current| below this reads as idle
extern int gTzOffsetMin;      // local time offset from UTC, minutes (+10h AEST)
extern float gSocWarn;        // % — warn at/below
extern float gSocCrit;        // % — critical at/below
extern float gVlow;           // V — critical at/below
extern float gVhigh;          // V — critical at/above
extern String gStaSsid, gStaPass;
extern uint32_t gManualEpoch, gManualMillis;  // AP-set clock (see currentLocalEpoch)
extern volatile bool gRebootReq;              // set to reboot from a web handler (loop applies)

extern HistRing gFine, gCoarse;  // continuous history rings (backing arrays in main.cpp)

// ---- cross-module function prototypes --------------------------------------
// Defined in main.cpp (the data/registry core), called from web.cpp etc.
sig::Resolved R(sig::Role role, uint32_t now);
sig::Resolved resolveSignal(sig::Role role, uint32_t now);
PanelModel collectPanel(uint32_t now);
int buildAlerts(uint32_t now, String* outArr);
String buildHistoryJson(int mins);
ChargeMode chargeMode(const sig::Resolved& ba);
const char* chargeModeName(ChargeMode m);
const char* chargeModeDisplayName(ChargeMode m);
uint32_t currentLocalEpoch();
uint32_t currentUtcEpoch();      // best-known UTC seconds (0 = no clock)
void saveClock();                // snapshot the clock to NVS now
void serviceClockPersist();      // periodic clock snapshot (call from both loops)
victron::Record parseType(const String& t);
const char* typeName(victron::Record r);
void applyProfile(int pid);
void wipeProfile(int pid);
void saveSettings(float capacity, float deadband, int tzMin);
void saveAlertSettings(float socWarn, float socCrit, float vLow, float vHigh);
void saveWifiCreds(const String& s, const String& pw);
void loadWifi();
void saveApCfg(const String& ssid, const String& pass);
String settingsNs(int profile);
void saveHistFile(int profile);
void startPairing();
bool pairingActive();
int pairSecsLeft();
void serviceRole();
void hexInto(char* out, size_t n, const uint8_t* p, size_t len);
void summarizeDevice(const DeviceSlot& s, char* out, size_t n);

// Defined in ble_ingest.cpp, called from main.cpp.
void pollBle();

// Defined in espnow.cpp, called from main.cpp.
void setupEspNow();
void sendSlaveBroadcast();
void sendStatsFrame();
void serviceHistSend();  // master: paced reply to a slave's graph-history pull
void setupOta(uint8_t role);  // init the OTA engine for this role (loads allow-remote NVS)
void serviceOta();            // drive OTA + refresh the slave's paired-master id; call each loop
void saveOtaAllow(bool allow);// persist the allow-remote-update flag (NVS ns "vicota")

// Defined in web.cpp, called from main.cpp.
String jsonEsc(const String& s);
void setupServer();

#ifdef VICMON_DISPLAY
// The four public display entry points (called from main.cpp setup/loop/slaveLoop)
// live in display.cpp and dispatch on gHwBoard: the Guition path renders inline,
// the LilyGo path forwards to display_lilygo.cpp.
extern bool gDisplayOk;
void bringUpDisplay();
void publishDash();
void publishSlaveDash();
void serviceDashRequests();
#endif

#ifdef VICMON_HAS_LILYGO
// LilyGo T-Display-S3 display path (display_lilygo.cpp), dispatched from the shared
// entry points above when gHwBoard == HW_LILYGO.
bool lilygoBringUp();   // power + panel init; sets gDisplayOk. Returns false on fail.
void lilygoRender();    // redraw the current page from the live model (rate-limited)
void lilygoService();   // poll the two buttons (short/long press) + apply their actions
// Provided by display.cpp so the LilyGo renderer reuses the exact per-role data
// assembly the Guition path uses (identical pages), plus the shared graph window.
void collectDashForRole(guition::DashData& d);
void setGraphWindowMinutes(int mins);
#endif
