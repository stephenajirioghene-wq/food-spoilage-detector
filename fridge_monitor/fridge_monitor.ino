/*
  fridge_monitor.ino  -  FRIDGE001 monitor firmware for the Smart Fridge app

  Implements the app team's "Smart Fridge MVP: Hardware Upload Requirements for
  Cooked Meals", raw-data structure v1. The board measures and uploads; the app makes
  the spoilage assessment. From 1.2.0 the board also shows its own alert (see below), as
  an extra field next to the unchanged raw data. Built on baseline_logger: same
  sampling, statistics, LittleFS logging, power-loss handling and NVS pointers.

  Every 2 s both MQ135 sensors are sampled. Every 60 s one record is appended to
  LittleFS and, once the clock is set, PUT to telemetry/FRIDGE001/latest. Every
  10 min (and on `u`) unsent records are PATCHed to
  telemetry/FRIDGE001/runs/<runId>/samples, together with the run metadata
  (experiments/<experimentId>/runs/<runId>) and restart events
  (experiments/<experimentId>/events/<runId>-restart).

  Wi-Fi setup: pressing the latching button on GPIO 4 opens the setup network
  SETUP_AP_SSID with a captive page at 192.168.4.1, where a phone hotspot or home
  network can be picked and saved to NVS (namespace "wifi"). Sampling and logging
  carry on; Firebase uploads pause until the portal closes. The board joins the
  saved network first and falls back to WIFI_SSID from secrets.h.

  Experiments (1.1.0): the same setup page starts and ends food experiments, adds
  observations and logs events. The app can start and end experiments through
  control/FRIDGE001/command; the board answers in control/FRIDGE001/ack/<commandId>.
  Page and command share startExperiment() / endExperiment() (Serial e/E removed). Every
  record they create goes to /outbox.jsonl first and is uploaded with the history.

  On-board spoilage alert (1.2.0): at experiment start each channel's baseline is the
  median Rs of the valid records of the last 15 min. Every record of the experiment is
  scored by how far Rs has fallen below it (log scale), shown on the LCD (FRESH / WARNING /
  SPOILT), sounded at Critical and uploaded as the extra field deviceAlert. Nothing 1.1.0
  uploads is changed, and devices/FRIDGE001/latest and decisionHistory are never written.
  Procedure: power on with the food OUT; after settling, the LCD shows CAL READY (one long
  beep) once 15 min of clean air are in the buffer; then put the food in and press Start.
  A second, record-only "food reference" (median Rs 30 to 45 min after the start) is also
  uploaded, with each record's ratio to it, to compare against the clean-air score later.

  The setup page lives in setup_page.html; make_setup_page.py (project folder) gzips it
  into setup_page.h. Run it again after editing the page.

  Arduino IDE Tools menu (1.2.0 does not fit the default 1.25 MB app partition):
    Board:                              ESP32 Dev Module (esp32 core 3.3.12)
    Partition Scheme:                   No OTA (2MB APP/2MB SPIFFS)
    Erase All Flash Before Sketch Upload: Disabled   (keeps NVS: counters, Wi-Fi, experiment)
  Changing the scheme from the default reformats LittleFS (dump with `d` and `r` first).
  LittleFS mounts on the "spiffs" partition label, as before.
  Libraries: LiquidCrystal I2C (Frank de Brabander) 1.1.2, ArduinoJson >= 7.0
             WiFi, HTTPClient, WiFiClientSecure, WebServer, DNSServer, Wire, LittleFS,
             Preferences come with the core.

  Serial (115200, line ending "Newline"):
    ?  status            u  upload now        d  dump all logs (BEGIN/END markers)
    x  show IDs and producer UID              r  dump the rejected file only
    (experiments: setup page or app only; e / E only point there)
    w  Wi-Fi status      W  forget the network saved with the setup page
    a  last 15 spoilage-alert evaluations
    A <pct>  TEST ONLY: force the experiment score to <pct> for the next 10 records

  Credentials come from secrets.h and are never printed.
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <map>
#include <vector>
#include <algorithm>
#include "esp_system.h"
#include "esp_sntp.h"
#include "secrets.h"
#include "setup_page.h"

// A secrets.h without the device user still compiles; uploads are then disabled.
#ifndef DEVICE_EMAIL
#define DEVICE_EMAIL ""
#endif
#ifndef DEVICE_PASSWORD
#define DEVICE_PASSWORD ""
#endif
// An older secrets.h without the setup network still compiles; the button then reports AP PW 8-13 CHARS.
#ifndef SETUP_AP_SSID
#define SETUP_AP_SSID "FRIDGE001-Setup"
#endif
#ifndef SETUP_AP_PASSWORD
#define SETUP_AP_PASSWORD ""
#endif

// ---------------- Identity ----------------
const char *DEVICE_ID        = "FRIDGE001";
const char *FIRMWARE_LABEL   = "fridge_monitor";
const char *FIRMWARE_VERSION = "1.2.0";
const int   SCHEMA_VERSION   = 1;
const char *CAL_VERSION      = "aug-2026-r0";   // R0 captured in August during warm-up, not yet validated
const char *CAL_SAMPLE_ID    = "NONE";          // sampleId in calibration mode (no meal sample)
const char *LATEST_PATH      = "telemetry/FRIDGE001/latest";
const char *RUNS_PATH        = "telemetry/FRIDGE001/runs";
const char *CONTROL_PATH     = "control/FRIDGE001";   // command (read, cleared) and ack/<commandId>
const char *REJECTED_PATH    = "/rejected.jsonl";     // records that can never be uploaded, with the reason
const char *OUTBOX_PATH      = "/outbox.jsonl";       // meta, observations, events and acks waiting for upload
const char *USED_IDS_PATH    = "/used_ids.txt";       // every experimentId started on this board
const char *DEFAULT_LOCATION = "refrigerator, same compartment as the device";

// ---------------- Pins ----------------
const int PIN_MQ1    = 34;  // MQ135 #1 (NH3 curve)
const int PIN_MQ2    = 35;  // MQ135 #2 (CO2 curve)
const int PIN_SDA    = 16;
const int PIN_SCL    = 17;
const int PIN_BUZZER = 27;  // HIGH = on
const int PIN_LED    = 33;  // HIGH = on
const int PIN_WIFI_BTN = 4; // latching Wi-Fi setup button to GND, INPUT_PULLUP: latched = LOW

// ---------------- Calibration (same as hw_selftest / baseline_logger) ----------------
const float RL1     = 2000.0f;
const float RL2     = 1370.0f;
const float R0_MQ1  = 7802.26f;
const float R0_MQ2  = 3463.12f;
const float VCC     = 5.0f;
const float ADC_REF = 3.3f;
const float ADC_MAX = 4095.0f;
const char *ADC_ATTENUATION = "11dB";
const float NH3_A = 102.2f,        NH3_B = -2.473f;
const float CO2_A = 116.6020682f,  CO2_B = -2.769034857f;

// ---------------- Timing ----------------
const uint32_t SAMPLE_PERIOD_MS  = 2000;                // one reading of each sensor
const uint32_t RECORD_PERIOD_MS  = 60000;               // one record to flash
const uint32_t LATEST_PERIOD_MS  = 60000;               // PUT telemetry/.../latest (= expectedUpdateSeconds)
const uint32_t HISTORY_PERIOD_MS = 10UL * 60 * 1000;    // batch upload of history, run meta and events
const uint32_t WIFI_TIMEOUT_MS   = 20000;
const uint32_t WIFI_RETRY_MS     = 60000;               // reconnect attempts while WiFi is down
const uint32_t GAP_LIMIT_MS      = 5000;                // longer sampling gaps go into gap_s
const int      CHUNK_RECORDS     = 10;                  // envelopes per PATCH request
const float    FS_PRUNE_FRACTION = 0.75f;               // delete uploaded segments above this
const uint32_t SEGMENT_S         = 6UL * 3600;          // one log file per 6 h of uptime
const uint32_t TOKEN_MARGIN_MS   = 5UL * 60 * 1000;     // refresh ID token this long before expiry
const uint32_t AUTH_RETRY_MS     = 10UL * 60 * 1000;    // wait after a rejected sign-in
const uint32_t SERIAL_LINE_TIMEOUT_MS = 60000;          // `W` confirmation gives up after this
const time_t   EPOCH_VALID       = 1700000000;          // anything earlier = clock not set
const uint32_t BTN_DEBOUNCE_MS   = 50;                  // button position must be stable this long
const uint32_t PORTAL_OPEN_MS    = 10UL * 60 * 1000;    // setup portal closes this long after opening
const uint32_t PORTAL_SAVED_MS   = 30000;               // ... or this long after a successful save
const uint32_t SAVE_TEST_MS      = 15000;               // limit for joining the network typed on the page
const uint32_t LCD_SCREEN_MS     = 4000;                // portal LCD alternates join details / status
const uint32_t LCD_FAIL_MS       = 10000;               // WIFI FAIL stays on the LCD this long
const uint32_t PORTAL_LED_MS     = 250;                 // LED fast blink while the portal is open
const uint32_t COMMAND_PERIOD_MS = 60000;               // read control/FRIDGE001/command, after latest
const uint32_t LCD_MSG_MS        = 5000;                // FOOD <id> / EXP ENDED stays on line 1
const int32_t  WAT_OFFSET_S      = 3600;                // West Africa Time = UTC+1, no DST
const int      ID_MIN_LEN = 3, ID_MAX_LEN = 32;         // experimentId, sampleId, commandId
const int      CMD_HISTORY       = 20;                  // commandIds remembered in NVS
const int      RUNID_MAX_TRIES   = 50;                  // runIds tried per check before giving up
const size_t   OUTBOX_KEEP_BYTES = 32768;               // a fully uploaded outbox above this is started afresh
const uint64_t FUTURE_SLACK_MS   = 10ULL * 60 * 1000;   // entered times may be this far past board time

// ---------------- Quality thresholds ----------------
const uint32_t SETTLE_S         = 1800;   // 30 min warm-up before readings count as valid
const uint32_t SETTLE_WARN_S    = 300;    // beep this long before settling ends: get the food ready
const float    NO_SIGNAL_V      = 0.05f;  // mean below: sensor board not connected
const float    NEAR_ADC_LIMIT_V = 3.2f;   // mean above: close to the ADC ceiling
const float    NOISE_V_SD       = 0.02f;  // within-minute SD above: high_noise (~4x clean-air max)

// ---------------- On-board spoilage alert (1.2.0) ----------------
// Starting values from one spoilt-rice run (R00062) and a few empty runs, NOT validated
// thresholds. Change them after a fresh-to-spoilt trial. pct = 100 * log10(Rs/Rs_baseline)
// / log10(FULL_SCALE_RATIO), clamped 0..100: Warning from Rs at 40% of baseline (2.5x fall),
// Critical from 16% (6.3x fall). Spoilt rice: MQ1 0.032 -> 100%, MQ2 0.233 -> 63%.
// Largest clean-air step seen: 0.72 -> 14%.
const char    *ALERT_METHOD            = "rs_drop_log_v1";
const int      BASELINE_WINDOW_RECORDS = 15;      // rolling buffer of one-minute records
const int      BASELINE_MIN_RECORDS    = 10;      // valid records a channel needs for a baseline
const uint32_t BASELINE_MAX_AGE_S      = 20 * 60; // older records do not count
const float    FULL_SCALE_RATIO        = 0.10f;   // Rs at this fraction of baseline scores 100%
const float    WARN_PCT                = 40.0f;
const float    CRIT_PCT                = 80.0f;
const float    HYSTERESIS_PCT          = 10.0f;   // a band drops only below its threshold minus this
const int      RISE_RECORDS            = 3;       // records in a row in/above a band to move up
const int      FALL_RECORDS            = 5;       // records in a row below threshold - hysteresis to move down
const uint32_t CRIT_REMIND_MS          = 10UL * 60 * 1000;   // 5 pulses again while Critical
const uint32_t WARN_LED_MS             = 1000;    // LED 1 s on / 1 s off in Warning
const uint32_t CRIT_LED_MS             = 200;     // LED 200 ms on / off in Critical
const int      ALERT_TEST_RECORDS      = 10;      // Serial `A <pct>`: records forced
const int      ALERT_LOG_N             = 15;      // Serial `a`: evaluations kept
// Food reference (record only, never used for the verdict): median Rs of the food's own
// early minutes, so each record also gives the ratio to the food when fresh. Taken from
// the 15 records ending FOOD_REF_DELAY_S after the start, once warm food has cooled.
const uint32_t FOOD_REF_DELAY_S        = 45 * 60; // window = minutes 30 to 45 after the start
const uint32_t FOOD_REF_LATE_S         = 60 * 60; // not taken this long after due: missed

// ---------------- Types ----------------
// Defined up here so Arduino's auto-generated prototypes can see them.
struct Stat {
  double sum = 0, sumSq = 0, mn = 0, mx = 0; int n = 0;
  void add(double v) {
    if (!n) { mn = mx = v; } else { if (v < mn) mn = v; if (v > mx) mx = v; }
    sum += v; sumSq += v * v; n++;
  }
  double mean() const { return n ? sum / n : 0; }
  double sd() const {
    if (n < 2) return 0;
    double m = mean(), var = (sumSq - n * m * m) / (n - 1);
    return var > 0 ? sqrt(var) : 0;
  }
};

struct SensorAcc {
  int pin; float rl, r0, a, b;
  Stat raw, volt, rs, ratio, ppm;
  int railCount = 0;     // readings at 0 or 4095 (uploaded as "rail")
  int railHigh = 0;      // readings at 4095 only (the "saturation" fault)
  void reset() { raw = Stat(); volt = Stat(); rs = Stat(); ratio = Stat(); ppm = Stat(); railCount = railHigh = 0; }
};

// One run = one recording session. A boot always starts a new run, and so does
// starting or ending an experiment. Records go to /f<run>_<seg>.jsonl (one segment per
// SEGMENT_S of uptime) and the run's metadata to /f<run>.m, keyed by the local run
// number. The runId is R<idNum>: idNum starts equal to run and moves up only if
// Firebase already holds that runId (checked before the run's first write).
struct RunInfo {
  uint32_t run = 0;
  uint32_t idNum = 0;             // number in the runId
  bool     idOk = false;          // runId checked free in Firebase (or already written by this run)
  String   etag;                  // ETag of the still-empty run record, for the first conditional PUT
  uint32_t boot = 0;
  String   reset = "UNKNOWN";
  String   startReason = "boot";  // "boot", "experiment_start", "experiment_end" (1.0.0: "experiment_change")
  uint32_t startUptime = 0;       // uptime_s when the run started (0 for a boot run)
  String   endReason;             // set when an experiment start/end closed the run without a reboot
  uint32_t endUptime = 0;
  bool     foodTest = false;
  String   expId, sampleId;
  uint64_t bootEpochMs = 0;       // 0 = this boot never had the clock set (yet)
  uint64_t syncedAtMs = 0;
  uint32_t sentUptime = 0;        // NVS "s<run>": uptime_s of the last uploaded record
  uint8_t  metaState = 0;         // NVS "m<run>": 0 not sent, 1 sent without time, 2 sent with time
  bool     eventSent = false;     // NVS "v<run>"
  String   wifiSource, wifiSsid;  // last Wi-Fi connection during the run ("saved" / "secrets")
  bool     metaDirty = false;     // run metadata must be PATCHed again (new connection, run ended)
  String   alertBaseline;         // 1.2.0: the experiment's baseline as JSON, for the run record
  String   foodRef;               // 1.2.0: its food reference as JSON, once taken
  std::vector<uint32_t> segs;     // segment numbers on flash, ascending
  uint32_t logged = 0, unsent = 0;
  uint32_t upSeg = 0; size_t upOffset = 0; bool upPosValid = false;  // first unsent byte
};

// ---- Spoilage alert (1.2.0) ----
enum AlertState { AL_OFF, AL_NO_BASELINE, AL_SETTLING, AL_FAULT, AL_NORMAL, AL_WARNING, AL_CRITICAL };

struct BaselineRec { uint32_t up = 0; float rs[2] = {0, 0}; int8_t q[2] = {-1, -1}; };   // one record in the buffer

// Per experiment, in NVS with the experimentId. A channel is usable with n >= BASELINE_MIN_RECORDS.
struct Baseline {
  bool any = false;               // at least one channel usable
  float rs[2] = {0, 0};           // median Rs, ohm
  uint8_t n[2] = {0, 0};          // valid records used per channel
  uint8_t window = 0;             // records of this boot within BASELINE_MAX_AGE_S at the start
  uint64_t fromMs = 0, toMs = 0;  // first / last qualifying record, Unix ms
  bool ok(int c) const { return n[c] >= BASELINE_MIN_RECORDS; }
};

enum FoodRefState { FR_NONE, FR_PENDING, FR_TAKEN, FR_MISSED };
struct FoodRef { int state = FR_NONE; uint64_t dueMs = 0; Baseline b; };   // NVS frSt frDue fr*

struct ChanEval {
  bool has = false;               // ratio computed (baseline usable and Rs defined)
  float ratio = 0, pct = 0;
  bool used = false, saturated = false;
  const char *reason = nullptr;   // why not used: no_baseline, settling, fault
};

struct Alert {
  AlertState state = AL_OFF;
  int band = 0;                   // 0 normal, 1 warning, 2 critical: the verdict, held across reboots (NVS)
  bool scored = false;            // the last record was scored
  float score = 0;                // its score, %
  bool test = false;              // the last record was forced by `A`
  ChanEval ch[2];
  uint32_t sinceUp = 0;           // uptime_s of this boot when the state began
  bool silenced = false;          // Critical reminders stopped for this episode (NVS)
  int rise[3] = {0, 0, 0}, fall[3] = {0, 0, 0};   // streaks per band threshold (index 1, 2)
  uint32_t lastRemindMs = 0;
};

struct AlertLog { uint32_t up = 0; uint64_t atMs = 0; bool scored = false; float score = 0; bool has[2] = {false, false};
                  float ratio[2] = {0, 0}; uint8_t state = 0; bool test = false; };

// ---------------- State ----------------
LiquidCrystal_I2C *lcd = nullptr;
bool lcdOk = false;
bool fsOk = false;
bool wifiConfigured = false;   // a saved or secrets.h network exists: WiFi + NTP
bool secretsWifiOk = false;    // WIFI_SSID / WIFI_PASSWORD in secrets.h are filled in
bool uploadsEnabled = false;   // also Firebase + device user present
bool sntpStarted = false;
Preferences prefs;
WiFiClientSecure tls;

// ---------------- Wi-Fi setup portal ----------------
WebServer web(80);
DNSServer dns;
String savedSsid, savedPass;          // NVS namespace "wifi"; "" = nothing saved
String curWifiSource, curWifiSsid;    // network last joined ("saved" / "secrets")
bool apPwOk = false;                  // SETUP_AP_PASSWORD is 8-13 characters
bool btnLatched = false, btnRaw = false, btnEvent = false, btnPwError = false;
uint32_t btnRawSinceMs = 0;
bool portalOpen = false;
uint32_t portalOpenMs = 0, portalCloseAtMs = 0, ledToggleMs = 0;
enum SaveState { SAVE_IDLE, SAVE_TESTING, SAVE_OK, SAVE_FAIL };
SaveState saveState = SAVE_IDLE;
String saveSsid, savePass, saveReason;
uint32_t saveStartMs = 0, saveDoneMs = 0;
int saveRssi = 0;
volatile uint8_t lastDiscReason = 0;  // set by the WiFi event task

// "Wi-Fi changed" event, sent with the next history upload (the portal pauses uploads).
struct WifiChangeEvent { bool pending = false; String expId, ssid; uint32_t run = 0, atMillis = 0; } wifiEvt;

SensorAcc s1{PIN_MQ1, RL1, R0_MQ1, NH3_A, NH3_B};
SensorAcc s2{PIN_MQ2, RL2, R0_MQ2, CO2_A, CO2_B};

std::vector<RunInfo> runs;   // oldest first; runs.back() is the current run
uint32_t bootCount = 0, runCounter = 0;
String   resetReasonStr;
String   nvsExp, nvsSample, nvsFood;  // experimentId / sampleId / food name in NVS ("" = calibration)
uint32_t baselineFiles = 0, baselineBytes = 0;  // baseline_logger's /r*.jsonl, left untouched

uint32_t nextSampleMs = 0, nextRecordMs = 0, nextLatestMs = 0, nextHistoryMs = 0;
uint32_t lastSampleMs = 0, gapMaxMs = 0;
uint32_t lastWifiTryMs = 0;
bool wasConnected = false;
bool timeSynced = false;
bool uploading = false, historyRequested = false, forceWifi = false, lastUploadOk = true;
const char *historyReason = "";
String lastHistoryResult = "none yet", lastLatestResult = "none yet";
bool writeDenied = false;
bool settleBeeped = false;
bool settleWarnBeeped = false;
bool haveRecord = false, quietSerial = false;
float lastV1 = 0, lastV2 = 0;
int lastRank1 = -1, lastRank2 = -1;
char lcdLines[2][17] = {"", ""};

bool latestPending = false;
String latestLine;
uint32_t latestRun = 0;

volatile bool sntpSynced = false;  // set by the SNTP callback: a real sync in THIS boot
uint32_t rejectedCount = 0;        // records in /rejected.jsonl (NVS "rejN")
uint32_t outboxOffset = 0, outboxPending = 0;   // first unsent byte of /outbox.jsonl (NVS "obOff")
uint32_t remoteMaxRun = 0;         // highest R<n> under telemetry/FRIDGE001/runs (0 = not read yet)
bool remoteMaxKnown = false, readDenied = false;
uint64_t lastKeyEpoch = 0;         // keeps observation / event keys unique
uint32_t nextCommandMs = 0;
String lcdMsg;                     // FOOD <id> / EXP ENDED on line 1 until lcdMsgUntilMs
uint32_t lcdMsgUntilMs = 0;
String lastResp;                   // start of the last failed Firebase response (for the rejected reason)

String idToken, refreshToken, producerUid;
uint32_t tokenExpiryMs = 0;
bool tokenValid = false, uidPrinted = false;
int lastAuthCode = 0;
uint32_t signInFailMs = 0;

enum InputMode { IN_CMD, IN_FORGET, IN_ATEST };
InputMode inputMode = IN_CMD;
String inBuf;
uint32_t inStartMs = 0;

const char *QUALITY_NAMES[] = {"valid", "settling", "fault"};

// ---- Spoilage alert (1.2.0) ----
const char *ALERT_NAMES[] = {"off", "no_baseline", "settling", "fault", "normal", "warning", "critical"};
const char *BAND_NAMES[]  = {"normal", "warning", "critical"};
const char *CH_NAMES[]    = {"mq1", "mq2"};
const char *NO_BASELINE_MSG = "Baseline not ready: wait until the LCD shows CAL VALID for 15 min before starting";
BaselineRec blBuf[BASELINE_WINDOW_RECORDS];
int blBufN = 0, blBufHead = 0;      // records in the buffer, next slot
bool calReady = false;              // calibration mode: a full clean-air window, food may go in (CAL READY)
Baseline bl;                        // baseline of the running experiment
FoodRef fr;                         // food reference of the running experiment (record only)
const char *FR_NAMES[] = {"none", "pending", "taken", "missed"};
Alert al;
int alertTestLeft = 0;              // `A <pct>`: records still forced
float alertTestPct = 0;
AlertLog alLog[ALERT_LOG_N];
int alLogN = 0, alLogHead = 0;

// =====================================================================
// Helpers
// =====================================================================

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

bool looksPlaceholder(const char *s) {
  if (!s || !*s) return true;
  String v(s); v.toUpperCase();
  return v.indexOf("YOUR") >= 0 || v.indexOf("PLACEHOLDER") >= 0 ||
         v.indexOf("CHANGE") >= 0 || v.indexOf("XXXX") >= 0 || v.indexOf('<') >= 0;
}

double rnd(double v, int decimals) {
  double p = pow(10.0, decimals);
  return round(v * p) / p;
}

RunInfo &curRun() { return runs.back(); }

uint32_t totalUnsent() {
  uint32_t n = 0;
  for (auto &r : runs) n += r.unsent;
  return n;
}

String runIdOf(uint32_t run) {
  char b[12]; snprintf(b, sizeof(b), "R%05lu", (unsigned long)run);
  return b;
}

String rid(const RunInfo &r) { return runIdOf(r.idNum); }

// <runId>-<uptime_s>, uptime padded to 7 digits so keys sort in time order for 115 days.
String measurementId(const RunInfo &r, uint32_t uptime) {
  char b[24]; snprintf(b, sizeof(b), "R%05lu-%07lu", (unsigned long)r.idNum, (unsigned long)uptime);
  return b;
}

String segPath(uint32_t run, uint32_t seg) { return "/f" + String(run) + "_" + String(seg) + ".jsonl"; }
String metaPath(uint32_t run) { return "/f" + String(run) + ".m"; }
String nvsKey(char prefix, uint32_t run) { return String(prefix) + String(run); }
String samplesPath(const RunInfo &r) { return String(RUNS_PATH) + "/" + rid(r) + "/samples"; }
String runMetaPath(const RunInfo &r) { return "experiments/" + r.expId + "/runs/" + rid(r); }

// Board time in Unix ms, or 0 while this boot has no real time sync.
uint64_t nowMs() {
  if (!timeSynced) return 0;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// "YYYY-MM-DDTHH:MM" (or with a space) in WAT -> Unix ms. 0 = blank or not a valid time.
uint64_t parseWat(String s) {
  s.trim();
  int y, mo, d, h, mi;
  if (s.length() < 16 || sscanf(s.c_str(), "%4d-%2d-%2d%*c%2d:%2d", &y, &mo, &d, &h, &mi) != 5) return 0;
  if (y < 2023 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) return 0;
  int64_t sec = daysFromCivil(y, mo, d) * 86400LL + h * 3600 + mi * 60 - WAT_OFFSET_S;
  return sec > 0 ? (uint64_t)sec * 1000ULL : 0;
}

// Unix ms -> "YYYY-MM-DD HH:MM" in WAT.
String fmtWat(uint64_t ms) {
  time_t t = (time_t)(ms / 1000) + WAT_OFFSET_S;
  struct tm tmW; gmtime_r(&t, &tmW);
  char b[24]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmW);
  return b;
}

// YYYYMMDD in WAT of the board time (0 while not synced).
uint32_t watDay() {
  uint64_t ms = nowMs();
  if (!ms) return 0;
  time_t t = (time_t)(ms / 1000) + WAT_OFFSET_S;
  struct tm tmW; gmtime_r(&t, &tmW);
  return (tmW.tm_year + 1900) * 10000 + (tmW.tm_mon + 1) * 100 + tmW.tm_mday;
}

// Unique seconds for observation / event keys, even for two actions in the same second.
uint64_t keyEpoch() {
  uint64_t s = nowMs() / 1000;
  if (s <= lastKeyEpoch) s = lastKeyEpoch + 1;
  lastKeyEpoch = s;
  return s;
}

// Upper-cases an ID; "" if it breaks the rules: A-Z 0-9 -, ID_MIN_LEN..ID_MAX_LEN characters.
// Firebase keys cannot contain . $ # [ ] /, so nothing else is let through.
String normId(String s) {
  s.trim();
  s.toUpperCase();
  if ((int)s.length() < ID_MIN_LEN || (int)s.length() > ID_MAX_LEN) return "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-') return "";
  }
  return s;
}

// Free text from the page or a command: trimmed, control characters made spaces.
// false if it has more than maxChars characters (UTF-8 counted per character).
bool cleanText(String &s, int maxChars) {
  s.trim();
  int chars = 0;
  for (size_t i = 0; i < s.length(); i++) {
    if ((uint8_t)s[i] < 0x20) s.setCharAt(i, ' ');
    if (((uint8_t)s[i] & 0xC0) != 0x80) chars++;
  }
  return chars <= maxChars;
}

String dbBaseUrl() {
  String u(FIREBASE_DATABASE_URL);
  while (u.endsWith("/")) u.remove(u.length() - 1);
  return u;
}

// Tag of the configured device email, so a refresh token of another user is not reused.
String emailTag() {
  uint32_t h = 2166136261u;
  for (const char *p = DEVICE_EMAIL; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  char b[9]; snprintf(b, sizeof(b), "%08lx", (unsigned long)h);
  return b;
}

void beep(int count) {
  for (int i = 0; i < count; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(120);
    digitalWrite(PIN_BUZZER, LOW);  delay(180);
  }
}

int qualityRank(const char *q) {
  if (!q) return -1;
  for (int i = 0; i < 3; i++) if (!strcmp(q, QUALITY_NAMES[i])) return i;
  return -1;
}

// =====================================================================
// LCD
// =====================================================================

void lcdInit() {
  // Bind pins BEFORE any library touches Wire (see hw_selftest): the
  // argument-less Wire.begin() inside lcd.init() is then a no-op.
  Wire.setPins(PIN_SDA, PIN_SCL);
  Wire.begin(PIN_SDA, PIN_SCL);
  uint8_t addr = 0;
  for (uint8_t a : {0x27, 0x3F}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { addr = a; break; }
  }
  if (!addr) { Serial.println(F("  LCD: not found at 0x27/0x3F - continuing without it")); return; }
  lcd = new LiquidCrystal_I2C(addr, 16, 2);
  lcd->init();
  lcd->backlight();
  lcdOk = true;
}

void lcdPut(int row, const char *text) {
  if (!lcdOk) return;
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  if (!strcmp(buf, lcdLines[row])) return;
  strcpy(lcdLines[row], buf);
  lcd->setCursor(0, row); lcd->print(buf);
}

// FOOD <experimentId> after a start, EXP ENDED after an end: line 1 for LCD_MSG_MS.
bool lcdMsgActive() { return lcdMsg.length() && (int32_t)(lcdMsgUntilMs - millis()) > 0; }

void showLcdMsg(const String &text) {
  lcdMsg = text.substring(0, 16);
  lcdMsgUntilMs = millis() + LCD_MSG_MS;
}

uint32_t portalLeftS() {
  int32_t ms = (int32_t)(portalCloseAtMs - millis());
  return ms > 0 ? ((uint32_t)ms + 999) / 1000 : 0;
}

// While the portal is open: join details (A) and status (B) alternate every 4 s,
// starting with A. Save results replace both. The AP password is shown only here.
void portalLcd() {
  uint32_t now = millis();
  char l1[24], l2[24];
  if (saveState == SAVE_TESTING) {
    snprintf(l1, sizeof(l1), "WIFI TESTING");
    snprintf(l2, sizeof(l2), "%s", saveSsid.c_str());
  } else if (saveState == SAVE_OK) {
    snprintf(l1, sizeof(l1), "WIFI OK");
    snprintf(l2, sizeof(l2), "%s", saveSsid.c_str());
  } else if (saveState == SAVE_FAIL && now - saveDoneMs < LCD_FAIL_MS) {
    snprintf(l1, sizeof(l1), "WIFI FAIL");
    snprintf(l2, sizeof(l2), "%s", saveReason.c_str());
  } else if (((now - portalOpenMs) / LCD_SCREEN_MS) % 2 == 0) {
    snprintf(l1, sizeof(l1), "%s", SETUP_AP_SSID);
    snprintf(l2, sizeof(l2), "PW:%s", SETUP_AP_PASSWORD);
  } else {
    uint32_t left = portalLeftS();
    snprintf(l1, sizeof(l1), "WIFI SETUP %lu:%02lu%s", (unsigned long)(left / 60), (unsigned long)(left % 60),
             WiFi.softAPgetStationNum() > 0 ? "*" : "");
    snprintf(l2, sizeof(l2), "%s", WiFi.softAPIP().toString().c_str());
  }
  if (lcdMsgActive()) snprintf(l1, sizeof(l1), "%s", lcdMsg.c_str());
  l1[16] = l2[16] = 0;
  lcdPut(0, l1);
  lcdPut(1, l2);
}

// Food-test line 1 (1.2.0): FRESH 12% / WARNING 55% / SPOILT 92% / NO BASELINE /
// FOOD SETTL 0h22m (or HELD SPOILT / HELD WARNING after a reboot) / FAULT MQ1.
// Forced records (`A`) end in TEST.
void alertLcdLine(char *l1, size_t n, uint32_t up) {
  const char *test = al.test ? " TEST" : "";
  switch (al.state) {
    case AL_NO_BASELINE: snprintf(l1, n, "NO BASELINE"); break;
    case AL_SETTLING:
      if (al.band == 2)      snprintf(l1, n, "HELD SPOILT");
      else if (al.band == 1) snprintf(l1, n, "HELD WARNING");
      else {
        uint32_t left = up < SETTLE_S ? (SETTLE_S - up + 59) / 60 : 0;
        snprintf(l1, n, "FOOD SETTL %luh%02lum", (unsigned long)(left / 60), (unsigned long)(left % 60));
      }
      break;
    case AL_FAULT: {
      bool f1 = al.ch[0].reason && !strcmp(al.ch[0].reason, "fault");
      bool f2 = al.ch[1].reason && !strcmp(al.ch[1].reason, "fault");
      snprintf(l1, n, "FAULT %s", f1 && f2 ? "MQ1+MQ2" : f1 ? "MQ1" : "MQ2");
      break;
    }
    case AL_WARNING:  snprintf(l1, n, "WARNING %.0f%%%s", al.score, test); break;
    case AL_CRITICAL: snprintf(l1, n, "SPOILT %.0f%%%s", al.score, test); break;
    default:
      if (al.scored) snprintf(l1, n, "FRESH %.0f%%%s", al.score, test);
      else           snprintf(l1, n, "FRESH");
  }
}

// Line 1: mode + quality ("FOOD SETTL 1h12m" = settling, 1 h 12 min left); in food-test
// mode the spoilage verdict (alertLcdLine).
// Line 2: both voltages + unsent count ("0.42V 0.71V U3"), or NOUP when uploads are disabled.
// Wi-Fi button latched with the portal closed: line 1 asks for it to be released.
void updateLcd() {
  if (!lcdOk) return;
  if (portalOpen) { portalLcd(); return; }
  const char *mode = curRun().foodTest ? "FOOD" : "CAL";
  uint32_t up = millis() / 1000;
  char l1[24], l2[24];
  if (curRun().foodTest) {
    alertLcdLine(l1, sizeof(l1), up);       // 1.2.0: the verdict replaces FOOD VALID
  } else if (lastRank1 == 2 || lastRank2 == 2) {
    snprintf(l1, sizeof(l1), "FAULT %s", lastRank1 == 2 && lastRank2 == 2 ? "MQ1+MQ2" : lastRank1 == 2 ? "MQ1" : "MQ2");
  } else if (up < SETTLE_S) {
    uint32_t left = (SETTLE_S - up + 59) / 60;
    snprintf(l1, sizeof(l1), "%s SETTL %luh%02lum", mode, (unsigned long)(left / 60), (unsigned long)(left % 60));
  } else {
    snprintf(l1, sizeof(l1), "%s", calReady ? "CAL READY" : "CAL VALID");   // food-test mode never gets here
  }
  if (!haveRecord)          snprintf(l2, sizeof(l2), "1st rec at 1m");
  else if (!uploadsEnabled) snprintf(l2, sizeof(l2), "%.2fV %.2fV NOUP", lastV1, lastV2);
  else {
    unsigned u = totalUnsent();
    snprintf(l2, sizeof(l2), "%.2fV %.2fV U%u", lastV1, lastV2, u);
    if (strlen(l2) > 16) snprintf(l2, sizeof(l2), "%.2fV%.2fV U%u", lastV1, lastV2, u);
  }
  if (btnLatched) snprintf(l1, sizeof(l1), "%s", btnPwError ? "AP PW 8-13 CHARS" : "RELEASE WIFI BTN");
  if (lcdMsgActive()) snprintf(l1, sizeof(l1), "%s", lcdMsg.c_str());
  l1[16] = l2[16] = 0;
  lcdPut(0, l1);
  lcdPut(1, l2);
}

// =====================================================================
// Run bookkeeping (LittleFS + NVS)
// =====================================================================

void writeMetaFile(const RunInfo &r) {
  if (!fsOk) return;
  JsonDocument d;
  d["run"]          = r.run;
  d["id_num"]       = r.idNum;
  d["id_ok"]        = r.idOk;
  d["boot"]         = r.boot;
  d["reset"]        = r.reset;
  d["start_reason"] = r.startReason;
  d["start_uptime"] = r.startUptime;
  if (r.endReason.length()) { d["end_reason"] = r.endReason; d["end_uptime"] = r.endUptime; }
  d["food_test"]    = r.foodTest;
  d["exp"]          = r.expId;
  d["sample"]       = r.sampleId;
  d["boot_epoch_ms"] = r.bootEpochMs;
  d["synced_at_ms"]  = r.syncedAtMs;
  if (r.wifiSource.length()) { d["wifi_source"] = r.wifiSource; d["wifi_ssid"] = r.wifiSsid; }
  if (r.alertBaseline.length()) d["al_bl"] = r.alertBaseline;
  if (r.foodRef.length()) d["al_fr"] = r.foodRef;
  File f = LittleFS.open(metaPath(r.run), FILE_WRITE);
  if (!f) { Serial.println(F("  FS: cannot write meta file")); return; }
  serializeJson(d, f);
  f.close();
}

bool readMetaFile(RunInfo &r) {
  File f = LittleFS.open(metaPath(r.run), FILE_READ);
  if (!f) return false;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return false;
  r.idNum       = d["id_num"] | r.run;
  r.idOk        = d["id_ok"] | false;   // 1.0.0 files: decided from metaState in loadOldRuns()
  r.boot        = d["boot"] | 0;
  r.reset       = (const char *)(d["reset"] | "UNKNOWN");
  r.startReason = (const char *)(d["start_reason"] | "boot");
  r.startUptime = d["start_uptime"] | 0;
  r.endReason   = (const char *)(d["end_reason"] | "");
  r.endUptime   = d["end_uptime"] | 0;
  r.foodTest    = d["food_test"] | false;
  r.expId       = (const char *)(d["exp"] | "");
  r.sampleId    = (const char *)(d["sample"] | CAL_SAMPLE_ID);
  r.bootEpochMs = d["boot_epoch_ms"].as<unsigned long long>();
  r.syncedAtMs  = d["synced_at_ms"].as<unsigned long long>();
  r.wifiSource  = (const char *)(d["wifi_source"] | "");
  r.wifiSsid    = (const char *)(d["wifi_ssid"] | "");
  r.alertBaseline = (const char *)(d["al_bl"] | "");
  r.foodRef       = (const char *)(d["al_fr"] | "");
  return r.expId.length() > 0;
}

// Count logged / unsent records of an earlier run and find where unsent data starts.
void scanRun(RunInfo &r) {
  JsonDocument filter;
  filter["uptime_s"] = true;
  for (uint32_t seg : r.segs) {
    File f = LittleFS.open(segPath(r.run, seg), FILE_READ);
    if (!f) continue;
    while (f.available()) {
      size_t pos = f.position();
      String line = f.readStringUntil('\n');
      if (line.length() < 2) continue;
      JsonDocument d;
      if (deserializeJson(d, line, DeserializationOption::Filter(filter))) continue;  // torn line
      r.logged++;
      if ((uint32_t)(d["uptime_s"] | 0) > r.sentUptime) {
        if (!r.unsent) { r.upSeg = seg; r.upOffset = pos; r.upPosValid = true; }
        r.unsent++;
      }
    }
    f.close();
  }
}

void runMetaJson(const RunInfo &r, JsonObject d);

// Appends one JSON line to /rejected.jsonl. Rejected records are never deleted; `d` and `r` dump them.
bool appendRejected(JsonDocument &d) {
  File out = LittleFS.open(REJECTED_PATH, FILE_APPEND);
  if (!out) return false;
  serializeJson(d, out);
  bool ok = out.print('\n') == 1;
  out.close();
  return ok;
}

void addRejected(uint32_t n) {
  rejectedCount += n;
  prefs.putUInt("rejN", rejectedCount);
}

// Part 3 fix. A run whose boot never had a real time sync can never get a real epoch:
// bootEpochMs only ever comes from a sync in the same boot. 1.0.0 left such runs in
// `unsent` for good (R00002 and R00006: 105 records, the floor of the stuck count).
// Their run metadata and every record are copied to /rejected.jsonl with the reason,
// and only then are the original files removed. Nothing is uploaded with a guessed time.
void rejectUntimedRun(RunInfo &r) {
  String reason = "time never synced in boot " + String(r.boot) + "; no real epoch can be given";
  uint32_t n = 0;
  bool ok = true;
  {
    JsonDocument d;
    d["reason"] = reason;
    d["runId"]  = rid(r);
    runMetaJson(r, d["meta"].to<JsonObject>());
    ok = appendRejected(d);
  }
  for (uint32_t seg : r.segs) {
    File f = LittleFS.open(segPath(r.run, seg), FILE_READ);
    if (!f) continue;
    while (ok && f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 2) continue;
      JsonDocument d;
      d["reason"] = reason;
      d["runId"]  = rid(r);
      JsonDocument rec;
      if (deserializeJson(rec, line)) d["raw"] = line;   // torn line from a power cut
      else d["rec"] = rec;
      ok = appendRejected(d);
      if (ok) n++;
    }
    f.close();
  }
  if (!ok) {   // flash full: keep the run as it is, try again at the next boot
    Serial.printf("  !! %s: could not write %s - run left in place\n", rid(r).c_str(), REJECTED_PATH);
    return;
  }
  for (uint32_t seg : r.segs) LittleFS.remove(segPath(r.run, seg));
  LittleFS.remove(metaPath(r.run));
  prefs.remove(nvsKey('s', r.run).c_str());
  prefs.remove(nvsKey('m', r.run).c_str());
  prefs.remove(nvsKey('v', r.run).c_str());
  addRejected(n);
  r.segs.clear();
  r.unsent = 0;
  if (n) Serial.printf("  REJECTED %s: %lu records moved to %s (%s)\n", rid(r).c_str(), (unsigned long)n,
                       REJECTED_PATH, reason.c_str());
}

// Loads earlier runs from flash; returns the highest run number found.
uint32_t loadOldRuns() {
  std::map<uint32_t, std::vector<uint32_t>> found;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String n = f.name();            // core 3.x returns the bare name, no leading '/'
    if (n.startsWith("/")) n.remove(0, 1);
    unsigned long run = 0, seg = 0;
    if (n.startsWith("f") && n.endsWith(".jsonl") && sscanf(n.c_str(), "f%lu_%lu", &run, &seg) == 2 && run) {
      found[run].push_back(seg);
    } else if (n.startsWith("f") && n.endsWith(".m") && sscanf(n.c_str(), "f%lu", &run) == 1 && run) {
      found[run];
    } else if (n.startsWith("r") && n.endsWith(".jsonl")) {
      baselineFiles++; baselineBytes += f.size();   // baseline_logger data: never touched here
    }
  }
  uint32_t maxRun = 0;
  for (auto &kv : found) {
    RunInfo r;
    r.run = kv.first;
    r.segs = kv.second;
    std::sort(r.segs.begin(), r.segs.end());
    r.idNum = r.run;
    if (!readMetaFile(r)) { r.expId = "CAL-" + runIdOf(r.run); r.sampleId = CAL_SAMPLE_ID; }
    r.sentUptime = prefs.getUInt(nvsKey('s', r.run).c_str(), 0);
    r.metaState  = prefs.getUChar(nvsKey('m', r.run).c_str(), 0);
    r.eventSent  = prefs.getBool(nvsKey('v', r.run).c_str(), false);
    if (r.metaState > 0) r.idOk = true;     // already written to Firebase under this runId
    maxRun = std::max(maxRun, std::max(r.run, r.idNum));
    scanRun(r);
    if (!r.bootEpochMs) {                   // every loaded run is from an earlier boot
      rejectUntimedRun(r);
      if (!r.segs.empty() || LittleFS.exists(metaPath(r.run))) runs.push_back(r);   // move failed
      continue;
    }
    Serial.printf("  Earlier run %s (%s): %lu records, %lu unsent\n",
                  rid(r).c_str(), r.expId.c_str(), (unsigned long)r.logged, (unsigned long)r.unsent);
    runs.push_back(r);
  }
  if (baselineFiles)
    Serial.printf("  %lu baseline_logger file(s) on flash (%lu bytes): left untouched\n",
                  (unsigned long)baselineFiles, (unsigned long)baselineBytes);
  return maxRun;
}

// Starts a new run with the experiment stored in NVS, using the same counter as at boot.
// reason: "boot", "experiment_start" or "experiment_end". Without a reboot the previous
// run is closed (runEndReason) and the uptime, so the settling clock, carries on.
void startRun(const char *reason) {
  bool boot = !strcmp(reason, "boot");
  if (!boot && !runs.empty()) {
    RunInfo &prev = curRun();
    prev.endReason = reason;
    prev.endUptime = millis() / 1000;
    prev.metaDirty = true;
    writeMetaFile(prev);
  }
  RunInfo r;
  r.run = ++runCounter;
  r.idNum = r.run;
  prefs.putUInt("run", runCounter);
  r.boot = bootCount;
  r.reset = resetReasonStr;
  r.startReason = reason;
  r.startUptime = boot ? 0 : millis() / 1000;
  r.foodTest = nvsExp.length() > 0;
  r.expId    = r.foodTest ? nvsExp : "CAL-" + rid(r);
  r.sampleId = r.foodTest ? nvsSample : String(CAL_SAMPLE_ID);
  if (!runs.empty() && timeSynced) {        // same boot: the clock is already known
    r.bootEpochMs = curRun().bootEpochMs;
    r.syncedAtMs  = curRun().syncedAtMs;
  }
  if (WiFi.status() == WL_CONNECTED && curWifiSource.length()) { r.wifiSource = curWifiSource; r.wifiSsid = curWifiSsid; }
  if (r.foodTest) r.alertBaseline = baselineJson(bl);   // alertStart() / alertBoot() ran before
  if (r.foodTest && fr.state == FR_TAKEN) r.foodRef = baselineJson(fr.b);
  runs.push_back(r);
  writeMetaFile(r);
  Serial.printf("  Run %s started (%s): mode %s, experimentId %s, sampleId %s\n", rid(r).c_str(), reason,
                r.foodTest ? "food_test" : "calibration", r.expId.c_str(), r.sampleId.c_str());
}

// Deletes uploaded, closed segment files (oldest first) while LittleFS is over the limit.
// Never touches runs whose clock was never set, unsent records, or baseline_logger files.
void pruneFs() {
  // Not while an upload or dump is walking through runs: erasing would shift them.
  if (!fsOk || uploading || quietSerial) return;
  uint32_t curSeg = (millis() / 1000) / SEGMENT_S;
  while (LittleFS.usedBytes() > FS_PRUNE_FRACTION * LittleFS.totalBytes()) {
    bool done = false;
    for (size_t i = 0; i < runs.size() && !done; i++) {
      RunInfo &r = runs[i];
      bool isCur = i + 1 == runs.size();
      if (!r.bootEpochMs) continue;
      for (size_t k = 0; k < r.segs.size() && !done; k++) {
        uint32_t seg = r.segs[k];
        if (isCur && seg >= curSeg) continue;              // still being written
        bool uploaded = r.unsent == 0 || r.sentUptime >= (seg + 1) * SEGMENT_S;
        if (!uploaded) continue;
        Serial.printf("  FS: deleting uploaded segment %s\n", segPath(r.run, seg).c_str());
        LittleFS.remove(segPath(r.run, seg));
        r.segs.erase(r.segs.begin() + k);
        done = true;
      }
      if (done && !isCur && r.segs.empty() && r.metaState == 2 &&
          (r.eventSent || !r.foodTest || r.startReason != "boot")) {
        LittleFS.remove(metaPath(r.run));
        prefs.remove(nvsKey('s', r.run).c_str());
        prefs.remove(nvsKey('m', r.run).c_str());
        prefs.remove(nvsKey('v', r.run).c_str());
        runs.erase(runs.begin() + i);
      }
    }
    if (!done) {
      static bool warned = false;
      if (!warned) Serial.println(F("  FS: over limit but no uploaded segment left to delete"));
      warned = true;
      return;
    }
  }
}

// =====================================================================
// Sampling and records
// =====================================================================

void sampleSensor(SensorAcc &s) {
  int raw = analogRead(s.pin);
  float v = raw * ADC_REF / ADC_MAX;
  s.raw.add(raw);
  s.volt.add(v);
  if (raw <= 0 || raw >= 4095) s.railCount++;
  if (raw >= 4095) s.railHigh++;
  if (v <= 0.0f) return;  // Rs undefined
  float rs = s.rl * (VCC - v) / v;
  float ratio = rs / s.r0;
  s.rs.add(rs); s.ratio.add(ratio); s.ppm.add(s.a * powf(ratio, s.b));
}

void putStats(JsonObject o, const char *name, const Stat &st, int decimals) {
  if (!st.n) { o[name] = nullptr; return; }
  JsonObject x = o[name].to<JsonObject>();
  x["mean"] = rnd(st.mean(), decimals);
  x["min"]  = rnd(st.mn, decimals);
  x["max"]  = rnd(st.mx, decimals);
  x["sd"]   = rnd(st.sd(), decimals + 1);
}

// Writes the channel statistics, quality and faults; returns 0 valid, 1 settling, 2 fault.
int putSensor(JsonObject o, const SensorAcc &s, uint32_t uptimeS) {
  o["n"]    = s.raw.n;
  if (s.ratio.n) { o["ppm"] = rnd(s.ppm.mean(), 3); o["rs_r0"] = rnd(s.ratio.mean(), 5); }
  o["rail"] = s.railCount;
  putStats(o, "raw", s.raw, 1);
  putStats(o, "rs", s.rs, 1);
  putStats(o, "volt", s.volt, 4);

  double vMean = s.volt.mean();
  bool fault = false;
  JsonArray faults = o["faults"].to<JsonArray>();
  if (vMean < NO_SIGNAL_V)      { faults.add("no_signal");      fault = true; }
  if (s.railHigh > 0)           { faults.add("saturation");     fault = true; }
  if (vMean > NEAR_ADC_LIMIT_V) { faults.add("near_adc_limit"); fault = true; }
  if (s.volt.sd() > NOISE_V_SD)   faults.add("high_noise");     // flag only, state unchanged
  if (!faults.size()) o.remove("faults");
  int rank = fault ? 2 : (uptimeS < SETTLE_S ? 1 : 0);
  o["quality"] = QUALITY_NAMES[rank];
  return rank;
}

void blinkLed() {
  digitalWrite(PIN_LED, HIGH); delay(40); digitalWrite(PIN_LED, LOW);
  if (!lastUploadOk) { delay(120); digitalWrite(PIN_LED, HIGH); delay(40); digitalWrite(PIN_LED, LOW); }
}

// =====================================================================
// On-board spoilage alert (1.2.0)
// =====================================================================
// The board scores each food experiment against that experiment's own clean-air baseline.
// It is an extra verdict (deviceAlert): the raw data and the app's assessment paths are
// untouched. NVS: blExp blR1 blR2 blN1 blN2 blW blFrom blTo (baseline), alBand alSil alN.

// Rolling buffer of the last BASELINE_WINDOW_RECORDS records of this boot (RAM only).
void feedBaseline(uint32_t up, int q1, int q2) {
  BaselineRec &b = blBuf[blBufHead];
  b.up = up;
  b.rs[0] = s1.rs.n ? s1.rs.mean() : 0;
  b.rs[1] = s2.rs.n ? s2.rs.mean() : 0;
  b.q[0] = q1; b.q[1] = q2;
  blBufHead = (blBufHead + 1) % BASELINE_WINDOW_RECORDS;
  if (blBufN < BASELINE_WINDOW_RECORDS) blBufN++;
}

// Per channel: records in the buffer that are valid, of this boot and no older than
// BASELINE_MAX_AGE_S. Fills out (median Rs) when told to.
void baselineFromBuffer(Baseline &out) {
  out = Baseline();
  uint32_t up = millis() / 1000;
  std::vector<float> v[2];
  uint32_t upMin = UINT32_MAX, upMax = 0;
  for (int i = 0; i < blBufN; i++) {
    const BaselineRec &b = blBuf[i];
    if (up - b.up > BASELINE_MAX_AGE_S) continue;
    out.window++;
    for (int c = 0; c < 2; c++) {
      if (b.q[c] != 0 || b.rs[c] <= 0) continue;
      v[c].push_back(b.rs[c]);
      upMin = std::min(upMin, b.up); upMax = std::max(upMax, b.up);
    }
  }
  for (int c = 0; c < 2; c++) {
    out.n[c] = v[c].size();
    if (!out.ok(c)) continue;
    std::sort(v[c].begin(), v[c].end());
    size_t m = v[c].size() / 2;
    out.rs[c] = v[c].size() % 2 ? v[c][m] : (v[c][m - 1] + v[c][m]) / 2;
  }
  out.any = out.ok(0) || out.ok(1);
  uint64_t boot = curRun().bootEpochMs;
  if (upMax && boot) { out.fromMs = boot + (uint64_t)upMin * 1000ULL; out.toMs = boot + (uint64_t)upMax * 1000ULL; }
}

// Calibration mode, once per record: CAL READY when every channel with valid records has a
// full window (BASELINE_WINDOW_RECORDS) and at least one channel has. One long beep on
// becoming ready; it re-arms when the window stops being full.
bool baselineReady(const Baseline &b) {
  if (!b.any) return false;
  for (int c = 0; c < 2; c++) if (b.n[c] && b.n[c] < BASELINE_WINDOW_RECORDS) return false;
  return true;
}

void serviceCalReady() {
  Baseline b;
  baselineFromBuffer(b);
  bool ready = baselineReady(b);
  if (ready && !calReady) {
    Serial.printf("*** CAL READY: %d min of clean air - put the food in now, then press Start ***\n", BASELINE_WINDOW_RECORDS);
    digitalWrite(PIN_BUZZER, HIGH); delay(600); digitalWrite(PIN_BUZZER, LOW);   // one long beep
  } else if (!ready && calReady) {
    Serial.println(F("  CAL READY lost (a record was not valid): wait for CAL READY again before the food goes in"));
  }
  calReady = ready;
}

void saveAlertNvs() {
  prefs.putUChar("alBand", al.band);
  prefs.putBool("alSil", al.silenced);
}

// NVS keys <p>R1 <p>R2 <p>N1 <p>N2 <p>W <p>From <p>To; p = "bl" (clean air) or "fr" (food reference).
void putBaselineNvs(const char *p, const Baseline &b) {
  String k(p);
  prefs.putFloat((k + "R1").c_str(), b.rs[0]); prefs.putFloat((k + "R2").c_str(), b.rs[1]);
  prefs.putUChar((k + "N1").c_str(), b.n[0]);  prefs.putUChar((k + "N2").c_str(), b.n[1]);
  prefs.putUChar((k + "W").c_str(), b.window);
  prefs.putULong64((k + "From").c_str(), b.fromMs); prefs.putULong64((k + "To").c_str(), b.toMs);
}

void getBaselineNvs(const char *p, Baseline &b) {
  String k(p);
  b.rs[0] = prefs.getFloat((k + "R1").c_str(), 0); b.rs[1] = prefs.getFloat((k + "R2").c_str(), 0);
  b.n[0] = prefs.getUChar((k + "N1").c_str(), 0);  b.n[1] = prefs.getUChar((k + "N2").c_str(), 0);
  b.window = prefs.getUChar((k + "W").c_str(), 0);
  b.fromMs = prefs.getULong64((k + "From").c_str(), 0); b.toMs = prefs.getULong64((k + "To").c_str(), 0);
  b.any = b.ok(0) || b.ok(1);
}

// Experiment start (setup page or command): baseline from the buffer, alert reset.
void alertStart(const String &exp) {
  baselineFromBuffer(bl);
  calReady = false;
  prefs.putString("blExp", exp);
  putBaselineNvs("bl", bl);
  prefs.putUInt("alN", 0);
  fr = FoodRef();
  fr.state = FR_PENDING;
  fr.dueMs = nowMs() + (uint64_t)FOOD_REF_DELAY_S * 1000ULL;   // a start always has the time
  prefs.putUChar("frSt", fr.state);
  prefs.putULong64("frDue", fr.dueMs);
  al = Alert();
  al.state = bl.any ? AL_NORMAL : AL_NO_BASELINE;
  al.sinceUp = millis() / 1000;
  alertTestLeft = 0;
  saveAlertNvs();
  if (bl.any)
    Serial.printf("  BASELINE taken: mq1 %s, mq2 %s (%u records in the window)\n",
                  bl.ok(0) ? (String(bl.rs[0], 0) + " ohm from " + bl.n[0] + " records").c_str() : (String("not usable, ") + bl.n[0] + " valid").c_str(),
                  bl.ok(1) ? (String(bl.rs[1], 0) + " ohm from " + bl.n[1] + " records").c_str() : (String("not usable, ") + bl.n[1] + " valid").c_str(),
                  bl.window);
  else
    Serial.printf("  BASELINE NOT READY (mq1 %u, mq2 %u valid records, %u needed): alert state no_baseline for this experiment\n",
                  bl.n[0], bl.n[1], BASELINE_MIN_RECORDS);
}

// Experiment end: baseline and alert cleared.
void alertEnd() {
  for (const char *k : {"blExp", "blR1", "blR2", "blN1", "blN2", "blW", "blFrom", "blTo", "alBand", "alSil", "alN",
                        "frSt", "frDue", "frR1", "frR2", "frN1", "frN2", "frW", "frFrom", "frTo"})
    prefs.remove(k);
  bl = Baseline();
  fr = FoodRef();
  al = Alert();
  blBufN = 0; blBufHead = 0;          // those records had food in: the next baseline needs fresh clean air
  calReady = false;
  alertTestLeft = 0;
  digitalWrite(PIN_LED, LOW);
}

// Boot: an experiment in NVS keeps its baseline and last band; settling until SETTLE_S.
void alertBoot() {
  if (!nvsExp.length()) return;
  if (prefs.getString("blExp", "") == nvsExp) {
    getBaselineNvs("bl", bl);
    fr.state = std::min<int>(prefs.getUChar("frSt", FR_NONE), FR_MISSED);
    fr.dueMs = prefs.getULong64("frDue", 0);
    if (fr.state == FR_TAKEN) getBaselineNvs("fr", fr.b);
  }
  al.band = std::min<int>(prefs.getUChar("alBand", 0), 2);
  al.silenced = prefs.getBool("alSil", false);
  al.state = bl.any ? AL_SETTLING : AL_NO_BASELINE;   // started before 1.2.0: no baseline either
  Serial.printf("  Spoilage alert: %s, last band %s%s. Food reference: %s\n", bl.any ? "baseline kept, settling" : "no baseline",
                BAND_NAMES[al.band], al.silenced ? " (silenced)" : "", FR_NAMES[fr.state]);
}

// Food reference: a second, record-only reference from the records FOOD_REF_DELAY_S - 15 min
// to FOOD_REF_DELAY_S after the start (food in and cooled). Same rules as the baseline.
// Tried at every record once due; given up FOOD_REF_LATE_S after due (e.g. a reboot left
// too few valid records). It never changes the alert verdict.
void serviceFoodRef() {
  if (fr.state != FR_PENDING || !timeSynced) return;
  uint64_t now = nowMs();
  if (now < fr.dueMs) return;
  if (now > fr.dueMs + (uint64_t)FOOD_REF_LATE_S * 1000ULL) {
    fr.state = FR_MISSED;
    prefs.putUChar("frSt", fr.state);
    Serial.printf("  FOOD REFERENCE missed: too few valid records within %lu min of its due time\n",
                  (unsigned long)(FOOD_REF_LATE_S / 60));
    return;
  }
  Baseline b;
  baselineFromBuffer(b);
  if (!b.any) return;                      // e.g. settling after a reboot: try at the next record
  fr.b = b;
  fr.state = FR_TAKEN;
  putBaselineNvs("fr", fr.b);
  prefs.putUChar("frSt", fr.state);
  RunInfo &r = curRun();
  r.foodRef = baselineJson(fr.b);
  r.metaDirty = true;
  writeMetaFile(r);
  Serial.printf("  FOOD REFERENCE taken (record only, the alert still uses the clean-air baseline): %s\n", foodRefText().c_str());
}

String foodRefText() {
  if (fr.state == FR_PENDING) return "pending, due " + (fr.dueMs ? fmtWat(fr.dueMs) + " WAT" : String("?"));
  if (fr.state != FR_TAKEN) return FR_NAMES[fr.state];
  String t;
  for (int c = 0; c < 2; c++) {
    if (c) t += ", ";
    t += String(CH_NAMES[c]) + " ";
    t += fr.b.ok(c) ? String(fr.b.rs[c], 0) + " ohm (" + fr.b.n[c] + " rec)" : String("not usable (") + fr.b.n[c] + " valid rec)";
  }
  if (fr.b.fromMs) t += ", " + fmtWat(fr.b.fromMs) + " to " + fmtWat(fr.b.toMs).substring(11) + " WAT";
  return t;
}

float pctOf(float ratio) {
  if (ratio <= 0) return 100;
  float p = 100.0f * log10f(ratio) / log10f(FULL_SCALE_RATIO);
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

float bandThreshold(int b) { return b == 2 ? CRIT_PCT : WARN_PCT; }

void setAlertState(AlertState st, uint32_t up) {
  if (st == al.state) return;
  al.state = st;
  al.sinceUp = up;
}

String alertTimeText(uint64_t atMs, uint32_t up) {
  return atMs ? fmtWat(atMs) + " WAT" : "uptime " + String(up / 60) + " min";
}

// experiments/<experimentId>/events/<runId>-alert-<n>, through the outbox. Before this
// boot's time sync, observedAtMs is filled in at upload from the run's boot time ("up").
void queueAlertEvent(int from, int to, uint64_t atMs, uint32_t up) {
  uint32_t n = prefs.getUInt("alN", 0) + 1;
  prefs.putUInt("alN", n);
  JsonDocument e;
  e["m"] = "PUT";
  e["p"] = "experiments/" + nvsExp + "/events/{RUN}-alert-" + String(n);
  e["r"] = curRun().run;
  if (!atMs) e["up"] = up;
  JsonObject b = e["b"].to<JsonObject>();
  b["experimentId"] = nvsExp;
  b["runId"]        = "";
  b["sampleId"]     = nvsSample;
  b["type"]         = "device_alert";
  if (atMs) b["observedAtMs"] = atMs;
  b["fromState"]    = BAND_NAMES[from];
  b["toState"]      = BAND_NAMES[to];
  b["scorePct"]     = rnd(al.score, 1);
  b["method"]       = ALERT_METHOD;
  for (int c = 0; c < 2; c++) if (al.ch[c].has) b[String(CH_NAMES[c]) + "Ratio"] = rnd(al.ch[c].ratio, 4);
  if (al.test) b["test"] = true;
  b["notes"] = String("On-board spoilage alert (") + ALERT_METHOD + ") changed from " + BAND_NAMES[from] + " to " +
               BAND_NAMES[to] + (al.test ? ". TEST: score forced with Serial A" : "") +
               ". Device verdict only, not an independent food observation.";
  appendOutbox(e);
}

void bandChanged(int from, int to, uint64_t atMs, uint32_t up) {
  al.band = to;
  if (to == 2 || from == 2) al.silenced = false;      // a new Critical episode sounds again
  saveAlertNvs();
  Serial.printf("ALERT BAND %s -> %s at %s: score %.1f%%, mq1 ratio %s, mq2 ratio %s%s\n", BAND_NAMES[from], BAND_NAMES[to],
                alertTimeText(atMs, up).c_str(), al.score,
                al.ch[0].has ? String(al.ch[0].ratio, 3).c_str() : "-", al.ch[1].has ? String(al.ch[1].ratio, 3).c_str() : "-",
                al.test ? " (TEST)" : "");
  queueAlertEvent(from, to, atMs, up);
  if (to == 2) { beep(5); al.lastRemindMs = millis(); }
  else if (to == 1 && from == 0) beep(2);
}

void logAlert(uint32_t up, uint64_t atMs) {
  AlertLog &g = alLog[alLogHead];
  g.up = up; g.atMs = atMs; g.scored = al.scored; g.score = al.score; g.state = al.state; g.test = al.test;
  for (int c = 0; c < 2; c++) { g.has[c] = al.ch[c].has; g.ratio[c] = al.ch[c].ratio; }
  alLogHead = (alLogHead + 1) % ALERT_LOG_N;
  if (alLogN < ALERT_LOG_N) alLogN++;
}

// Once per one-minute record, food-test mode only (s1/s2 still hold the minute's statistics).
void evaluateAlert(uint32_t up, uint64_t atMs) {
  al.scored = false;
  al.test = false;
  for (auto &e : al.ch) e = ChanEval();
  if (!bl.any) {
    for (auto &e : al.ch) e.reason = "no_baseline";
    setAlertState(AL_NO_BASELINE, up);
    logAlert(up, atMs);
    return;
  }
  const SensorAcc *s[2] = {&s1, &s2};
  bool anyUsed = false;
  float score = 0;
  for (int c = 0; c < 2; c++) {
    ChanEval &e = al.ch[c];
    if (!bl.ok(c)) { e.reason = "no_baseline"; continue; }
    // A reading at the ADC rail still gives an Rs (an upper bound): it counts, marked saturated.
    if (s[c]->rs.n && s[c]->volt.mean() >= NO_SIGNAL_V) {
      e.has = true;
      e.ratio = s[c]->rs.mean() / bl.rs[c];
      e.pct = pctOf(e.ratio);
      e.saturated = s[c]->railHigh > 0;
    }
    if (up < SETTLE_S) { e.reason = "settling"; continue; }
    if (!e.has) { e.reason = "fault"; continue; }
    e.used = true;
    anyUsed = true;
    score = std::max(score, e.pct);
  }
  bool forced = alertTestLeft > 0 && up >= SETTLE_S;
  if (forced) {
    score = alertTestPct;
    alertTestLeft--;
    al.test = true;
    Serial.printf("!! TEST: score forced to %.1f%% (real %.1f%%), %d forced record(s) left - deviceAlert.test = true\n",
                  alertTestPct, anyUsed ? std::max(al.ch[0].used ? al.ch[0].pct : 0.0f, al.ch[1].used ? al.ch[1].pct : 0.0f) : 0.0f,
                  alertTestLeft);
  }
  if (!anyUsed && !forced) {               // last verdict kept (al.band), streaks restart
    for (int b = 0; b < 3; b++) al.rise[b] = al.fall[b] = 0;
    setAlertState(up < SETTLE_S ? AL_SETTLING : AL_FAULT, up);
    logAlert(up, atMs);
    return;
  }
  al.scored = true;
  al.score = score;
  for (int b = 1; b <= 2; b++) {
    al.rise[b] = score >= bandThreshold(b) ? al.rise[b] + 1 : 0;
    al.fall[b] = score < bandThreshold(b) - HYSTERESIS_PCT ? al.fall[b] + 1 : 0;
  }
  int band = al.band, nb = band;
  for (int b = 2; b > band; b--) if (al.rise[b] >= RISE_RECORDS) { nb = b; break; }
  if (nb == band) while (nb > 0 && al.fall[nb] >= FALL_RECORDS) nb--;
  if (nb != band) bandChanged(band, nb, atMs, up);
  setAlertState((AlertState)(AL_NORMAL + nb), up);
  logAlert(up, atMs);
}

// n = records of this boot in the window at the start; mq1N / mq2N = valid ones used per channel.
void putBaseline(JsonObject b, const Baseline &x) {
  if (x.ok(0)) b["mq1RsOhm"] = rnd(x.rs[0], 1);
  if (x.ok(1)) b["mq2RsOhm"] = rnd(x.rs[1], 1);
  b["n"] = x.window;
  b["mq1N"] = x.n[0];
  b["mq2N"] = x.n[1];
  if (x.fromMs) { b["fromMs"] = x.fromMs; b["toMs"] = x.toMs; }
}

String baselineJson(const Baseline &x) {
  JsonDocument d;
  putBaseline(d.to<JsonObject>(), x);
  String s;
  serializeJson(d, s);
  return s;
}

// deviceAlert of one record (stored with the record on flash; buildEnvelope moves it to the
// envelope and turns sinceUptimeS into sinceMs).
void putDeviceAlert(JsonObject o, bool foodTest) {
  o["method"] = ALERT_METHOD;
  o["state"]  = ALERT_NAMES[foodTest ? al.state : AL_OFF];
  if (!foodTest) return;
  if (al.scored) o["scorePct"] = rnd(al.score, 1);
  if (bl.any) o["band"] = BAND_NAMES[al.band];
  JsonObject chs = o["channels"].to<JsonObject>();
  for (int c = 0; c < 2; c++) {
    const ChanEval &e = al.ch[c];
    JsonObject x = chs[CH_NAMES[c]].to<JsonObject>();
    if (e.has) { x["ratio"] = rnd(e.ratio, 4); x["pct"] = rnd(e.pct, 1); }
    x["used"] = e.used;
    x["saturated"] = e.saturated;
    if (e.reason) x["reason"] = e.reason;
  }
  putBaseline(o["baseline"].to<JsonObject>(), bl);
  // Food reference: Rs ratio to the food's own early minutes, for comparison only (no score).
  JsonObject f = o["foodRef"].to<JsonObject>();
  f["state"] = FR_NAMES[fr.state];
  if (fr.state == FR_PENDING) f["dueMs"] = fr.dueMs;
  if (fr.state == FR_TAKEN) {
    putBaseline(f, fr.b);
    const SensorAcc *s[2] = {&s1, &s2};
    for (int c = 0; c < 2; c++)
      if (fr.b.ok(c) && s[c]->rs.n && s[c]->volt.mean() >= NO_SIGNAL_V)
        f[String(CH_NAMES[c]) + "Ratio"] = rnd(s[c]->rs.mean() / fr.b.rs[c], 4);
  }
  o["fullScaleRatio"] = FULL_SCALE_RATIO;
  o["warnPct"] = WARN_PCT;
  o["critPct"] = CRIT_PCT;
  o["sinceUptimeS"] = al.sinceUp;
  o["silenced"] = al.silenced;
  if (al.test) o["test"] = true;
}

// Silence button on the setup page: stops the Critical reminders for this episode only.
String silenceAlarm() {
  if (al.state != AL_CRITICAL) return "The alarm sounds only in Critical; nothing to silence";
  if (al.silenced) return "Already silenced for this Critical episode";
  al.silenced = true;
  saveAlertNvs();
  Serial.println(F("ALERT: Critical reminders silenced for this episode (state, LCD and upload unchanged)"));
  return "";
}

// Warning: 1 s on / 1 s off. Critical: 200 ms. Otherwise the normal logging blink.
uint32_t alertLedPeriod() {
  if (!curRun().foodTest) return 0;
  return al.state == AL_CRITICAL ? CRIT_LED_MS : al.state == AL_WARNING ? WARN_LED_MS : 0;
}

void serviceAlertLed() {
  static uint32_t lastMs = 0;
  static bool on = false, active = false;
  uint32_t period = alertLedPeriod();
  if (!period) {
    if (active) { digitalWrite(PIN_LED, LOW); on = active = false; }
    return;
  }
  active = true;
  if (millis() - lastMs >= period) {
    on = !on;
    digitalWrite(PIN_LED, on ? HIGH : LOW);
    lastMs = millis();
  }
}

// Critical: 5 pulses again every CRIT_REMIND_MS until silenced or the band drops.
void serviceAlertBuzzer() {
  if (al.state != AL_CRITICAL || al.silenced || !curRun().foodTest) return;
  if (al.lastRemindMs && millis() - al.lastRemindMs < CRIT_REMIND_MS) return;
  al.lastRemindMs = millis();
  Serial.println(F("ALERT: Critical reminder (5 pulses). Silence it on the setup page."));
  beep(5);
}

void setAlertTest(float pct) {
  if (!curRun().foodTest) { Serial.println(F("  A: ignored in calibration mode")); return; }
  if (!bl.any) { Serial.println(F("  A: this experiment has no baseline (state no_baseline) - nothing to force")); return; }
  alertTestLeft = ALERT_TEST_RECORDS;
  alertTestPct = pct;
  Serial.printf("!! TEST MODE: the next %d records score %.1f%% regardless of the sensors. They are marked \"test\": true\n"
                "!! in deviceAlert, and band changes they cause are real events in Firebase (with test: true).\n",
                ALERT_TEST_RECORDS, pct);
}

String baselineText() {
  if (!bl.any) return String("none (") + NO_BASELINE_MSG + ")";
  String t;
  for (int c = 0; c < 2; c++) {
    if (c) t += ", ";
    t += String(CH_NAMES[c]) + " ";
    t += bl.ok(c) ? String(bl.rs[c], 0) + " ohm (" + bl.n[c] + " rec)" : String("not usable (") + bl.n[c] + " valid rec)";
  }
  if (bl.fromMs) t += ", " + fmtWat(bl.fromMs) + " to " + fmtWat(bl.toMs).substring(11) + " WAT";
  return t;
}

void printAlertLog() {
  Serial.println(F("---------------- ALERT LOG (last evaluations, oldest first) ----------------"));
  if (!alLogN) Serial.println(F("  none yet (evaluated once per record in food-test mode)"));
  for (int i = 0; i < alLogN; i++) {
    const AlertLog &g = alLog[(alLogHead - alLogN + i + ALERT_LOG_N) % ALERT_LOG_N];
    Serial.printf("  %-22s score %-7s mq1 %-6s mq2 %-6s %s%s\n", alertTimeText(g.atMs, g.up).c_str(),
                  g.scored ? (String(g.score, 1) + "%").c_str() : "-",
                  g.has[0] ? String(g.ratio[0], 3).c_str() : "-", g.has[1] ? String(g.ratio[1], 3).c_str() : "-",
                  ALERT_NAMES[g.state], g.test ? " TEST" : "");
  }
  Serial.println(F("-----------------------------------------------------------------------------"));
}

// Appends one line to the run's current segment. A line torn by an earlier power cut
// or full flash is ended first, so the new record does not get glued onto it.
bool appendLine(RunInfo &r, uint32_t uptimeS, const String &line) {
  if (!fsOk) return false;
  uint32_t seg = uptimeS / SEGMENT_S;
  String path = segPath(r.run, seg);
  if (r.segs.empty() || r.segs.back() != seg) r.segs.push_back(seg);
  bool needNl = false;
  if (LittleFS.exists(path)) {
    File t = LittleFS.open(path, FILE_READ);
    if (t && t.size() > 0) { t.seek(t.size() - 1); needNl = t.read() != '\n'; }
    if (t) t.close();
  }
  File f = LittleFS.open(path, FILE_APPEND);
  if (!f) return false;
  if (needNl) f.print('\n');
  bool ok = f.print(line) == line.length();
  f.close();
  return ok;
}

void writeRecord(uint32_t nowMs) {
  if (!s1.raw.n) return;
  RunInfo &r = curRun();
  uint32_t up = nowMs / 1000;
  uint32_t ep = timeSynced ? (uint32_t)time(nullptr) : 0;   // 0: rebuilt from uptime after the sync

  JsonDocument d;
  d["epoch"]    = ep;
  d["uptime_s"] = up;
  lastRank1 = putSensor(d["mq1"].to<JsonObject>(), s1, up);
  lastRank2 = putSensor(d["mq2"].to<JsonObject>(), s2, up);
  d["free_heap"] = ESP.getFreeHeap();
  d["unsent"]    = totalUnsent() + 1;
  d["rejected"]  = rejectedCount;          // records in /rejected.jsonl, never uploaded (Part 3)
  if (gapMaxMs > GAP_LIMIT_MS) d["gap_s"] = rnd(gapMaxMs / 1000.0, 1);
  feedBaseline(up, lastRank1, lastRank2);
  if (!r.foodTest) serviceCalReady();
  if (r.foodTest) serviceFoodRef();
  if (r.foodTest) evaluateAlert(up, (uint64_t)ep * 1000ULL);
  putDeviceAlert(d["deviceAlert"].to<JsonObject>(), r.foodTest);

  String line;
  serializeJson(d, line);
  line += '\n';
  bool saved = appendLine(r, up, line);
  if (saved) { r.logged++; r.unsent++; }
  if (saved && !r.upPosValid) { r.upSeg = r.segs.back(); r.upOffset = 0; r.upPosValid = true; }

  // latest only ever gets the record just made, never a backfilled one.
  if (ep && uploadsEnabled && (int32_t)(nowMs - nextLatestMs) >= 0) {
    latestLine = line; latestRun = r.run; latestPending = true;
    while ((int32_t)(nowMs - nextLatestMs) >= 0) nextLatestMs += LATEST_PERIOD_MS;
  }

  lastV1 = s1.volt.mean(); lastV2 = s2.volt.mean(); haveRecord = true;
  if (!quietSerial) {
    String alTxt = r.foodTest ? String(" AL=") + ALERT_NAMES[al.state] + (al.scored ? " " + String(al.score, 1) + "%" : String()) +
                                    (al.test ? " TEST" : "")
                              : String();
    Serial.printf("REC %4lum up=%lu ep=%lu V1=%.3f V2=%.3f Rs1=%.0f Rs2=%.0f Q=%s/%s U=%lu%s%s latest:%s%s\n",
                  (unsigned long)(up / 60), (unsigned long)up, (unsigned long)ep,
                  lastV1, lastV2, s1.rs.mean(), s2.rs.mean(), QUALITY_NAMES[lastRank1], QUALITY_NAMES[lastRank2],
                  (unsigned long)totalUnsent(), gapMaxMs > GAP_LIMIT_MS ? " GAP" : "", alTxt.c_str(), lastLatestResult.c_str(),
                  saved ? "" : "  !! NOT SAVED TO FLASH");
  }
  s1.reset(); s2.reset(); gapMaxMs = 0;
  updateLcd();
  pruneFs();
}

// Called from loop() and from every wait loop in the network code, so that
// WiFi/upload never stop sampling for more than one HTTP request.
void serviceSampling() {
  uint32_t now = millis();
  if ((int32_t)(now - nextSampleMs) >= 0) {
    if (lastSampleMs && now - lastSampleMs > GAP_LIMIT_MS && now - lastSampleMs > gapMaxMs)
      gapMaxMs = now - lastSampleMs;
    sampleSensor(s1);
    sampleSensor(s2);
    lastSampleMs = now;
    nextSampleMs += SAMPLE_PERIOD_MS;
    if ((int32_t)(now - nextSampleMs) >= 0) nextSampleMs = now + SAMPLE_PERIOD_MS;  // fell behind
    if (!portalOpen && !alertLedPeriod()) blinkLed();   // the portal and Warning/Critical drive the LED themselves
  }
  if (!portalOpen) serviceAlertLed();
  if ((int32_t)(now - nextRecordMs) >= 0) {
    writeRecord(now);
    while ((int32_t)(now - nextRecordMs) >= 0) nextRecordMs += RECORD_PERIOD_MS;
  }
}

// =====================================================================
// Measurement envelope and run metadata (section 4 of the app team's spec)
// =====================================================================

// Builds the envelope for one stored record. false = no real epoch yet (never upload then).
bool buildEnvelope(JsonObject env, JsonDocument &rec, const RunInfo &r) {
  uint32_t up = rec["uptime_s"] | 0;
  uint32_t ep = rec["epoch"] | 0;
  const char *src = "ntp";
  if (!ep) {
    if (!r.bootEpochMs) return false;
    ep = (uint32_t)(r.bootEpochMs / 1000) + up;
    src = "rebuilt";
  }
  int q1 = qualityRank(rec["mq1"]["quality"].as<const char *>());
  int q2 = qualityRank(rec["mq2"]["quality"].as<const char *>());

  env["schemaVersion"] = SCHEMA_VERSION;
  env["deviceId"]      = DEVICE_ID;
  env["experimentId"]  = r.expId;
  env["sampleId"]      = r.sampleId;
  env["runId"]         = rid(r);
  env["measurementId"] = measurementId(r, up);
  env["measuredAtMs"]  = (uint64_t)ep * 1000ULL;
  env["uploadedAtMs"][".sv"] = "timestamp";               // Firebase server time
  env["expectedUpdateSeconds"] = LATEST_PERIOD_MS / 1000;
  env["mode"]          = r.foodTest ? "food_test" : "calibration";
  JsonObject cal = env["calibration"].to<JsonObject>();
  cal["state"]   = r.foodTest ? "pending_validation" : "collecting_baseline";
  cal["version"] = CAL_VERSION;
  env["quality"]["state"] = (q1 < 0 || q2 < 0) ? "unknown" : QUALITY_NAMES[std::max(q1, q2)];
  env["timeSource"] = src;
  env["sample"] = rec;
  env["sample"]["epoch"] = ep;
  // 1.2.0: the board's verdict is stored with the record; it goes next to sample, not in it.
  if (rec["deviceAlert"].is<JsonObject>()) {
    env["sample"].remove("deviceAlert");
    JsonObject da = env["deviceAlert"].to<JsonObject>();
    da.set(rec["deviceAlert"].as<JsonObjectConst>());
    if (da["sinceUptimeS"].is<uint32_t>()) {
      uint32_t since = da["sinceUptimeS"];
      da.remove("sinceUptimeS");
      if (r.bootEpochMs) da["sinceMs"] = r.bootEpochMs + (uint64_t)since * 1000ULL;
    }
  }
  return true;
}

void putChannelMeta(JsonObject c, const char *gas, int pin, float rl, float r0, float a, float b) {
  c["model"]     = "MQ135";
  c["curveGas"]  = gas;
  c["selective"] = false;
  c["note"]      = "MQ135 is not selective; curveGas names the datasheet curve used for ppm, not a measured gas";
  c["pin"]       = pin;
  c["rlOhm"]     = rl;
  c["r0Ohm"]     = r0;
  c["curveA"]    = a;
  c["curveB"]    = b;
  c["ppmFormula"] = "curveA * (Rs/R0)^curveB, Rs = rlOhm * (supplyV - Vout) / Vout";
}

void runMetaJson(const RunInfo &r, JsonObject d) {
  d["schemaVersion"]   = SCHEMA_VERSION;
  d["deviceId"]        = DEVICE_ID;
  d["experimentId"]    = r.expId;
  d["sampleId"]        = r.sampleId;
  d["runId"]           = rid(r);
  d["purpose"]         = r.foodTest ? "food_test" : "calibration";
  d["firmware"]        = FIRMWARE_LABEL;
  d["firmwareVersion"] = FIRMWARE_VERSION;

  d["bootCount"]       = r.boot;
  d["resetReason"]     = r.reset;
  d["runStartReason"]  = r.startReason;
  d["runStartUptimeS"] = r.startUptime;
  d["runIdAssignment"] = "run counter in NVS, kept above every run on flash; checked free in Firebase "
                         "before the run's first write. Separate from bootCount.";
  if (r.endReason.length()) {
    d["runEndReason"]  = r.endReason;
    d["runEndUptimeS"] = r.endUptime;
  }
  if (r.bootEpochMs) {
    d["bootEpochMs"]    = r.bootEpochMs;
    d["runStartedAtMs"] = r.bootEpochMs + (uint64_t)r.startUptime * 1000ULL;
    d["timeSyncedAtMs"] = r.syncedAtMs;
    if (r.endReason.length()) d["runEndedAtMs"] = r.bootEpochMs + (uint64_t)r.endUptime * 1000ULL;
  }

  JsonObject hw = d["hardware"].to<JsonObject>();
  hw["adcMax"]      = (int)ADC_MAX;
  hw["adcBits"]     = 12;
  hw["adcRefV"]     = ADC_REF;
  hw["supplyV"]     = VCC;
  hw["attenuation"] = ADC_ATTENUATION;   // no temperature/humidity sensor: no such field (brief 01)

  JsonObject ch = d["channels"].to<JsonObject>();
  putChannelMeta(ch["mq1"].to<JsonObject>(), "NH3", PIN_MQ1, RL1, R0_MQ1, NH3_A, NH3_B);
  putChannelMeta(ch["mq2"].to<JsonObject>(), "CO2", PIN_MQ2, RL2, R0_MQ2, CO2_A, CO2_B);

  JsonObject cal = d["calibration"].to<JsonObject>();
  cal["resistanceUnit"]     = "ohm";
  cal["calibrationVersion"] = CAL_VERSION;
  cal["calibrationState"]   = r.foodTest ? "pending_validation" : "collecting_baseline";
  cal["calibrationValidAfterRestart"] = false;   // the board cannot check this; the app decides

  JsonObject per = d["periods"].to<JsonObject>();
  per["samplePeriodS"]  = SAMPLE_PERIOD_MS / 1000;
  per["recordPeriodS"]  = RECORD_PERIOD_MS / 1000;
  per["latestPeriodS"]  = LATEST_PERIOD_MS / 1000;
  per["historyPeriodS"] = HISTORY_PERIOD_MS / 1000;

  d["samplesPath"] = samplesPath(r);
  d["latestPath"]  = LATEST_PATH;
  if (r.wifiSource.length()) {             // last connection of the run; never a password
    d["wifiSource"] = r.wifiSource;
    d["wifiSsid"]   = r.wifiSsid;
  }

  JsonObject st = d["stabilisation"].to<JsonObject>();
  st["settleS"]       = SETTLE_S;
  st["noSignalV"]     = NO_SIGNAL_V;
  st["nearAdcLimitV"] = NEAR_ADC_LIMIT_V;
  st["noiseVSd"]      = NOISE_V_SD;
  st["rules"] = "per channel: fault if mean V < noSignalV (no_signal), any reading at 4095 (saturation) "
                "or mean V > nearAdcLimitV (near_adc_limit); else settling while uptime < settleS; else valid. "
                "high_noise flag if within-minute V SD > noiseVSd (state unchanged). quality.state = worst channel. "
                "timeSource rebuilt = epoch reconstructed from uptime after the first clock sync in the same boot. "
                "Settling follows uptime since boot, so a run started after settleS is valid at once.";

  // 1.2.0: the on-board alert's constants, and the experiment's baseline for food-test runs.
  JsonObject da = d["deviceAlert"].to<JsonObject>();
  da["method"]                = ALERT_METHOD;
  da["fullScaleRatio"]        = FULL_SCALE_RATIO;
  da["warnPct"]               = WARN_PCT;
  da["critPct"]               = CRIT_PCT;
  da["hysteresisPct"]         = HYSTERESIS_PCT;
  da["riseRecords"]           = RISE_RECORDS;
  da["fallRecords"]           = FALL_RECORDS;
  da["critRemindS"]           = CRIT_REMIND_MS / 1000;
  da["baselineWindowRecords"] = BASELINE_WINDOW_RECORDS;
  da["baselineMinRecords"]    = BASELINE_MIN_RECORDS;
  da["baselineMaxAgeS"]       = BASELINE_MAX_AGE_S;
  da["rules"] = "baseline per channel = median mean-Rs of the records with quality valid, of the same boot and "
                "at most baselineMaxAgeS old, from the last baselineWindowRecords records before the experiment start; "
                "usable with >= baselineMinRecords. pct = 100*log10(Rs/baseline)/log10(fullScaleRatio), clamped 0..100; "
                "score = highest pct of the channels used (settled, Rs defined; ADC-rail readings count, saturated). "
                "Up a band after riseRecords records in a row at or above it; down after fallRecords records in a row "
                "below its threshold minus hysteresisPct. Device verdict only; the app makes the assessment.";
  da["foodRefDelayS"] = FOOD_REF_DELAY_S;
  da["foodRefLateS"]  = FOOD_REF_LATE_S;
  da["foodRefRules"]  = "record only, never used for the verdict: median mean-Rs per channel of the valid records of the "
                        "baselineWindowRecords records ending foodRefDelayS after the experiment start (food in and cooled), "
                        "same rules as the baseline; retried each record until foodRefLateS after due, then missed. "
                        "Envelopes give mq1Ratio / mq2Ratio = Rs mean / food reference.";
  if (r.foodTest && r.alertBaseline.length()) {
    JsonDocument b;
    if (!deserializeJson(b, r.alertBaseline)) da["baseline"] = b;
  }
  if (r.foodTest && r.foodRef.length()) {
    JsonDocument b;
    if (!deserializeJson(b, r.foodRef)) da["foodRef"] = b;
  }
}

// =====================================================================
// WiFi / time
// =====================================================================

// Joins one network within WIFI_TIMEOUT_MS. Gives up early when the Wi-Fi button
// changes position, so a press opens the portal without waiting for the attempt.
bool tryJoin(const char *ssid, const char *pass) {
  WiFi.disconnect();
  uint32_t t0 = millis();
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    serviceSampling();
    if (pollButton()) break;
    delay(20);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Saved network (setup page) first, then the one in secrets.h.
bool wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (savedSsid.length()) {
    if (tryJoin(savedSsid.c_str(), savedPass.c_str())) {
      curWifiSource = "saved"; curWifiSsid = savedSsid;
      return true;
    }
    if (btnEvent) { WiFi.disconnect(); return false; }
    if (secretsWifiOk) Serial.printf("  WiFi: saved network \"%s\" not reached in %lu s - trying secrets.h\n",
                                     savedSsid.c_str(), (unsigned long)(WIFI_TIMEOUT_MS / 1000));
  }
  if (secretsWifiOk && tryJoin(WIFI_SSID, WIFI_PASSWORD)) {
    curWifiSource = "secrets"; curWifiSsid = WIFI_SSID;
    return true;
  }
  WiFi.disconnect();      // no endless background retries with credentials that just failed
  return false;
}

// esp32 core 3.3.12: the first hostByName() after the IP state changes calls dns_clear_cache()
// without the lwIP lock. A DNS query SNTP still has pending then runs its callback from the
// loop task and lwIP asserts ("Required to lock TCPIP core functionality"). So SNTP starts only
// after the first lookup on a live connection, and no HTTPS request starts while WiFi is down.
// Runs in the lwIP task when SNTP has set the clock from a server.
void onSntpSync(struct timeval *) { sntpSynced = true; }

void startSntp() {
  if (sntpStarted || WiFi.status() != WL_CONNECTED) return;
  String host = dbBaseUrl();
  int p = host.indexOf("://");
  if (p >= 0) host.remove(0, p + 3);
  IPAddress ip;
  WiFi.hostByName(host.length() ? host.c_str() : "pool.ntp.org", ip);   // core clears its DNS cache here, safely
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  sntpStarted = true;
}

// Called at each new connection: the current run's metadata gets the network (never the password).
void noteWifiConnection() {
  if (!curWifiSource.length()) return;
  RunInfo &r = curRun();
  r.wifiSource = curWifiSource;
  r.wifiSsid   = curWifiSsid;
  r.metaDirty  = true;
  writeMetaFile(r);
}

// Part 4b fix. 1.0.0 took the first clock value >= EPOCH_VALID as the sync. After a soft
// reset (TASK_WDT, PANIC, SW) the ESP32 keeps the previous boot's clock, carried across
// the reset by the inaccurate RTC slow clock, so R00063 (TASK_WDT) took a stale clock
// about 85 s fast and got a boot time later than R00064's. Now setup() zeroes the clock
// and only the SNTP callback of THIS boot (sntpSynced) counts as a sync. bootEpochMs and
// every rebuilt time come from that sync only.
void checkTimeSync() {
  if (timeSynced || !sntpSynced) return;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < EPOCH_VALID) return;
  uint64_t syncMs = (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
  uint64_t bootMs = syncMs - millis();
  uint64_t prevBootMs = 0;
  for (auto &r : runs) {
    if (r.boot != bootCount) { prevBootMs = std::max(prevBootMs, r.bootEpochMs); continue; }
    if (r.bootEpochMs) continue;
    r.bootEpochMs = bootMs;
    r.syncedAtMs  = syncMs;
    writeMetaFile(r);
  }
  if (prevBootMs > bootMs)
    Serial.printf("  !! NTP: this boot (%llu ms) is earlier than an earlier run's boot (%llu ms); the older value came from 1.0.0\n",
                  (unsigned long long)bootMs, (unsigned long long)prevBootMs);
  timeSynced = true;
  time_t wat = tv.tv_sec + 3600;
  struct tm tmWat; gmtime_r(&wat, &tmWat);
  char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmWat);
  Serial.printf("  NTP: clock set, %s WAT. Boot epoch %llu ms; earlier records of this boot get rebuilt times\n",
                tbuf, (unsigned long long)bootMs);
  flushWifiEvent();
  if (uploadsEnabled) { historyRequested = true; historyReason = "time sync"; }
}

// =====================================================================
// Firebase auth (dedicated email/password user, refreshed with its refresh token)
// =====================================================================

bool storeTokens(const String &body, const char *idKey, const char *refreshKey, const char *expiresKey, const char *uidKey) {
  JsonDocument filter;
  filter[idKey] = true; filter[refreshKey] = true; filter[expiresKey] = true; filter[uidKey] = true;
  JsonDocument resp;
  if (deserializeJson(resp, body, DeserializationOption::Filter(filter))) return false;
  const char *id = resp[idKey] | "";
  const char *rt = resp[refreshKey] | "";
  const char *uid = resp[uidKey] | "";
  if (!*id) return false;
  idToken = id;
  if (*rt && refreshToken != rt) {
    refreshToken = rt;
    prefs.putString("rtok", refreshToken);
    prefs.putString("rtokFor", emailTag());
  }
  if (*uid) {
    if (producerUid != uid) { producerUid = uid; prefs.putString("uid", producerUid); }
    if (!uidPrinted) { Serial.printf("  Producer UID: %s\n", producerUid.c_str()); uidPrinted = true; }
  }
  long expires = atol(resp[expiresKey] | "3600");
  if (expires <= 0) expires = 3600;
  tokenExpiryMs = millis() + (uint32_t)expires * 1000UL;
  tokenValid = true;
  return true;
}

// Prints only Firebase's error code (e.g. INVALID_LOGIN_CREDENTIALS), never the request.
void printAuthError(const char *what, int code, const String &body) {
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument e;
  const char *msg = "";
  if (!deserializeJson(e, body, DeserializationOption::Filter(filter))) msg = e["error"]["message"] | "";
  Serial.printf("  Auth: %s HTTP %d %s\n", what, code, msg);
}

int authSignIn() {
  if (WiFi.status() != WL_CONNECTED) return -1;   // no DNS lookup without an IP (see startSntp)
  HTTPClient http;
  http.setReuse(false); http.setConnectTimeout(8000); http.setTimeout(10000);
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;
  if (!http.begin(tls, url)) return -100;
  http.addHeader("Content-Type", "application/json");
  String req;
  {
    JsonDocument d;
    d["email"] = DEVICE_EMAIL;
    d["password"] = DEVICE_PASSWORD;
    d["returnSecureToken"] = true;
    serializeJson(d, req);
  }
  int code = http.POST(req);
  req = String();
  String body = http.getString();
  http.end();
  if (code == 200 && !storeTokens(body, "idToken", "refreshToken", "expiresIn", "localId")) code = -101;
  if (code == 200) Serial.println(F("  Auth: signed in as the device user"));
  else printAuthError("sign-in", code, body);
  return code;
}

int authRefresh() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.setReuse(false); http.setConnectTimeout(8000); http.setTimeout(10000);
  String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
  if (!http.begin(tls, url)) return -100;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST("grant_type=refresh_token&refresh_token=" + refreshToken);
  String body = http.getString();
  http.end();
  if (code == 200 && !storeTokens(body, "id_token", "refresh_token", "expires_in", "user_id")) code = -101;
  if (code != 200) printAuthError("token refresh", code, body);
  return code;
}

bool ensureToken() {
  if (tokenValid && (int32_t)(tokenExpiryMs - millis()) > (int32_t)TOKEN_MARGIN_MS) return true;
  tokenValid = false;
  if (refreshToken.length()) {
    lastAuthCode = authRefresh();
    serviceSampling();
    if (lastAuthCode == 200) return true;
    // Network trouble: try again later rather than signing in again.
    if (lastAuthCode < 400 || lastAuthCode >= 500) return false;
    Serial.println(F("  Auth: refresh token rejected - signing in again"));
    refreshToken = "";
    prefs.remove("rtok");
  }
  // A rejected password is not retried every minute (Firebase locks out repeated failures).
  if (signInFailMs && millis() - signInFailMs < AUTH_RETRY_MS) return false;
  lastAuthCode = authSignIn();
  serviceSampling();
  if (lastAuthCode == 200) { signInFailMs = 0; return true; }
  if (lastAuthCode >= 400 && lastAuthCode < 500) signInFailMs = millis() | 1;
  return false;
}

// =====================================================================
// Firebase upload
// =====================================================================

// One RTDB REST request. resp gets the body of a 200 answer. shallow: keys only.
// ifMatch: write only if the node still has this ETag (HTTP 412 otherwise).
// etagOut: ask for and return the node's ETag. The firmware never sends DELETE.
int fbRequest(const char *method, const String &path, const String &body, String *resp = nullptr,
              bool shallow = false, const String *ifMatch = nullptr, String *etagOut = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.setReuse(false); http.setConnectTimeout(8000); http.setTimeout(10000);
  String url = dbBaseUrl() + "/" + path + ".json?auth=" + idToken + (shallow ? "&shallow=true" : "");
  if (!http.begin(tls, url)) return -100;
  http.addHeader("Content-Type", "application/json");
  if (etagOut) {
    http.addHeader("X-Firebase-ETag", "true");
    const char *keys[] = {"ETag"};
    http.collectHeaders(keys, 1);
  }
  if (ifMatch) http.addHeader("if-match", *ifMatch);
  int code = http.sendRequest(method, body);
  bool isWrite = strcmp(method, "GET") != 0;
  if (code == 200) {
    if (resp) *resp = http.getString();
    if (etagOut) *etagOut = http.header("ETag");
    if (isWrite) writeDenied = false; else readDenied = false;
  } else {
    lastResp = http.getString().substring(0, 120);
    static String lastFail;                // the same failure every minute is printed once
    String fail = String(method) + path + code;
    if (fail != lastFail) Serial.printf("  %s %s -> HTTP %d %s\n", method, path.c_str(), code, lastResp.c_str());
    lastFail = fail;
    // RTDB answers a rules rejection with 401 "Permission denied": the token is fine.
    if ((code == 401 || code == 403) && lastResp.indexOf("ermission") >= 0) {
      if (isWrite) writeDenied = true; else readDenied = true;
    } else if (code == 401) tokenValid = false;
  }
  http.end();
  return code;
}

int fbSend(const char *method, const String &path, const String &body) { return fbRequest(method, path, body); }

// Shallow GET: 1 = the node exists, 0 = it does not, <0 / >1 = HTTP failure code.
int fbExists(const String &path, String *etagOut = nullptr) {
  String body;
  int code = fbRequest("GET", path, "", &body, etagOut == nullptr, nullptr, etagOut);
  serviceSampling();
  if (code != 200) return code == 0 ? -1 : code;
  body.trim();
  return body == "null" ? 0 : 1;
}

// Lowest runId number not used by another run kept on this board.
bool localIdTaken(uint32_t idNum, const RunInfo *self) {
  for (auto &o : runs) if (&o != self && o.idNum == idNum) return true;
  return false;
}

// Part 4a fix. Before the first write of a run, make sure its runId is new in Firebase:
// a shallow read of telemetry/FRIDGE001/runs gives the highest runId there (once per boot),
// a run of this boot starts above it, and a shallow GET of telemetry/FRIDGE001/runs/<runId>
// plus a GET of the run record (with its ETag, for a conditional first PUT) confirm the
// candidate. Taken: next number, logged on Serial. false = could not check yet (offline or
// no read permission); the run then waits on the board and nothing is written for it.
bool verifyRun(RunInfo &r, int &lastCode) {
  if (r.idOk) return true;
  if (!ensureToken()) { lastCode = lastAuthCode; return false; }
  if (!remoteMaxKnown) {
    String body;
    lastCode = fbRequest("GET", RUNS_PATH, "", &body, true);
    serviceSampling();
    if (lastCode != 200) {
      if (readDenied) Serial.println(F("  runId check: READ DENIED - the board must be allowed to read telemetry/FRIDGE001/runs and experiments/*/runs; data waits on the board"));
      return false;
    }
    JsonDocument keys;
    if (!deserializeJson(keys, body) && keys.is<JsonObject>()) {
      for (JsonPair kv : keys.as<JsonObject>()) {
        unsigned long n = 0;
        if (sscanf(kv.key().c_str(), "R%lu", &n) == 1) remoteMaxRun = std::max(remoteMaxRun, (uint32_t)n);
      }
    }
    remoteMaxKnown = true;
    Serial.printf("  runId check: highest runId in Firebase is %s\n", remoteMaxRun ? runIdOf(remoteMaxRun).c_str() : "none");
  }
  uint32_t cand = r.idNum;
  if (r.boot == bootCount && cand <= remoteMaxRun) cand = remoteMaxRun + 1;   // a new run goes above everything
  for (int tries = 0; tries < RUNID_MAX_TRIES; tries++, cand++) {
    if (localIdTaken(cand, &r)) continue;
    String id = runIdOf(cand);
    String exp = r.foodTest ? r.expId : "CAL-" + id;
    int t = fbExists(String(RUNS_PATH) + "/" + id);
    if (t < 0 || t > 1) { lastCode = t; return false; }
    String etag;
    int m = t ? 1 : fbExists("experiments/" + exp + "/runs/" + id, &etag);
    if (m < 0 || m > 1) { lastCode = m; return false; }
    if (t || m) {
      Serial.printf("  runId check: %s already exists in Firebase (%s) - not reused, trying the next one\n",
                    id.c_str(), t ? "samples" : "run record");
      continue;
    }
    if (cand != r.idNum) {
      Serial.printf("  runId check: local run %lu now uses %s (was %s)\n", (unsigned long)r.run, id.c_str(), rid(r).c_str());
      r.idNum = cand;
      if (!r.foodTest) r.expId = exp;
      prefs.putUInt(nvsKey('i', r.run).c_str(), cand);   // outbox entries of this run resolve through it
    }
    if (cand > runCounter) { runCounter = cand; prefs.putUInt("run", runCounter); }
    remoteMaxRun = std::max(remoteMaxRun, cand);
    r.idOk = true;
    r.etag = etag;
    writeMetaFile(r);
    return true;
  }
  Serial.println(F("  runId check: no free runId found - trying again at the next upload"));
  lastCode = -2;
  return false;
}

