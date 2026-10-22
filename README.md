# FRIDGE001 food spoilage detector

FRIDGE001 is an ESP32 board that sits in a refrigerator next to a cooked-meal sample and logs the air
with two MQ135 gas sensors. Every minute it builds one record (mean, min, max and SD of raw ADC,
voltage and Rs per sensor, Rs/R0, ppm, quality flags), keeps it in LittleFS and uploads it to a
Firebase Realtime Database for the Smart Fridge app. It implements the app team's
"Smart Fridge MVP: Hardware Upload Requirements for Cooked Meals", raw-data structure v1
([docs/app_upload_requirements_v1.pdf](docs/app_upload_requirements_v1.pdf)).

The app makes the spoilage assessment. From firmware 1.2.0 the board also shows its own alert
(FRESH / WARNING / SPOILT on the LCD, buzzer at Critical) and uploads it as the extra field
`deviceAlert`. Its thresholds come from one spoilt-rice run and are not validated yet.

Both sensors are MQ135s, which are not selective: "NH3" and "CO2" name the datasheet curve used
for ppm, not measured gases. There is no temperature or humidity sensor.

## What is in this repository

| Path | What it is |
|---|---|
| [`fridge_monitor/`](fridge_monitor) | **Current firmware, 1.2.0**: logging, upload, Wi-Fi setup page, experiments, on-board alert |
| [`make_setup_page.py`](make_setup_page.py) | Gzips `fridge_monitor/setup_page.html` into `setup_page.h`. Run it after editing the page |
| [`experiment_tool.py`](experiment_tool.py) | PC command-line tool for experiment metadata, observations, events and status |
| [`docs/`](docs) | Briefs, release notes, the app team's spec and the test data behind the alert thresholds |

Only firmware 1.2.0 and what it needs are here. Earlier sketches (1.0.0 and 1.1.0 backups,
`baseline_logger`, `hw_selftest`, `lcd_test`) are not part of this repository.

## Hardware and wiring

| Part | ESP32 pin | Notes |
|---|---|---|
| MQ135 #1 (channel `mq1`, NH3 curve) | GPIO 34 | RL1 = 2000 ohm, R0 = 7802.26 ohm |
| MQ135 #2 (channel `mq2`, CO2 curve) | GPIO 35 | RL2 = 1370 ohm, R0 = 3463.12 ohm |
| 16x2 I2C LCD | SDA GPIO 16, SCL GPIO 17 | address 0x27 (0x3F also detected) |
| Active buzzer | GPIO 27 | HIGH = on |
| LED | GPIO 33 | HIGH = on |
| Latching Wi-Fi setup button | GPIO 4 to GND | `INPUT_PULLUP`, latched = LOW |

Sensors run from 5 V. ADC: 12-bit (max 4095), 3.3 V reference, 11 dB attenuation.
`Rs = RL x (VCC - Vout) / Vout`, `ppm = a x (Rs/R0)^b`
(NH3: a = 102.2, b = -2.473; CO2: a = 116.6020682, b = -2.769034857).

## Arduino IDE settings

| Tools menu | Setting |
|---|---|
| Board | ESP32 Dev Module |
| esp32 core | 3.3.12 |
| Partition Scheme | **No OTA (2MB APP/2MB SPIFFS)**. 1.2.0 does not fit the default 1.25 MB app partition |
| Erase All Flash Before Sketch Upload | **Disabled**, which keeps NVS (boot and run counters, Wi-Fi, experiment, baseline) |
| Serial Monitor | 115200 baud, line ending "Newline" |

Libraries: LiquidCrystal I2C (Frank de Brabander) 1.1.2, and ArduinoJson 7.x (built with 7.4.3).
WiFi, HTTPClient, WiFiClientSecure, WebServer, DNSServer, Wire, LittleFS and Preferences come with the core.

Switching from the default partition scheme reformats LittleFS. Dump the logs with `d` and `r` on
Serial first.

## Filling in secrets.h

The sketch does `#include "secrets.h"`. That file is in `.gitignore` and is never committed.

