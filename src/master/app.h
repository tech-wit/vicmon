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

#include "DeviceConfig.h"
#include "Profiles.h"
#include "Registry.h"
#include "Signals.h"
#include "SlaveLink.h"
#include "SlaveReceiver.h"
#include "Stats.h"
#include "VictronTypes.h"

// Board selection: the display DRIVER is chosen at build time per board model,
// while the master/slave ROLE is chosen at runtime (NVS flag). This one app runs
// on every board — BOARD_GUITION selects the AXS15231B QSPI driver; BOARD_LILYGO
// is reserved for the T-Display-S3 (ST7789 + buttons, driver TBD, builds headless
// for now); no board flag = headless (no screen, e.g. the bare S3 / AtomS3). A
// selected display board defines VICMON_DISPLAY, which guards all rendering.
#if defined(BOARD_GUITION)
  #define VICMON_DISPLAY 1
  #include <GuitionDisplay.h>
  #include <GuitionTouch.h>
  #include <GfxDashboard.h>
#elif defined(BOARD_LILYGO)
  #warning "BOARD_LILYGO: display driver not implemented yet — building headless on the LilyGo"
#endif

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

// ---- shared globals (defined in main.cpp) ----------------------------------

extern char kApSsid[24];        // Vicmon-<mac3>, filled at boot
extern const char* kApPass;
extern const char* kFwVersion;

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
victron::Record parseType(const String& t);
const char* typeName(victron::Record r);
void applyProfile(int pid);
void wipeProfile(int pid);
void saveSettings(float capacity, float deadband, int tzMin);
void saveAlertSettings(float socWarn, float socCrit, float vLow, float vHigh);
void saveWifiCreds(const String& s, const String& pw);
void loadWifi();
String settingsNs(int profile);
void saveHistFile(int profile);
void startPairing();
bool pairingActive();
int pairSecsLeft();

// Defined in web.cpp, called from main.cpp.
String jsonEsc(const String& s);
void setupServer();