void putLatest() {
  latestPending = false;
  if (latestRun != curRun().run) return;           // experiment changed since: drop it
  if (!ensureToken()) {
    lastLatestResult = "FAIL auth " + String(lastAuthCode);
    lastUploadOk = false;
    return;
  }
  int vcode = 0;
  if (!verifyRun(curRun(), vcode)) {               // latest carries the runId: only once it is checked
    lastLatestResult = "WAIT runId check " + String(vcode);
    lastUploadOk = false;
    return;
  }
  JsonDocument rec;
  if (deserializeJson(rec, latestLine)) return;
  JsonDocument env;
  if (!buildEnvelope(env.to<JsonObject>(), rec, curRun())) return;
  rec.clear();
  String body;
  serializeJson(env, body);
  env.clear();
  int code = fbSend("PUT", LATEST_PATH, body);
  serviceSampling();
  lastUploadOk = code == 200;
  lastLatestResult = code == 200 ? String("OK") : "FAIL " + String(code);
}

// Reads up to CHUNK_RECORDS unsent records of run r as envelopes keyed by measurementId.
// Files are closed again before returning, so records can be appended between chunks.
int readChunk(const RunInfo &r, JsonDocument &out, uint32_t &endSeg, size_t &endOffset, uint32_t &lastUptime,
              int maxRecords = CHUNK_RECORDS) {
  int n = 0;
  endSeg = r.upSeg; endOffset = r.upOffset;
  for (uint32_t seg : r.segs) {
    if (r.upPosValid && seg < r.upSeg) continue;
    File f = LittleFS.open(segPath(r.run, seg), FILE_READ);
    if (!f) continue;
    if (r.upPosValid && seg == r.upSeg) f.seek(r.upOffset);
    while (n < maxRecords && f.available()) {
      String line = f.readStringUntil('\n');
      endSeg = seg; endOffset = f.position();
      if (line.length() < 2) continue;
      JsonDocument rec;
      if (deserializeJson(rec, line)) continue;  // torn line from a power cut
      uint32_t up = rec["uptime_s"] | 0;
      if (up <= r.sentUptime) continue;
      if (!buildEnvelope(out[measurementId(r, up)].to<JsonObject>(), rec, r)) continue;
      lastUptime = up;
      n++;
    }
    if (!f.available()) { endSeg = seg; endOffset = f.position(); }
    f.close();
    if (n >= maxRecords) break;
  }
  return n;
}