1. Copy `fridge_monitor/secrets.example.h` to `fridge_monitor/secrets.h`.
2. Fill in:
   - `WIFI_SSID`, `WIFI_PASSWORD`: a 2.4 GHz network (the ESP32 cannot see 5 GHz). This is the
     fallback; a network saved with the setup page is tried first.
   - `FIREBASE_API_KEY`: Firebase console, Project settings > General > Web API key.
   - `FIREBASE_DATABASE_URL`: Realtime Database > Data tab, the URL at the top.
   - `DEVICE_EMAIL`, `DEVICE_PASSWORD`: an Email/Password user you create under Authentication.
     While these are missing, the board logs locally and says that uploads are disabled. At the
     first sign-in it prints the `Producer UID`, which the app team needs for their write rules.
   - `SETUP_AP_SSID`, `SETUP_AP_PASSWORD` (8 to 13 characters): the Wi-Fi setup network. The
     password is shown on the LCD while setup is open.

`experiment_tool.py` reads `secrets.h` next to it, else `fridge_monitor/secrets.h`, or the path given with `--secrets`.

## Firebase paths the board writes

| Path | Method | Content |
|---|---|---|
| `telemetry/FRIDGE001/latest` | PUT every 60 s | newest measurement envelope |
| `telemetry/FRIDGE001/runs/<runId>/samples/<measurementId>` | PATCH, batches of 10, every 10 min | every envelope (history) |
| `experiments/<experimentId>/runs/<runId>` | PUT once, then PATCH | run metadata (incl. `deviceAlert` constants and baseline) |
| `experiments/<experimentId>/meta` | PUT at start, PATCH at end | experiment metadata |
| `experiments/<experimentId>/observations/<sampleId>-obs-<epoch>` | PUT | observation from the setup page |
| `experiments/<experimentId>/events/<runId>-restart` | PUT | `device_restart` |
| `experiments/<experimentId>/events/<runId>-experiment-start` / `-experiment-end` | PUT | experiment start and end |
| `experiments/<experimentId>/events/<runId>-<type>-<epoch>` | PUT | events logged on the setup page |
| `experiments/<experimentId>/events/<runId>-wifi-<epoch>` | PUT | Wi-Fi changed during a trial |
| `experiments/<experimentId>/events/<runId>-alert-<n>` | PUT | `device_alert` band change |
| `control/FRIDGE001/command` | GET, then PUT `""` | command from the app (read and cleared) |
| `control/FRIDGE001/ack/<commandId>` | PUT | command acknowledgement |

IDs: `runId` = `R<bootCount>` (5 digits, e.g. `R00092`), `measurementId` = `<runId>-<uptime_s>`.
Without an experiment the mode is `calibration` and the experimentId is `CAL-<bootCount>`.
The board never writes `devices/FRIDGE001/latest` or `decisionHistory` (reserved for the app),
and never sends DELETE.

## Docs

1.2.0 builds on 1.0.0 and 1.1.0, so their briefs and notes still describe parts of the current firmware.

| What | File |
|---|---|
| App team requirements | [app_upload_requirements_v1.pdf](docs/app_upload_requirements_v1.pdf) |
| 1.2.0: on-board spoilage alert | [spoilage_alert_brief.md](docs/spoilage_alert_brief.md), [fridge_monitor_1.2.0_notes.md](docs/fridge_monitor_1.2.0_notes.md) (current release notes) |
| 1.1.0: experiments from the setup page and the app | [experiment_page_brief.md](docs/experiment_page_brief.md), [fridge_monitor_1.1.0_notes.md](docs/fridge_monitor_1.1.0_notes.md) (RTDB paths and rules for the app team) |
| Wi-Fi setup page | [wifi_setup_brief.md](docs/wifi_setup_brief.md), [wifi_setup_notes.md](docs/wifi_setup_notes.md) |
| 1.0.0: monitor firmware and upload format | [fridge_monitor_brief.md](docs/fridge_monitor_brief.md), [fridge_monitor_notes.md](docs/fridge_monitor_notes.md) (device user setup) |
| Data behind the alert thresholds | [calibration_data_findings_2026-10-17.md](docs/calibration_data_findings_2026-10-17.md), [rice_test_RICE-SPOILT-001_2026-10-20.md](docs/rice_test_RICE-SPOILT-001_2026-10-20.md) |
| Spoilt-rice test RICE-SPOILT-001 | [docs/rice_test_RICE-SPOILT-001/](docs/rice_test_RICE-SPOILT-001): report (.docx), chart, run R00062 CSV and JSON export |