// Run metadata, restart event and every unsent record of one run. false = stop this upload.
bool uploadRun(RunInfo &r, int &sent, int &lastCode) {
  uint8_t target = r.bootEpochMs ? 2 : 1;
  if (r.metaState < target || r.metaDirty) {
    if (!verifyRun(r, lastCode)) return false;    // nothing is written before the runId is checked
    JsonDocument d;
    runMetaJson(r, d.to<JsonObject>());
    String body;
    serializeJson(d, body);
    if (r.metaState == 0 && r.etag.length()) {
      // First write: PUT only while the run record is still the empty node checked in verifyRun().
      lastCode = fbRequest("PUT", runMetaPath(r), body, nullptr, false, &r.etag);
      if (lastCode == 412) {
        Serial.printf("  %s: run record appeared since the check - choosing another runId\n", rid(r).c_str());
        r.idOk = false; r.etag = "";
        writeMetaFile(r);
      }
    } else {
      lastCode = fbSend("PATCH", runMetaPath(r), body);
    }
    serviceSampling();
    if (lastCode != 200) return false;
    r.metaDirty = false;
    r.etag = "";
    if (r.metaState < target) {
      r.metaState = target;
      prefs.putUChar(nvsKey('m', r.run).c_str(), target);
    }
  }
  if (r.foodTest && r.startReason == "boot" && r.bootEpochMs && !r.eventSent) {
    JsonDocument d;
    d["experimentId"] = r.expId;
    d["runId"]        = rid(r);
    d["type"]         = "device_restart";
    d["observedAtMs"] = r.bootEpochMs;
    d["resetReason"]  = r.reset;
    d["notes"] = r.reset == "BROWNOUT"
      ? "Board restarted after a BROWNOUT reset (supply voltage sagged). Outage start and duration unknown to the board."
      : "Board restarted. Outage start and duration unknown to the board.";
    String body;
    serializeJson(d, body);
    lastCode = fbSend("PUT", "experiments/" + r.expId + "/events/" + rid(r) + "-restart", body);
    serviceSampling();
    if (lastCode != 200) return false;
    r.eventSent = true;
    prefs.putBool(nvsKey('v', r.run).c_str(), true);
  }
  if (!r.bootEpochMs || !r.unsent) return true;    // never upload a record without a real epoch
  int perChunk = CHUNK_RECORDS;
  while (true) {
    JsonDocument chunk;
    uint32_t endSeg, lastUptime = 0;
    size_t endOffset;
    int n = readChunk(r, chunk, endSeg, endOffset, lastUptime, perChunk);
    if (n <= 0) { r.upSeg = endSeg; r.upOffset = endOffset; r.upPosValid = true; r.unsent = 0; return true; }
    String body;
    serializeJson(chunk, body);
    chunk.clear();
    lastCode = fbSend("PATCH", samplesPath(r), body);
    serviceSampling();
    bool rejected = false;
    if (lastCode == 400) {
      // Firebase refuses the content itself: it will refuse it every time. Find the record
      // one by one and move it to the rejected file instead of retrying it for ever.
      if (n > 1) { perChunk = 1; continue; }
      JsonDocument d;
      d["reason"] = "HTTP 400 " + lastResp;
      d["runId"]  = rid(r);
      JsonDocument env;
      if (!deserializeJson(env, body)) d["envelopes"] = env;
      if (!appendRejected(d)) return false;
      addRejected(1);
      Serial.printf("  REJECTED %s uptime %lu: HTTP 400 - moved to %s\n", rid(r).c_str(), (unsigned long)lastUptime, REJECTED_PATH);
      rejected = true;
    } else if (lastCode != 200) {
      return false;
    }
    body = String();
    r.upSeg = endSeg; r.upOffset = endOffset; r.upPosValid = true;
    r.sentUptime = lastUptime;
    prefs.putUInt(nvsKey('s', r.run).c_str(), lastUptime);
    r.unsent = r.unsent > (uint32_t)n ? r.unsent - n : 0;
    if (!rejected) sent += n;
    if (!rejected) Serial.printf("  %s: sent %d records, %lu left\n", rid(r).c_str(), n, (unsigned long)r.unsent);
    updateLcd();
    serviceSampling();
  }
}

// =====================================================================
// Outbox: meta, observations and events created on the board (page, commands, Serial)
// =====================================================================
// One JSON line per write: {"m":"PUT"|"PATCH","p":path,"b":body,"r":localRun,"own":ms,"new":true}.
// "r": path "{RUN}" and body "runId" are filled with that run's checked runId at upload time.
// "own": a meta write; experiments/<id>/meta/startedAtMs must be this value (or absent for
// "new"), so the board never overwrites a meta record of another experiment.

RunInfo *findRun(uint32_t run) {
  for (auto &r : runs) if (r.run == run) return &r;
  return nullptr;
}

bool appendOutbox(JsonDocument &e) {
  if (!fsOk) return false;
  bool needNl = false;
  {
    File t = LittleFS.open(OUTBOX_PATH, FILE_READ);
    if (t && t.size() > 0) { t.seek(t.size() - 1); needNl = t.read() != '\n'; }
    if (t) t.close();
  }
  File f = LittleFS.open(OUTBOX_PATH, FILE_APPEND);
  if (!f) return false;
  if (needNl) f.print('\n');
  serializeJson(e, f);
  bool ok = f.print('\n') == 1;
  f.close();
  if (ok) outboxPending++;
  if (uploadsEnabled) { historyRequested = true; historyReason = "new experiment record"; }
  return ok;
}

// Counts the lines not uploaded yet (boot).
void scanOutbox() {
  outboxOffset = prefs.getUInt("obOff", 0);
  outboxPending = 0;
  File f = LittleFS.open(OUTBOX_PATH, FILE_READ);
  if (!f) { outboxOffset = 0; return; }
  if (outboxOffset > f.size()) outboxOffset = 0;
  f.seek(outboxOffset);
  while (f.available()) if (f.readStringUntil('\n').length() > 1) outboxPending++;
  f.close();
}

void outboxAdvance(uint32_t next) {
  outboxOffset = next;
  prefs.putUInt("obOff", outboxOffset);
  if (outboxPending) outboxPending--;
}

void rejectOutboxLine(const String &reason, JsonVariantConst entry, const String &raw) {
  JsonDocument d;
  d["reason"] = reason;
  if (raw.length()) d["raw"] = raw; else d["outbox"] = entry;
  appendRejected(d);
  addRejected(1);
  Serial.printf("  REJECTED outbox record: %s - moved to %s\n", reason.c_str(), REJECTED_PATH);
}

// Uploads the outbox in order. false = stop this upload (network, rules, runId check).
bool processOutbox(int &lastCode, int &sent) {
  while (outboxPending > 0) {
    File f = LittleFS.open(OUTBOX_PATH, FILE_READ);
    if (!f) { outboxPending = 0; break; }
    if (outboxOffset >= f.size()) { f.close(); outboxPending = 0; break; }   // count was off: nothing left
    f.seek(outboxOffset);
    String line = f.readStringUntil('\n');
    uint32_t next = f.position();
    f.close();
    line.trim();
    if (line.length() < 2) { outboxAdvance(next); outboxPending++; continue; }   // blank line: not counted
    JsonDocument e;
    if (deserializeJson(e, line)) { rejectOutboxLine("torn line (power cut while saving)", e, line); outboxAdvance(next); continue; }
    String path = e["p"] | "";
    RunInfo *r = nullptr;
    if (e["r"].is<uint32_t>()) {
      uint32_t lrun = e["r"];
      r = findRun(lrun);
      uint32_t idNum;
      if (r) {
        if (!verifyRun(*r, lastCode)) return false;
        idNum = r->idNum;
      } else {
        idNum = prefs.getUInt(nvsKey('i', lrun).c_str(), lrun);   // run already pruned from flash
      }
      String id = runIdOf(idNum);
      path.replace("{RUN}", id);
      if (e["b"]["runId"].is<const char *>()) e["b"]["runId"] = id;
    }
    // "up": made before this boot's time sync (1.2.0 alert events); the time comes from the run's boot.
    if (e["up"].is<uint32_t>()) {
      uint64_t boot = r ? r->bootEpochMs : 0;
      if (!boot && r && r->boot == bootCount) { lastCode = -3; return false; }   // clock not set yet: wait
      if (!boot) {
        rejectOutboxLine("time never synced in the boot that made it; no real observedAtMs can be given", e, "");
        outboxAdvance(next);
        continue;
      }
      e["b"]["observedAtMs"] = boot + (uint64_t)e["up"].as<uint32_t>() * 1000ULL;
    }
    uint64_t own = e["own"].as<unsigned long long>();
    if (own) {
      String cur;
      lastCode = fbRequest("GET", path + "/startedAtMs", "", &cur);
      serviceSampling();
      if (lastCode != 200) return false;
      cur.trim();
      bool mine = cur != "null" && strtoull(cur.c_str(), nullptr, 10) == own;
      bool free = cur == "null" && (e["new"] | false);
      if (!mine && !free) {
        rejectOutboxLine(path + " belongs to another experiment (startedAtMs " + cur + "); not overwritten", e, "");
        outboxAdvance(next);
        continue;
      }
    }
    String body;
    serializeJson(e["b"], body);
    lastCode = fbSend(e["m"] | "PUT", path, body);
    serviceSampling();
    if (lastCode == 400) {
      rejectOutboxLine("HTTP 400 " + lastResp, e, "");
    } else if (lastCode != 200) {
      return false;
    } else {
      sent++;
      Serial.printf("  %s %s: OK\n", (const char *)(e["m"] | "PUT"), path.c_str());
    }
    outboxAdvance(next);
  }
  // Fully uploaded and large: start a new file (everything in it is in Firebase).
  File f = LittleFS.open(OUTBOX_PATH, FILE_READ);
  if (f) {
    size_t size = f.size();
    f.close();
    if (outboxPending == 0 && outboxOffset >= size && size > OUTBOX_KEEP_BYTES) {
      LittleFS.remove(OUTBOX_PATH);
      outboxOffset = 0;
      prefs.putUInt("obOff", 0);
    }
  }
  return true;
}

// experiments/<experimentId>/events/<runId>-wifi-<epoch>: explains an upload gap during a trial.
// Goes to the outbox as soon as this boot has a real time (until then it waits in RAM).
void flushWifiEvent() {
  if (!wifiEvt.pending || !timeSynced) return;
  uint64_t atMs = curRun().bootEpochMs + wifiEvt.atMillis;
  JsonDocument e;
  e["m"] = "PUT";
  e["p"] = "experiments/" + wifiEvt.expId + "/events/{RUN}-wifi-" + String((unsigned long)(atMs / 1000));
  e["r"] = wifiEvt.run;
  JsonObject b = e["b"].to<JsonObject>();
  b["experimentId"] = wifiEvt.expId;
  b["runId"]        = "";
  b["type"]         = "other";
  b["observedAtMs"] = atMs;
  b["notes"]        = "Wi-Fi changed to " + wifiEvt.ssid + " via setup page";
  if (appendOutbox(e)) wifiEvt.pending = false;
}

// =====================================================================
// Experiments: one set of functions for the setup page, Firebase commands and Serial
// =====================================================================

const char *VIA_NOTE[] = {"from the setup page", "by Firebase command "};
int viaIndex(const char *via) { return !strcmp(via, "firebase_command") ? 1 : 0; }

bool idUsed(const String &id) {
  if (id == nvsExp) return true;
  for (auto &r : runs) if (r.foodTest && r.expId == id) return true;
  File f = LittleFS.open(USED_IDS_PATH, FILE_READ);
  if (!f) return false;
  bool used = false;
  while (!used && f.available()) { String l = f.readStringUntil('\n'); l.trim(); used = l == id; }
  f.close();
  return used;
}

// Optional free text: copied when given, otherwise listed in notRecorded.
bool optText(JsonVariantConst src, JsonObject dst, const char *key, int maxChars, JsonArray nr,
             const char *nrName, String &err) {
  String v = src | "";
  if (!cleanText(v, maxChars)) { err = String(nrName) + ": at most " + maxChars + " characters"; return false; }
  if (v.length()) dst[key] = v;
  else if (!nr.isNull()) nr.add(nrName);
  return true;
}

// Optional time in Unix ms: a real time, not later than the board time (+ slack).
bool optTime(JsonVariantConst src, JsonObject dst, const char *key, JsonArray nr, const char *nrName,
             uint64_t now, String &err) {
  if (src.isNull() || (src.is<unsigned long long>() && src.as<unsigned long long>() == 0)) { nr.add(nrName); return true; }
  uint64_t t = src.as<unsigned long long>();
  if (!src.is<unsigned long long>() || t < (uint64_t)EPOCH_VALID * 1000ULL || t > now + FUTURE_SLACK_MS) {
    err = String(nrName) + " is not a valid time (Unix ms, not in the future)";
    return false;
  }
  dst[key] = t;
  return true;
}

// Next suggestion counter NN for <FOOD>-YYYYMMDD-NN (kept in NVS per WAT day).
uint32_t nextDayNumber() {
  uint32_t day = watDay();
  return prefs.getUInt("nnDay", 0) == day ? prefs.getUInt("nnCnt", 0) + 1 : 1;
}

// Start: in = the start fields with the meta record's names and nesting:
//   experimentId, sampleId, food{name, cookedAtMs, recipe, portion{amount, unit}, priorStorage},
//   storedAtMs, storage{location, container, cover, otherFood}, notes
// via: "setup_page" or "firebase_command" (cmd = the command object).
// Returns "" when applied, otherwise the reason it was refused. Nothing changes when refused.
String startExperimentChecked(JsonObjectConst in, const char *via, JsonObjectConst cmd) {
  if (!timeSynced) return "Waiting for time sync";
  if (nvsExp.length()) return "End the current experiment first (" + nvsExp + " is running)";
  String exp = normId(in["experimentId"] | ""), smp = normId(in["sampleId"] | "");
  if (!exp.length()) return "experimentId: 3 to 32 characters, only A-Z, 0-9 and -";
  if (!smp.length()) return "sampleId: 3 to 32 characters, only A-Z, 0-9 and -";
  if (exp.startsWith("CAL-") || exp == "NONE" || smp == "NONE") return "CAL-... and NONE are reserved for calibration mode";
  if (idUsed(exp)) return "experimentId " + exp + " was already used on this board";
  uint64_t now = nowMs();

  JsonDocument meta;
  String err;
  meta["schemaVersion"] = SCHEMA_VERSION;
  meta["experimentId"]  = exp;
  meta["deviceId"]      = DEVICE_ID;
  meta["sampleId"]      = smp;
  JsonObject food = meta["food"].to<JsonObject>();
  JsonObject sto  = meta["storage"].to<JsonObject>();
  JsonArray nr    = meta["notRecorded"].to<JsonArray>();
  JsonObjectConst inFood = in["food"], inSto = in["storage"];
  String name = inFood["name"] | "";
  if (!cleanText(name, 40) || !name.length()) return "Food name is required, up to 40 characters";
  food["name"]        = name;
  food["category"]    = "cooked_meal";
  food["preparation"] = "cooked";
  if (!optTime(inFood["cookedAtMs"], food, "cookedAtMs", nr, "food.cookedAtMs", now, err)) return err;
  if (!optText(inFood["recipe"], food, "recipe", 120, nr, "food.recipe", err)) return err;
  if (!optText(inFood["priorStorage"], food, "priorStorage", 120, nr, "food.priorStorage", err)) return err;
  JsonObjectConst por = inFood["portion"];
  if (por.isNull()) nr.add("food.portion");
  else {
    float amount = por["amount"] | 0.0f;
    String unit = por["unit"] | "";
    if (!(amount > 0 && amount < 1e6f) || !(unit == "g" || unit == "kg" || unit == "ml" || unit == "pieces"))
      return "Portion: a number above 0 and a unit g, kg, ml or pieces";
    food["portion"]["amount"] = amount;
    food["portion"]["unit"]   = unit;
  }
  meta["startedAtMs"] = now;
  if (!optTime(in["storedAtMs"], meta.as<JsonObject>(), "storedAtMs", nr, "storedAtMs", now, err)) return err;
  meta["purpose"] = "food_test";
  meta["status"]  = "running";
  String loc = inSto["location"] | "";
  if (!cleanText(loc, 80) || !loc.length()) return "Storage location is required, up to 80 characters";
  sto["location"] = loc;
  if (!optText(inSto["container"], sto, "container", 60, nr, "storage.container", err)) return err;
  String cover = inSto["cover"] | "";
  if (cover == "covered" || cover == "open") sto["cover"] = cover;
  else if (cover.length() && cover != "not recorded") return "Covered or open: covered, open or not recorded";
  else nr.add("storage.cover");
  if (!optText(inSto["otherFood"], sto, "otherFood", 120, nr, "storage.otherFood", err)) return err;
  if (!optText(in["notes"], meta.as<JsonObject>(), "notes", 200, JsonArray(), "notes", err)) return err;
  if (!nr.size()) meta.remove("notRecorded");
  meta["startedVia"] = via;

  // Checks passed: apply. IDs to NVS first, so they survive a power cut from here on.
  nvsExp = exp; nvsSample = smp; nvsFood = name;
  prefs.putString("exp", nvsExp);
  prefs.putString("smp", nvsSample);
  prefs.putString("food", nvsFood);
  prefs.putULong64("expStart", now);
  File u = LittleFS.open(USED_IDS_PATH, FILE_APPEND);
  if (u) { u.println(exp); u.close(); }
  uint32_t day = watDay();
  prefs.putUInt("nnCnt", nextDayNumber());
  prefs.putUInt("nnDay", day);
  alertStart(exp);                    // 1.2.0: baseline from the minutes before the start
  startRun("experiment_start");
  RunInfo &r = curRun();

  JsonDocument m;
  m["m"] = "PUT";                     // only the fields entered plus the fixed values
  m["p"] = "experiments/" + exp + "/meta";
  m["own"] = now;
  m["new"] = true;
  m["b"] = meta;
  appendOutbox(m);

  JsonDocument e;
  e["m"] = "PUT";
  e["p"] = "experiments/" + exp + "/events/{RUN}-experiment-start";
  e["r"] = r.run;
  JsonObject b = e["b"].to<JsonObject>();
  b["experimentId"] = exp;
  b["runId"]        = "";
  b["sampleId"]     = smp;
  b["type"]         = "other";
  b["observedAtMs"] = now;
  b["startedAtMs"]  = now;
  int vi = viaIndex(via);
  b["notes"] = String("Experiment started ") + VIA_NOTE[vi] + (vi == 1 ? String(cmd["commandId"] | "") : String());
  if (vi == 1) {
    b["commandId"]  = cmd["commandId"];
    b["issuedAtMs"] = cmd["issuedAtMs"];
    b["issuedBy"]   = cmd["issuedBy"];
  }
  appendOutbox(e);

  Serial.printf("EXPERIMENT STARTED (%s): %s, sample %s, food \"%s\", run %s\n", via, exp.c_str(), smp.c_str(),
                name.c_str(), rid(r).c_str());
  showLcdMsg("FOOD " + exp);
  updateLcd();
  return "";
}

String startExperiment(JsonObjectConst in, const char *via, JsonObjectConst cmd) {
  String err = startExperimentChecked(in, via, cmd);
  if (err.length()) Serial.printf("EXPERIMENT START REFUSED (%s): %s\n", via, err.c_str());
  beep(err.length() ? 2 : 1);
  return err;
}

String endExperimentChecked(String status, String notes, const char *via, JsonObjectConst cmd) {
  if (!timeSynced) return "Waiting for time sync";
  if (!nvsExp.length()) return "No experiment is running";
  status.trim();
  if (status != "completed" && status != "interrupted" && status != "abandoned")
    return "Final status must be completed, interrupted or abandoned";
  if (!cleanText(notes, 200)) return "Notes: at most 200 characters";
  uint64_t now = nowMs();
  String exp = nvsExp;
  RunInfo &r = curRun();

  JsonDocument m;
  m["m"] = "PATCH";
  m["p"] = "experiments/" + exp + "/meta";
  m["own"] = prefs.getULong64("expStart", 0);   // 0: started by 1.0.0 `e`, meta written by experiment_tool
  m["b"]["status"]    = status;
  m["b"]["endedAtMs"] = now;
  m["b"]["endedVia"]  = via;
  appendOutbox(m);

  JsonDocument e;
  e["m"] = "PUT";
  e["p"] = "experiments/" + exp + "/events/{RUN}-experiment-end";
  e["r"] = r.run;
  JsonObject b = e["b"].to<JsonObject>();
  b["experimentId"] = exp;
  b["runId"]        = "";
  b["sampleId"]     = nvsSample;
  b["type"]         = "other";
  b["observedAtMs"] = now;
  b["status"]       = status;
  int vi = viaIndex(via);
  String n = String("Experiment ended ") + VIA_NOTE[vi] + (vi == 1 ? String(cmd["commandId"] | "") : String()) +
             " (" + status + ")";
  if (notes.length()) n += ". " + notes;
  b["notes"] = n;
  if (vi == 1) {
    b["commandId"]  = cmd["commandId"];
    b["issuedAtMs"] = cmd["issuedAtMs"];
    b["issuedBy"]   = cmd["issuedBy"];
  }
  appendOutbox(e);

  nvsExp = ""; nvsSample = ""; nvsFood = "";
  prefs.putString("exp", "");
  prefs.putString("smp", "");
  prefs.putString("food", "");
  prefs.putULong64("expStart", 0);
  alertEnd();
  startRun("experiment_end");
  Serial.printf("EXPERIMENT ENDED (%s): %s, %s. Back to calibration, run %s\n", via, exp.c_str(), status.c_str(),
                rid(curRun()).c_str());
  showLcdMsg("EXP ENDED");
  updateLcd();
  return "";
}

String endExperiment(const String &status, const String &notes, const char *via, JsonObjectConst cmd) {
  String err = endExperimentChecked(status, notes, via, cmd);
  if (err.length()) Serial.printf("EXPERIMENT END REFUSED (%s): %s\n", via, err.c_str());
  beep(err.length() ? 2 : 1);
  return err;
}

// Observed / event time from the page: blank = now, otherwise WAT, not in the future.
bool pageTime(const String &field, uint64_t now, uint64_t &out, String &err) {
  if (!field.length()) { out = now; return true; }
  out = parseWat(field);
  if (!out || out > now + FUTURE_SLACK_MS) { err = "Time: enter a valid WAT date and time, not in the future"; return false; }
  return true;
}

// experiments/<experimentId>/observations/<sampleId>-obs-<epoch>. The page never suggests a label.
String addObservation(const String &label, const String &method, String methodNote, const String &at, String notes) {
  if (!timeSynced) return "Waiting for time sync";
  if (!nvsExp.length()) return "Start an experiment first";
  if (label != "unknown" && label != "fresh" && label != "spoiled" && label != "inconclusive")
    return "Label must be unknown, fresh, spoiled or inconclusive";
  if (method != "smell" && method != "visual" && method != "smell_and_visual" && method != "other")
    return "Method must be smell, visual, smell and visual, or other";
  if (!cleanText(methodNote, 60) || (method == "other" && !methodNote.length())) return "Other method: describe it (up to 60 characters)";
  if (!cleanText(notes, 200)) return "Notes: at most 200 characters";
  uint64_t now = nowMs(), obsMs;
  String err;
  if (!pageTime(at, now, obsMs, err)) return err;
  JsonDocument e;
  e["m"] = "PUT";
  e["p"] = "experiments/" + nvsExp + "/observations/" + nvsSample + "-obs-" + String((unsigned long)keyEpoch());
  JsonObject b = e["b"].to<JsonObject>();
  b["experimentId"] = nvsExp;
  b["sampleId"]     = nvsSample;
  b["observedAtMs"] = obsMs;
  b["label"]        = label;
  b["method"]       = method;
  if (method == "other") b["methodDetail"] = methodNote;
  if (notes.length()) b["notes"] = notes;
  b["recordedAtMs"] = now;
  b["source"]       = "setup_page";
  if (!appendOutbox(e)) return "Could not save on the board (flash full?)";
  Serial.printf("OBSERVATION saved: %s by %s at %s WAT\n", label.c_str(), method.c_str(), fmtWat(obsMs).c_str());
  return "";
}

// experiments/<experimentId>/events/<runId>-<type>-<epoch>.
String logEvent(const String &type, const String &detail, const String &at, const String &closedAt, String notes) {
  if (!timeSynced) return "Waiting for time sync";
  if (!nvsExp.length()) return "Start an experiment first";
  if (!cleanText(notes, 200)) return "Notes: at most 200 characters";
  uint64_t now = nowMs(), atMs, closedMs = 0;
  String err;
  if (!pageTime(at, now, atMs, err)) return err;
  JsonDocument e;
  JsonObject b = e["b"].to<JsonObject>();
  if (type == "storage_change") {
    if (detail != "food_in" && detail != "food_out" && detail != "moved") return "Storage change: food in, food out or moved";
    b["change"] = detail;
  } else if (type == "door_opened") {
    if (closedAt.length()) {
      if (!pageTime(closedAt, now, closedMs, err)) return "Door closed at: " + err;
      if (closedMs < atMs) return "Door closed before it was opened";
      b["doorClosedAtMs"] = closedMs;
    }
  } else if (type == "power_outage") {
    if (detail != "sensor_power" && detail != "refrigeration_power" && detail != "both" && detail != "unknown")
      return "Power cut: sensor power, refrigeration power, both or unknown";
    b["powerScope"] = detail;
  } else if (type != "sample_replaced" && type != "other") {
    return "Unknown event type";
  }
  e["m"] = "PUT";
  e["p"] = "experiments/" + nvsExp + "/events/{RUN}-" + type + "-" + String((unsigned long)keyEpoch());
  e["r"] = curRun().run;
  b["experimentId"] = nvsExp;
  b["runId"]        = "";
  b["type"]         = type;
  b["observedAtMs"] = atMs;
  if (notes.length()) b["notes"] = notes;
  b["recordedAtMs"] = now;
  b["source"]       = "setup_page";
  if (!appendOutbox(e)) return "Could not save on the board (flash full?)";
  Serial.printf("EVENT saved: %s at %s WAT\n", type.c_str(), fmtWat(atMs).c_str());
  return "";
}

// =====================================================================
// Commands through Firebase (control/FRIDGE001/command -> ack/<commandId>)
// =====================================================================
// NVS "cmdIds": the last CMD_HISTORY commandIds (upper case), one per line.
// NVS "cmdLast": {"key","ack":{...},"acked","cleared"} for the last command handled. A command
// is acknowledged before the command node is cleared, and the ack keeps a full copy of it.

bool cmdHandled(const String &idUpper) {
  String list = "\n" + prefs.getString("cmdIds", "") + "\n";
  return list.indexOf("\n" + idUpper + "\n") >= 0;
}

void cmdRemember(const String &idUpper) {
  String list = prefs.getString("cmdIds", "");
  list = list.length() ? list + "\n" + idUpper : idUpper;
  int lines = 1;
  for (size_t i = 0; i < list.length(); i++) if (list[i] == '\n') lines++;
  while (lines-- > CMD_HISTORY) list.remove(0, list.indexOf('\n') + 1);
  prefs.putString("cmdIds", list);
}

// Acknowledges (if not done yet) and then clears the command node, only if it still holds
// the command that was read (ETag). false = try again at the next poll.
bool finishCommand(JsonDocument &last, const String &etag) {
  String key = last["key"] | "";
  if (!(last["acked"] | false)) {
    String body;
    serializeJson(last["ack"], body);
    int code = fbSend("PUT", String(CONTROL_PATH) + "/ack/" + key, body);
    serviceSampling();
    if (code != 200) return false;
    last["acked"] = true;
    String s; serializeJson(last, s); prefs.putString("cmdLast", s);
  }
  if (!(last["cleared"] | false)) {
    int code = fbRequest("PUT", String(CONTROL_PATH) + "/command", "\"\"", nullptr, false, &etag);
    serviceSampling();
    if (code == 412) { Serial.println(F("  COMMAND: node changed after it was read - not cleared, read again next time")); return true; }
    if (code != 200) return false;
    last["cleared"] = true;
    String s; serializeJson(last, s); prefs.putString("cmdLast", s);
  }
  return true;
}

void pollCommand() {
  nextCommandMs = millis() + COMMAND_PERIOD_MS;
  if (!ensureToken()) return;
  String body, etag;
  int code = fbRequest("GET", String(CONTROL_PATH) + "/command", "", &body, false, nullptr, &etag);
  serviceSampling();
  if (code != 200) return;
  body.trim();
  if (body == "null" || body == "\"\"") return;          // nothing to do
  JsonDocument c;
  static String warned;
  if (deserializeJson(c, body) || !c.is<JsonObject>()) {
    if (warned != body) Serial.println(F("  COMMAND: node holds something that is not a command object - left untouched"));
    warned = body;
    return;
  }
  String rawId = c["commandId"] | "";
  String idU = normId(rawId);
  JsonDocument last;
  deserializeJson(last, prefs.getString("cmdLast", "{}"));
  if (idU.length() && rawId == (const char *)(last["key"] | "") &&!((last["acked"] | false) && (last["cleared"] | false))) {
    finishCommand(last, etag);                            // ack or clear did not complete last time
    return;
  }
  if (idU.length() && cmdHandled(idU)) {
    if (warned != idU) Serial.printf("  COMMAND %s: already handled - ignored\n", rawId.c_str());
    warned = idU;
    return;
  }
  String type = c["type"] | "";
  String key = idU.length() ? rawId : "INVALID-" + String((unsigned long)(nowMs() / 1000));
  String err;
  String endedExp = nvsExp;
  if (!idU.length()) err = "commandId must be 3 to 32 characters of A-Z, 0-9 and -";
  else if (!c["payload"].is<JsonObject>()) err = "payload must be an object";
  else if (type == "start_experiment") err = startExperiment(c["payload"].as<JsonObjectConst>(), "firebase_command", c.as<JsonObjectConst>());
  else if (type == "end_experiment")
    err = endExperiment(c["payload"]["status"] | "", c["payload"]["notes"] | "", "firebase_command", c.as<JsonObjectConst>());
  else err = "type must be start_experiment or end_experiment";
  if (idU.length()) cmdRemember(idU);

  int vcode;
  verifyRun(curRun(), vcode);                           // the ack names the runId: check it first if possible
  last.clear();
  last["key"] = key;
  JsonObject a = last["ack"].to<JsonObject>();
  a["commandId"]   = rawId;
  a["type"]        = type;
  a["status"]      = err.length() ? "rejected" : "applied";
  if (err.length()) a["reason"] = err;
  a["appliedAtMs"] = nowMs();
  a["experimentId"] = type == "end_experiment" && !err.length() ? endedExp : (nvsExp.length() ? nvsExp : curRun().expId);
  a["runId"]       = rid(curRun());
  a["command"]     = c;                                 // full copy: clearing the node loses nothing
  last["acked"] = false;
  last["cleared"] = false;
  String s; serializeJson(last, s); prefs.putString("cmdLast", s);
  Serial.printf("COMMAND %s (%s): %s%s\n", rawId.c_str(), type.c_str(), err.length() ? "rejected - " : "applied",
                err.c_str());
  finishCommand(last, etag);
}

void syncHistory(const char *reason) {
  historyRequested = false;
  uploading = true;
  updateLcd();
  Serial.printf("UPLOAD (%s): %lu unsent, %lu experiment records\n", reason, (unsigned long)totalUnsent(),
                (unsigned long)outboxPending);
  int sent = 0, code = 0;
  bool ok = false;
  const char *stage = "auth";
  if (!ensureToken()) {
    code = lastAuthCode;
  } else {
    ok = true;
    for (size_t i = 0; i < runs.size() && ok; i++) ok = uploadRun(runs[i], sent, code);
    stage = ok ? "experiment records" : "samples";
    if (ok) ok = processOutbox(code, sent);
    if (ok) stage = "done";
    if (ok && !curRun().bootEpochMs) Serial.println(F("  Clock not set yet: this run's records wait in flash"));
  }
  lastUploadOk = ok;
  char buf[48];
  if (ok) snprintf(buf, sizeof(buf), "OK %lum (%d rec)", (unsigned long)(millis() / 60000), sent);
  else    snprintf(buf, sizeof(buf), "FAIL %s %d%s", stage, code, writeDenied ? " (no write rule yet?)" : "");
  lastHistoryResult = buf;
  Serial.printf("UPLOAD %s: %d records sent, %lu unsent\n", ok ? "OK" : "FAILED", sent, (unsigned long)totalUnsent());
  uploading = false;
  updateLcd();
  pruneFs();
}

// WiFi stays connected; latest goes up right after each record, history every 10 min.
void serviceNetwork() {
  if (!wifiConfigured) return;
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wasConnected) { Serial.println(F("  WiFi: connection lost")); wasConnected = false; }
    if (!forceWifi && lastWifiTryMs && now - lastWifiTryMs < WIFI_RETRY_MS) return;
    lastWifiTryMs = now | 1;
    bool manual = forceWifi;
    forceWifi = false;
    if (!wifiConnect()) {
      if (manual || historyRequested) {
        Serial.printf("UPLOAD (%s) FAILED: no WiFi within %lu s\n", historyReason, (unsigned long)(WIFI_TIMEOUT_MS / 1000));
        lastHistoryResult = "FAIL wifi";
        historyRequested = false;
      }
      if (latestPending) { latestPending = false; lastLatestResult = "FAIL wifi"; }
      if (uploadsEnabled) lastUploadOk = false;
      return;
    }
  }
  if (!wasConnected) {
    Serial.printf("  WiFi: connected to \"%s\" (%s), RSSI %d dBm\n", curWifiSsid.c_str(), curWifiSource.c_str(), WiFi.RSSI());
    wasConnected = true;
    startSntp();
    noteWifiConnection();
    if (uploadsEnabled && !historyRequested) { historyRequested = true; historyReason = "wifi connected"; }
  }
  checkTimeSync();
  if (!uploadsEnabled) { latestPending = false; historyRequested = false; return; }
  if (latestPending) putLatest();
  // Commands only once this boot has a real time, so none is refused just for an early read.
  if (timeSynced && (int32_t)(millis() - nextCommandMs) >= 0) pollCommand();
  if (historyRequested) syncHistory(historyReason);
}

// =====================================================================
// Wi-Fi setup portal (latching button on GPIO 4)
// =====================================================================

// The page (setup_page.html, gzipped into setup_page.h) is served from flash; no external files.
// SSIDs and every user-entered value are inserted with textContent, never as HTML.

void onStaDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  lastDiscReason = info.wifi_sta_disconnected.reason;
}

const char *joinFailReason(uint8_t r) {
  switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
      return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "not found";
    default:
      return "timeout";
  }
}

// Name of the first secrets.h setting that stops uploads, or nullptr when uploads can run.
const char *missingSetting() {
  if (!wifiConfigured)                              return "WIFI_SSID / WIFI_PASSWORD";
  if (looksPlaceholder(FIREBASE_API_KEY))           return "FIREBASE_API_KEY";
  if (looksPlaceholder(FIREBASE_DATABASE_URL))      return "FIREBASE_DATABASE_URL";
  if (looksPlaceholder(DEVICE_EMAIL) || String(DEVICE_EMAIL).indexOf('@') < 0) return "DEVICE_EMAIL";
  if (looksPlaceholder(DEVICE_PASSWORD))            return "DEVICE_PASSWORD";
  return nullptr;
}

// After the saved network changes: a board without a secrets.h network can start uploading.
void refreshWifiConfig() {
  wifiConfigured = secretsWifiOk || savedSsid.length() > 0;
  bool was = uploadsEnabled;
  uploadsEnabled = missingSetting() == nullptr;
  if (uploadsEnabled && !was) {
    Serial.println(F("  Uploads enabled with the saved network"));
    historyRequested = true; historyReason = "wifi saved";
  } else if (!uploadsEnabled && was) {
    Serial.printf("  !!! UPLOADS DISABLED: %s missing. Logging to flash only.\n", missingSetting());
  }
}

void forgetSaved() {
  Preferences wp;
  if (wp.begin("wifi", false)) { wp.clear(); wp.end(); }
  bool had = savedSsid.length() > 0;
  savedSsid = ""; savedPass = "";
  if (curWifiSource == "saved") {
    if (saveState != SAVE_TESTING) WiFi.disconnect();    // rejoin via secrets.h
    curWifiSource = ""; curWifiSsid = "";
    lastWifiTryMs = 0;
  }
  if (saveState == SAVE_OK) saveState = SAVE_IDLE;
  refreshWifiConfig();
  Serial.println(had ? F("  WiFi: saved network forgotten - using secrets.h") : F("  WiFi: no saved network"));
}

// Debounced button. Returns true while a new position is waiting for serviceButton().
bool pollButton() {
  bool raw = digitalRead(PIN_WIFI_BTN) == LOW;     // LOW = latched in
  uint32_t now = millis();
  if (raw != btnRaw) { btnRaw = raw; btnRawSinceMs = now; }
  else if (raw != btnLatched && now - btnRawSinceMs >= BTN_DEBOUNCE_MS) { btnLatched = raw; btnEvent = true; }
  return btnEvent;
}

void openPortal() {
  if (portalOpen) return;
  WiFi.mode(WIFI_AP_STA);                  // AP for the phone, STA to scan and test networks
  if (!WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD)) {
    Serial.println(F("WIFI SETUP: could not start the setup network"));
    WiFi.mode(wifiConfigured ? WIFI_STA : WIFI_OFF);
    return;
  }
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());     // every name resolves to the board: captive portal
  web.begin();
  uint32_t now = millis();
  portalOpen = true;
  portalOpenMs = now;
  portalCloseAtMs = now + PORTAL_OPEN_MS;
  ledToggleMs = now;
  saveState = SAVE_IDLE;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);                 // first list is ready when the page opens
  Serial.printf("WIFI SETUP: open - join \"%s\" (password on the LCD), then open http://%s\n",
                SETUP_AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.println(F("  Uploads paused; sampling and logging continue."));
  updateLcd();
  beep(1);
}

void closePortal(const char *reason) {
  if (!portalOpen) return;
  web.stop();
  dns.stop();
  WiFi.scanDelete();
  if (saveState == SAVE_TESTING) WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(wifiConfigured ? WIFI_STA : WIFI_OFF);
  portalOpen = false;
  saveState = SAVE_IDLE;
  savePass = "";
  digitalWrite(PIN_LED, LOW);
  wasConnected = false;                    // serviceNetwork() reports and records the connection
  lastWifiTryMs = 0;                       // reconnect at once if not connected
  if (uploadsEnabled) { historyRequested = true; historyReason = "after wifi setup"; }
  Serial.printf("WIFI SETUP: closed (%s). Uploads resume.\n", reason);
  updateLcd();
}

void serviceButton() {
  pollButton();
  if (!btnEvent) return;
  btnEvent = false;
  if (btnLatched) {                        // released -> latched: fresh press
    if (!apPwOk) {
      btnPwError = true;
      Serial.println(F("WIFI SETUP: AP PW 8-13 CHARS - SETUP_AP_PASSWORD in secrets.h must be 8 to 13 characters. Setup not opened."));
    } else {
      openPortal();
    }
  } else {                                 // latched -> released
    btnPwError = false;
    if (portalOpen) closePortal("button released");
    else Serial.println(F("WIFI SETUP: button released"));
  }
  updateLcd();
}

void startSaveTest(const String &ssid, const String &pass) {
  saveSsid = ssid; savePass = pass; saveReason = "";
  saveState = SAVE_TESTING;
  saveStartMs = millis();
  WiFi.disconnect();
  lastDiscReason = 0;
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  Serial.printf("WIFI SETUP: testing \"%s\" (up to %lu s)\n", ssid.c_str(), (unsigned long)(SAVE_TEST_MS / 1000));
  updateLcd();
}

// Non-blocking: the page polls /status while this runs.
void serviceSaveTest() {
  if (saveState != SAVE_TESTING) return;
  uint32_t now = millis();
  wl_status_t st = WiFi.status();
  bool timedOut = now - saveStartMs >= SAVE_TEST_MS;
  bool failed = st == WL_CONNECT_FAILED && now - saveStartMs > 1000;
  if (st == WL_CONNECTED) {
    Preferences wp;
    bool stored = wp.begin("wifi", false) && wp.putString("ssid", saveSsid) > 0;
    if (stored) {
      wp.putString("pass", savePass);
      // An empty password reads back as "" either way; check a non-empty one really landed.
      stored = !savePass.length() || wp.getString("pass", "") == savePass;
    }
    wp.end();
    saveDoneMs = now;
    if (!stored) {
      saveState = SAVE_FAIL; saveReason = "not stored";
      Serial.println(F("WIFI SETUP: connected, but writing to NVS failed - nothing saved"));
      WiFi.disconnect();
    } else {
      saveState = SAVE_OK;
      saveRssi = WiFi.RSSI();
      savedSsid = saveSsid; savedPass = savePass;
      curWifiSource = "saved"; curWifiSsid = saveSsid;
      if ((int32_t)(portalCloseAtMs - (now + PORTAL_SAVED_MS)) > 0) portalCloseAtMs = now + PORTAL_SAVED_MS;
      refreshWifiConfig();
      RunInfo &r = curRun();
      if (r.foodTest) {
        wifiEvt.pending = true; wifiEvt.expId = r.expId; wifiEvt.run = r.run;
        wifiEvt.ssid = saveSsid; wifiEvt.atMillis = now;
        flushWifiEvent();
      }
      Serial.printf("WIFI SETUP: saved \"%s\", RSSI %d dBm. Setup closes in %lu s.\n",
                    saveSsid.c_str(), saveRssi, (unsigned long)(PORTAL_SAVED_MS / 1000));
      updateLcd();
      beep(2);
    }
    savePass = "";
  } else if (failed || timedOut) {
    saveState = SAVE_FAIL;
    saveDoneMs = now;
    saveReason = joinFailReason(lastDiscReason);
    savePass = "";
    WiFi.disconnect();                     // stop retrying with credentials that failed
    Serial.printf("WIFI SETUP: \"%s\" failed: %s - nothing saved\n", saveSsid.c_str(), saveReason.c_str());
  }
}

void servicePortal() {
  dns.processNextRequest();
  web.handleClient();
  serviceSaveTest();
  if (WiFi.status() == WL_CONNECTED) checkTimeSync();
  uint32_t now = millis();
  if (now - ledToggleMs >= PORTAL_LED_MS) {
    static bool ledOn = false;
    ledOn = !ledOn;
    digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
    ledToggleMs = now;
  }
  if (saveState != SAVE_TESTING && (int32_t)(now - portalCloseAtMs) >= 0)
    closePortal(saveState == SAVE_OK ? "saved" : "10 min timeout");
}

// ---- web handlers ----

void sendJson(JsonDocument &d) {
  String out;
  serializeJson(d, out);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", out);
}

void sendError(const char *text) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = text;
  sendJson(d);
}

void handleRedirect() {
  web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  web.send(302, "text/plain", "");
}

void handleRoot() {
  if (web.hostHeader() != WiFi.softAPIP().toString()) { handleRedirect(); return; }
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("Content-Encoding", "gzip");
  web.send_P(200, "text/html; charset=utf-8", (const char *)SETUP_PAGE_GZ, SETUP_PAGE_GZ_LEN);
}

// ---- experiment section of the page ----

// The page's actions only work while the portal is open (button + setup password).
void sendResult(const String &err, const String &okMsg) {
  JsonDocument d;
  d["ok"]  = !err.length();
  d["msg"] = err.length() ? err : okMsg;
  sendJson(d);
}

String savedNote() {
  return WiFi.status() == WL_CONNECTED ? "Saved on the board. It uploads when this setup page closes."
                                       : "Saved on the board, will upload when online.";
}

void handleExpState() {
  RunInfo &r = curRun();
  JsonDocument d;
  d["mode"]  = r.foodTest ? "food_test" : "calibration";
  d["exp"]   = r.expId;
  d["smp"]   = r.foodTest ? r.sampleId : String();
  d["food"]  = nvsFood;
  d["run"]   = rid(r) + (r.idOk ? "" : " (not checked yet)");
  int q = std::max(lastRank1, lastRank2);
  d["q"]     = q < 0 ? "no record yet" : QUALITY_NAMES[q];
  uint32_t up = millis() / 1000;
  d["settle"] = up < SETTLE_S ? SETTLE_S - up : 0;
  d["sync"]  = timeSynced;
  d["t"]     = nowMs();
  d["online"] = WiFi.status() == WL_CONNECTED;
  uint32_t day = watDay();
  if (day) { d["day"] = String(day); d["nn"] = nextDayNumber(); }
  d["uns"] = totalUnsent();
  d["ob"]  = outboxPending;
  d["rej"] = rejectedCount;
  // 1.2.0 spoilage alert
  d["al"] = ALERT_NAMES[r.foodTest ? al.state : AL_OFF];
  if (r.foodTest) {
    if (al.scored) d["sc"] = rnd(al.score, 1);
    d["band"] = BAND_NAMES[al.band];
    d["sil"]  = al.silenced;
    d["test"] = al.test;
    for (int c = 0; c < 2; c++) if (al.ch[c].has) d[c ? "r2" : "r1"] = rnd(al.ch[c].ratio, 3);
    d["bl"] = baselineText();
    d["fr"] = foodRefText();
    if (!bl.any) d["blWhy"] = NO_BASELINE_MSG;
  } else {                                // would a Start now get a baseline?
    Baseline now;
    baselineFromBuffer(now);
    d["bwn1"] = now.n[0]; d["bwn2"] = now.n[1]; d["bwNeed"] = BASELINE_MIN_RECORDS; d["bwOk"] = now.any;
    d["bwReady"] = calReady; d["bwFull"] = BASELINE_WINDOW_RECORDS;
  }
  JsonDocument last;
  if (!deserializeJson(last, prefs.getString("cmdLast", "{}")) && last["ack"].is<JsonObject>()) {
    d["cmd"]["id"]     = last["ack"]["commandId"];
    d["cmd"]["type"]   = last["ack"]["type"];
    d["cmd"]["status"] = last["ack"]["status"];
    d["cmd"]["at"]     = last["ack"]["appliedAtMs"];
    d["cmd"]["reason"] = last["ack"]["reason"];
  }
  sendJson(d);
}

// Optional WAT time field -> ms in the payload; false (and err) if filled in but not a time.
bool formTime(const char *field, JsonVariant dst, const char *label, String &err) {
  String v = web.arg(field);
  v.trim();
  if (!v.length()) return true;
  uint64_t t = parseWat(v);
  if (!t) { err = String(label) + ": not a valid date and time"; return false; }
  dst.set(t);
  return true;
}

void handleExpStart() {
  JsonDocument in;
  String err;
  in["experimentId"] = web.arg("exp");
  in["sampleId"]     = web.arg("smp");
  JsonObject food = in["food"].to<JsonObject>();
  food["name"]         = web.arg("food");
  food["recipe"]       = web.arg("recipe");
  food["priorStorage"] = web.arg("prior");
  if (web.arg("pamt").length()) {
    food["portion"]["amount"] = web.arg("pamt").toFloat();
    food["portion"]["unit"]   = web.arg("punit");
  }
  JsonObject sto = in["storage"].to<JsonObject>();
  sto["location"]  = web.arg("loc");
  sto["container"] = web.arg("cont");
  sto["cover"]     = web.arg("cover");
  sto["otherFood"] = web.arg("others");
  in["notes"] = web.arg("notes");
  if (!formTime("cooked", food["cookedAtMs"], "Cooked at", err) || !formTime("stored", in["storedAtMs"], "Stored at", err)) {
    beep(2);
    sendResult(err, "");
    return;
  }
  err = startExperiment(in.as<JsonObjectConst>(), "setup_page", JsonObjectConst());
  sendResult(err, "Started " + nvsExp + ", sample " + nvsSample + ", run " + rid(curRun()) + ".\n" +
                  (bl.any ? "Baseline: " + baselineText() + ".\n"
                          : String(NO_BASELINE_MSG) + ". This experiment has no spoilage alert (no_baseline).\n") +
                  "Log the food going in with Log event, Storage change.\n" + savedNote());
}

void handleSilence() {
  String err = silenceAlarm();
  sendResult(err, "Alarm silenced for this Critical episode. The LCD and the uploaded verdict stay Critical.");
}

void handleExpObs() {
  String err = addObservation(web.arg("label"), web.arg("method"), web.arg("mnote"), web.arg("at"), web.arg("notes"));
  beep(err.length() ? 2 : 1);
  sendResult(err, "Observation saved. " + savedNote());
}

void handleExpEvent() {
  String type = web.arg("type");
  String detail = type == "storage_change" ? web.arg("sc") : type == "power_outage" ? web.arg("ps") : String();
  String err = logEvent(type, detail, web.arg("at"), type == "door_opened" ? web.arg("closed") : String(), web.arg("notes"));
  beep(err.length() ? 2 : 1);
  String ok = "Event saved. " + savedNote();
  if (!err.length() && type == "sample_replaced")
    ok += "\nA new sample needs a NEW experiment: end this one, then start a new one.";
  sendResult(err, ok);
}

void handleExpEnd() {
  String exp = nvsExp;
  String err = endExperiment(web.arg("status"), web.arg("notes"), "setup_page", JsonObjectConst());
  sendResult(err, "Experiment " + exp + " ended. Back to calibration mode, run " + rid(curRun()) + ".\n" + savedNote());
}

void handleScan() {
  int n = WiFi.scanComplete();
  bool canScan = saveState != SAVE_TESTING && n != WIFI_SCAN_RUNNING;
  if (canScan && (web.hasArg("refresh") || n == WIFI_SCAN_FAILED)) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    n = WIFI_SCAN_RUNNING;
  }
  JsonDocument d;
  if (n < 0) { d["busy"] = true; sendJson(d); return; }
  std::vector<int> idx;
  for (int i = 0; i < n; i++) if (WiFi.SSID(i).length()) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
  JsonArray a = d["networks"].to<JsonArray>();
  std::vector<String> seen;
  for (int i : idx) {
    String s = WiFi.SSID(i);
    if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;   // strongest of each name
    seen.push_back(s);
    JsonObject o = a.add<JsonObject>();
    o["ssid"]   = s;
    o["rssi"]   = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    if (a.size() >= 25) break;
  }
  sendJson(d);
}

void handleStatus() {
  static const char *names[] = {"idle", "testing", "ok", "fail"};
  JsonDocument d;
  d["saved"] = savedSsid;                   // name only, never the password
  d["state"] = names[saveState];
  d["ssid"]  = saveSsid;
  d["rssi"]  = saveRssi;
  d["reason"] = saveReason;
  d["left"]  = portalLeftS();
  sendJson(d);
}

void handleSave() {
  if (saveState == SAVE_TESTING) { sendError("A test is already running."); return; }
  String ssid = web.arg("ssid"), pass = web.arg("pass");
  if (ssid.length() < 1 || ssid.length() > 32) { sendError("Network name must be 1 to 32 characters."); return; }
  if (pass.length() > 63) { sendError("Password must be at most 63 characters."); return; }
  if (pass.length() && pass.length() < 8) { sendError("Wi-Fi passwords have at least 8 characters."); return; }
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) { sendError("Still scanning. Try again in a few seconds."); return; }
  startSaveTest(ssid, pass);
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
}

void handleForget() {
  forgetSaved();
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
}

void setupWebRoutes() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/scan", HTTP_GET, handleScan);
  web.on("/status", HTTP_GET, handleStatus);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/forget", HTTP_POST, handleForget);
  web.on("/exp", HTTP_GET, handleExpState);
  web.on("/exp/start", HTTP_POST, handleExpStart);
  web.on("/exp/obs", HTTP_POST, handleExpObs);
  web.on("/exp/event", HTTP_POST, handleExpEvent);
  web.on("/exp/end", HTTP_POST, handleExpEnd);
  web.on("/exp/silence", HTTP_POST, handleSilence);
  // Connectivity checks (Android, iOS/macOS, Windows): a redirect makes the phone open the page.
  for (const char *p : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                        "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink", "/canonical.html", "/success.txt"})
    web.on(p, handleRedirect);
  web.onNotFound(handleRedirect);
}

void printWifiStatus() {
  bool conn = WiFi.status() == WL_CONNECTED;
  Serial.println(F("---------------- WIFI ------------------"));
  if (conn) {
    Serial.printf("  connected        : \"%s\" (source %s), RSSI %d dBm, IP %s\n", WiFi.SSID().c_str(),
                  curWifiSource.length() ? curWifiSource.c_str() : "?", WiFi.RSSI(), WiFi.localIP().toString().c_str());
  } else {
    Serial.println(F("  connected        : no"));
  }
  Serial.printf("  saved network    : %s\n", savedSsid.length() ? ("\"" + savedSsid + "\"").c_str() : "none");
  Serial.printf("  secrets.h network: %s\n", secretsWifiOk ? "\"" WIFI_SSID "\"" : "not set");
  if (portalOpen) {
    uint32_t left = portalLeftS();
    Serial.printf("  setup portal     : OPEN, \"%s\" at %s, %d phone(s) joined, closes in %lu:%02lu\n",
                  SETUP_AP_SSID, WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(),
                  (unsigned long)(left / 60), (unsigned long)(left % 60));
  } else {
    Serial.printf("  setup portal     : closed%s\n", !apPwOk ? " (AP PW 8-13 CHARS: cannot open)" : "");
  }
  Serial.printf("  setup button     : %s\n", btnLatched ? "latched" : "released");
  Serial.println(F("----------------------------------------"));
}

// =====================================================================
// Serial commands
// =====================================================================

void printStatus() {
  RunInfo &r = curRun();
  uint32_t up = millis() / 1000;
  Serial.println(F("---------------- STATUS ----------------"));
  Serial.printf("  run              : %s (boot %lu, %s)%s\n", rid(r).c_str(), (unsigned long)r.boot, r.startReason.c_str(),
                r.idOk ? "" : " - runId not checked in Firebase yet");
  Serial.printf("  mode             : %s, experimentId %s, sampleId %s\n", r.foodTest ? "food_test" : "calibration",
                r.expId.c_str(), r.sampleId.c_str());
  Serial.printf("  uptime           : %lu min (%s)\n", (unsigned long)(up / 60),
                up < SETTLE_S ? "settling" : "settling period over");
  if (lastRank1 >= 0) Serial.printf("  last quality     : mq1 %s, mq2 %s\n", QUALITY_NAMES[lastRank1], QUALITY_NAMES[lastRank2]);
  Serial.printf("  clock            : %s\n", timeSynced ? "set (NTP)" : "NOT set - records wait in flash");
  Serial.printf("  records this run : %lu logged, %lu unsent\n", (unsigned long)r.logged, (unsigned long)r.unsent);
  Serial.printf("  unsent (all)     : %lu\n", (unsigned long)totalUnsent());
  for (size_t i = 0; i + 1 < runs.size(); i++)
    Serial.printf("    earlier run %s (%s): %lu logged, %lu unsent%s\n", rid(runs[i]).c_str(),
                  runs[i].expId.c_str(), (unsigned long)runs[i].logged, (unsigned long)runs[i].unsent,
                  runs[i].bootEpochMs ? "" : ", clock never set (moves to rejected at next boot)");
  Serial.printf("  experiment recs  : %lu waiting in %s\n", (unsigned long)outboxPending, OUTBOX_PATH);
  Serial.printf("  rejected         : %lu records in %s (r = dump)\n", (unsigned long)rejectedCount, REJECTED_PATH);
  {
    JsonDocument last;
    if (!deserializeJson(last, prefs.getString("cmdLast", "{}")) && last["ack"].is<JsonObject>())
      Serial.printf("  last command     : %s %s %s%s\n", (const char *)(last["ack"]["commandId"] | ""),
                    (const char *)(last["ack"]["type"] | ""), (const char *)(last["ack"]["status"] | ""),
                    (last["acked"] | false) ? "" : " (ack not written yet)");
  }
  if (!uploadsEnabled) Serial.println(F("  uploads          : DISABLED (see boot messages)"));
  else {
    Serial.printf("  WiFi             : %s", WiFi.status() == WL_CONNECTED ? "connected" : "not connected");
    if (WiFi.status() == WL_CONNECTED) Serial.printf(", RSSI %d dBm", WiFi.RSSI());
    Serial.println();
    Serial.printf("  auth             : %s (last code %d)\n", tokenValid ? "signed in" : "not signed in", lastAuthCode);
    if (writeDenied) Serial.println(F("  writes           : DENIED by database rules - app team must add the producer UID"));
    if (readDenied)  Serial.println(F("  reads            : last read DENIED by database rules (see the HTTP 401 line) - app team must allow it"));
    Serial.printf("  last latest PUT  : %s\n", lastLatestResult.c_str());
    Serial.printf("  last history     : %s\n", lastHistoryResult.c_str());
    Serial.printf("  next history in  : %ld min\n", (long)((int32_t)(nextHistoryMs - millis()) / 60000));
  }
  if (r.foodTest) {
    Serial.printf("  spoilage alert   : %s%s, band %s%s%s\n", ALERT_NAMES[al.state],
                  al.scored ? (", score " + String(al.score, 1) + "%").c_str() : "", BAND_NAMES[al.band],
                  al.silenced ? ", silenced" : "", al.test ? " (TEST record)" : "");
    for (int c = 0; c < 2; c++)
      Serial.printf("    %s ratio       : %s%s%s\n", CH_NAMES[c], al.ch[c].has ? String(al.ch[c].ratio, 3).c_str() : "-",
                    al.ch[c].used ? (" (" + String(al.ch[c].pct, 1) + "%, used)").c_str()
                                  : (String(" (not used: ") + (al.ch[c].reason ? al.ch[c].reason : "no record yet") + ")").c_str(),
                    al.ch[c].saturated ? " SATURATED" : "");
    Serial.printf("  baseline         : %s\n", baselineText().c_str());
    Serial.printf("  food reference   : %s\n", foodRefText().c_str());
    if (alertTestLeft) Serial.printf("  !! TEST          : %d more record(s) forced to %.1f%%\n", alertTestLeft, alertTestPct);
  } else {
    Baseline now;
    baselineFromBuffer(now);
    Serial.printf("  spoilage alert   : off (calibration). Baseline if started now: mq1 %u, mq2 %u valid records (%d needed, %d for CAL READY) - %s\n",
                  now.n[0], now.n[1], BASELINE_MIN_RECORDS, BASELINE_WINDOW_RECORDS,
                  calReady ? "CAL READY: put the food in, then Start" : now.any ? "usable, not full: keep the food out" : "NOT ready: keep the food out");
  }
  if (fsOk) Serial.printf("  LittleFS         : %u of %u bytes used, %u free\n", (unsigned)LittleFS.usedBytes(),
                          (unsigned)LittleFS.totalBytes(), (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
  else      Serial.println(F("  LittleFS         : NOT MOUNTED"));
  if (baselineFiles) Serial.printf("  baseline_logger  : %lu old file(s), %lu bytes, untouched\n",
                                   (unsigned long)baselineFiles, (unsigned long)baselineBytes);
  Serial.printf("  free heap        : %u\n", (unsigned)ESP.getFreeHeap());
  Serial.println(F("----------------------------------------"));
}

void printIds() {
  RunInfo &r = curRun();
  Serial.println(F("---------------- IDS -------------------"));
  Serial.printf("  experimentId : %s%s\n", r.expId.c_str(), r.foodTest ? "" : " (calibration mode)");
  Serial.printf("  sampleId     : %s\n", r.sampleId.c_str());
  if (r.foodTest) Serial.printf("  food         : %s\n", nvsFood.c_str());
  if (r.foodTest) Serial.printf("  baseline     : %s\n", baselineText().c_str());
  if (r.foodTest) Serial.printf("  food ref     : %s\n", foodRefText().c_str());
  Serial.printf("  runId        : %s%s (bootCount %lu)\n", rid(r).c_str(), r.idOk ? "" : " (not checked yet)", (unsigned long)bootCount);
  Serial.printf("  Producer UID : %s\n", producerUid.length() ? producerUid.c_str() : "(not signed in yet)");
  Serial.println(F("----------------------------------------"));
}

// Prints a LittleFS file line by line, 10 lines at a time, closed in between so sampling carries on.
uint32_t dumpFile(const String &path) {
  uint32_t lines = 0;
  size_t offset = 0;
  bool more = true;
  while (more) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) break;
    f.seek(offset);
    for (int k = 0; k < 10 && (more = f.available()); k++) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() > 1) { Serial.println(line); lines++; }
    }
    offset = f.position();
    f.close();
    serviceSampling();
  }
  return lines;
}

void dumpRejected() {
  quietSerial = true;
  Serial.println(F("===== BEGIN REJECTED DUMP ====="));
  uint32_t n = fsOk ? dumpFile(REJECTED_PATH) : 0;
  Serial.printf("===== END REJECTED DUMP (%lu lines) =====\n", (unsigned long)n);
  quietSerial = false;
}

// Prints every run as {"meta":{...}} followed by its records, then the outbox and the
// rejected file, between BEGIN/END markers.
void dumpLogs() {
  if (!fsOk) { Serial.println(F("LittleFS not mounted - nothing to dump")); return; }
  quietSerial = true;
  uint32_t lines = 0;
  Serial.println(F("===== BEGIN FRIDGE DUMP ====="));
  for (size_t i = 0; i < runs.size(); i++) {
    {
      JsonDocument m;
      runMetaJson(runs[i], m.to<JsonObject>());
      Serial.print(F("{\"meta\":"));
      serializeJson(m, Serial);
      Serial.println('}');
    }
    std::vector<uint32_t> segs = runs[i].segs;
    for (uint32_t seg : segs) lines += dumpFile(segPath(runs[i].run, seg));
  }
  Serial.printf("===== OUTBOX (%s, %lu not uploaded) =====\n", OUTBOX_PATH, (unsigned long)outboxPending);
  dumpFile(OUTBOX_PATH);
  Serial.printf("===== REJECTED (%s) =====\n", REJECTED_PATH);
  dumpFile(REJECTED_PATH);
  Serial.printf("===== END FRIDGE DUMP (%lu records) =====\n", (unsigned long)lines);
  quietSerial = false;
  pruneFs();
}

void processLine(String line) {
  line.trim();
  if (!line.length()) return;                       // blank lines (e.g. after `W`) are ignored
  if (line == ".") { Serial.println(F("  cancelled - nothing changed")); inputMode = IN_CMD; return; }
  if (inputMode == IN_FORGET) {
    inputMode = IN_CMD;
    if (line == "y" || line == "Y") forgetSaved();
    else Serial.println(F("  not confirmed - nothing changed"));
  } else if (inputMode == IN_ATEST) {
    inputMode = IN_CMD;
    char *end = nullptr;
    float p = strtof(line.c_str(), &end);
    if (end == line.c_str() || *end || !(p >= 0 && p <= 100)) Serial.println(F("  A: give a score from 0 to 100, e.g. A 90 - nothing changed"));
    else setAlertTest(p);
  }
}

void handleSerial() {
  if (inputMode != IN_CMD && millis() - inStartMs > SERIAL_LINE_TIMEOUT_MS) {
    Serial.println(F("  no input for 60 s - cancelled, nothing changed"));
    inputMode = IN_CMD; inBuf = "";
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (inputMode != IN_CMD) {
      if (c == '\r') continue;
      if (c == '\n') { String l = inBuf; inBuf = ""; processLine(l); }
      else if (inBuf.length() < 64) inBuf += c;
      continue;
    }
    if (c == '?') printStatus();
    else if (c == 'u' || c == 'U') {
      if (!uploadsEnabled) Serial.println(F("UPLOAD skipped: uploads are disabled (see boot messages)"));
      else {
        historyRequested = true; historyReason = "serial"; forceWifi = true;
        if (portalOpen) Serial.println(F("UPLOAD waits: uploads are paused while Wi-Fi setup is open"));
      }
    }
    else if (c == 'w') printWifiStatus();
    else if (c == 'W') {
      if (!savedSsid.length()) Serial.println(F("  no saved network - nothing to forget"));
      else {
        Serial.printf("  forget saved network \"%s\"? y = yes (. = cancel):\n", savedSsid.c_str());
        inputMode = IN_FORGET; inBuf = ""; inStartMs = millis();
      }
    }
    else if (c == 'd' || c == 'D') dumpLogs();
    else if (c == 'r' || c == 'R') dumpRejected();
    else if (c == 'x' || c == 'X') printIds();
    else if (c == 'a') printAlertLog();
    else if (c == 'A') {                 // A <pct>: the rest of the line is the score
      if (!curRun().foodTest) Serial.println(F("  A: ignored in calibration mode"));
      else { inputMode = IN_ATEST; inBuf = ""; inStartMs = millis(); }
    }
    else if (c == 'e' || c == 'E')    // experiments are started and ended only on the setup page or by the app
      Serial.println(F("  Experiments are started and ended on the setup page (Wi-Fi button) or by the app."));
  }
}

// =====================================================================
// Setup / loop
// =====================================================================

void setup() {
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);    digitalWrite(PIN_LED, LOW);
  pinMode(PIN_WIFI_BTN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ1, ADC_11db);
  analogSetPinAttenuation(PIN_MQ2, ADC_11db);

  // Part 4b: a soft reset keeps the previous boot's (drifted) clock. Start from 0 so that
  // nothing can mistake it for a time sync; only SNTP in this boot sets the clock.
  struct timeval tv0 = {0, 0};
  settimeofday(&tv0, nullptr);

  Serial.begin(115200);
  delay(1500);

  esp_reset_reason_t rr = esp_reset_reason();
  resetReasonStr = resetReasonName(rr);
  Serial.println();
  Serial.println(F("================================================"));
  Serial.printf("  FRIDGE001 MONITOR  %s %s\n", FIRMWARE_LABEL, FIRMWARE_VERSION);
  Serial.println(F("================================================"));
  Serial.printf("  Reset reason: %s\n", resetReasonStr.c_str());
  if (rr == ESP_RST_BROWNOUT)
    Serial.println(F("  !!! BROWNOUT RESET - the 5 V supply sagged."));

  lcdInit();
  lcdPut(0, "FRIDGE MONITOR");
  lcdPut(1, resetReasonStr.c_str());

  fsOk = LittleFS.begin(true);
  if (!fsOk) Serial.println(F("  !!! LittleFS mount failed - records will NOT be kept"));
  prefs.begin("fridge", false);
  bootCount = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootCount);
  nvsExp    = prefs.getString("exp", "");
  nvsSample = prefs.getString("smp", "");
  nvsFood   = prefs.getString("food", "");
  rejectedCount = prefs.getUInt("rejN", 0);
  producerUid = prefs.getString("uid", "");
  if (prefs.getString("rtokFor", "") == emailTag()) refreshToken = prefs.getString("rtok", "");

  // runIds continue above baseline_logger's boot counter and above any run on flash, so a
  // fresh NVS does not restart at R00001 and reuse measurementIds already in Firebase.
  uint32_t baselineBoot = 0;
  {
    Preferences pb;
    if (pb.begin("baseline", true)) { baselineBoot = pb.getUInt("boot", 0); pb.end(); }
  }
  uint32_t maxLocalRun = fsOk ? loadOldRuns() : 0;
  if (fsOk) scanOutbox();
  if (outboxPending) Serial.printf("  %lu experiment record(s) waiting in %s\n", (unsigned long)outboxPending, OUTBOX_PATH);
  if (rejectedCount) Serial.printf("  %lu record(s) in %s (never uploaded; `r` dumps them)\n", (unsigned long)rejectedCount, REJECTED_PATH);
  // runIds also continue above every run on flash; verifyRun() then checks Firebase before the first write.
  runCounter = std::max(prefs.getUInt("run", 0), std::max(baselineBoot, maxLocalRun));
  Serial.printf("  Boot #%lu\n", (unsigned long)bootCount);
  alertBoot();                             // before startRun: the run record gets the baseline
  startRun("boot");

  lcdPut(0, curRun().foodTest ? "EXPERIMENT:" : "CALIBRATION");
  lcdPut(1, curRun().expId.c_str());
  delay(2000);

  // Wi-Fi: the network saved with the setup page first, secrets.h as fallback.
  {
    Preferences wp;
    if (wp.begin("wifi", false)) { savedSsid = wp.getString("ssid", ""); savedPass = wp.getString("pass", ""); wp.end(); }
  }
  secretsWifiOk = !(looksPlaceholder(WIFI_SSID) || looksPlaceholder(WIFI_PASSWORD));
  wifiConfigured = secretsWifiOk || savedSsid.length() > 0;
  if (savedSsid.length())
    Serial.printf("  WiFi: saved network \"%s\" first, then %s\n", savedSsid.c_str(), secretsWifiOk ? "secrets.h" : "nothing");
  size_t apPwLen = strlen(SETUP_AP_PASSWORD);
  apPwOk = apPwLen >= 8 && apPwLen <= 13;
  if (!apPwOk)
    Serial.println(F("  !!! AP PW 8-13 CHARS: SETUP_AP_PASSWORD in secrets.h must be 8 to 13 characters; the Wi-Fi button will not open setup"));
  if (strlen(SETUP_AP_SSID) > 16)
    Serial.println(F("  WARN: SETUP_AP_SSID is longer than 16 characters - the LCD shows only the first 16"));
  btnRaw = btnLatched = digitalRead(PIN_WIFI_BTN) == LOW;
  btnRawSinceMs = millis();
  if (btnLatched)
    Serial.println(F("  Wi-Fi button is latched: release it (setup opens only on a fresh press)"));

  const char *missing = missingSetting();
  uploadsEnabled = missing == nullptr;
  if (!uploadsEnabled) {
    Serial.printf("  !!! UPLOADS DISABLED: %s in secrets.h is missing or a placeholder.\n", missing);
    Serial.println(wifiConfigured ? F("      Logging to flash; WiFi is used for the clock only.")
                                  : F("      Logging to flash only, with uptime (no clock)."));
    lcdPut(0, "UPLOADS DISABLED");
    lcdPut(1, "logging locally");
    delay(2000);
  } else if (String(FIREBASE_DATABASE_URL).indexOf("food-spoilage-7b60a") < 0) {
    Serial.println(F("  WARN: database URL does not contain \"food-spoilage-7b60a\" - check secrets.h"));
  }
  tls.setInsecure();  // no cert pinning, same as hw_selftest

  WiFi.persistent(false);   // networks are stored only by the setup page, after a successful test
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (wifiConfigured) WiFi.mode(WIFI_STA);   // SNTP starts once connected (see startSntp)
  setupWebRoutes();

  // Sampling starts now, so the WiFi connection in loop() runs alongside it.
  nextSampleMs  = millis();
  nextRecordMs  = RECORD_PERIOD_MS;
  nextLatestMs  = 0;
  nextHistoryMs = HISTORY_PERIOD_MS;
  if (uploadsEnabled) { historyRequested = true; historyReason = "boot"; }
  Serial.printf("  Sample %lu s, record %lu s, latest %lu s, history every %lu min, settling %lu min.\n",
                (unsigned long)(SAMPLE_PERIOD_MS / 1000), (unsigned long)(RECORD_PERIOD_MS / 1000),
                (unsigned long)(LATEST_PERIOD_MS / 1000), (unsigned long)(HISTORY_PERIOD_MS / 60000),
                (unsigned long)(SETTLE_S / 60));
  Serial.println(F("  Commands: ? status, u upload, d dump, r rejected, x IDs, w Wi-Fi, W forget Wi-Fi,"));
  Serial.println(F("            a alert log, A <pct> force the alert score (test only)"));
  if (fsOk) Serial.printf("  LittleFS: %u of %u bytes used\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  updateLcd();
}

void loop() {
  serviceSampling();
  handleSerial();
  serviceButton();
  if (portalOpen) servicePortal();

  uint32_t now = millis();
  if (!settleWarnBeeped && now / 1000 >= SETTLE_S - SETTLE_WARN_S) {
    settleWarnBeeped = true;
    if (curRun().foodTest) Serial.printf("*** settling ends in %lu min - scoring resumes then ***\n", (unsigned long)(SETTLE_WARN_S / 60));
    else Serial.printf("*** settling ends in %lu min - then wait %d min for the baseline (CAL READY); keep the food OUT ***\n",
                       (unsigned long)(SETTLE_WARN_S / 60), BASELINE_WINDOW_RECORDS);
    beep(3);
  }
  if (!settleBeeped && now / 1000 >= SETTLE_S) {
    settleBeeped = true;
    // 1.2.0: the food goes in only at CAL READY, after the baseline window (not at this beep).
    if (curRun().foodTest) Serial.printf("*** settling period over (%lu min) - scoring resumes ***\n", (unsigned long)(SETTLE_S / 60));
    else Serial.printf("*** settling period over (%lu min) - wait %d min for the baseline; keep the food OUT until CAL READY ***\n",
                       (unsigned long)(SETTLE_S / 60), BASELINE_WINDOW_RECORDS);
    beep(1);
  }
  if ((int32_t)(now - nextHistoryMs) >= 0) {
    while ((int32_t)(now - nextHistoryMs) >= 0) nextHistoryMs += HISTORY_PERIOD_MS;
    if (uploadsEnabled && !historyRequested) { historyRequested = true; historyReason = "periodic"; }
  }
  serviceAlertBuzzer();                    // Critical reminders, also while setup is open
  if (!portalOpen) serviceNetwork();       // Firebase uploads pause while setup is open
  updateLcd();
  delay(portalOpen ? 2 : 10);
}
