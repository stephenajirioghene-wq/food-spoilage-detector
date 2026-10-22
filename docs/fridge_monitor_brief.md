# Task brief: FRIDGE001 monitor firmware for the Smart Fridge app (raw-data structure v1)

## Purpose

The app team has defined how the hardware must upload data for cooked-meal trials. Their document is "Smart Fridge MVP: Hardware Upload Requirements for Cooked Meals", raw-data structure v1. This brief turns it into firmware. Where the two differ, the app team's document wins. Report any conflict you find; do not resolve it silently.

The board only measures and uploads. It does not calculate spoilage percentages or decisions. The app does that.

Deliverables:
1. `fridge_monitor/fridge_monitor.ino`, a new sketch. Build it on `baseline_logger` for:
   - LittleFS logging, one JSON line per record, flushed and closed every minute;
   - power-loss safety and detection of half-written lines;
   - the NVS upload pointer and the boot counter;
   - per-sensor statistics (mean, min, max, SD of raw ADC, voltage and Rs; Rs/R0; ppm; rail count);
   - the Serial `?`, `u` and `d` commands.
2. `experiment_tool.py`, a small command-line script Steve uses to write experiment metadata, observations and events (Part 2).

Do not modify `food_spoilage_test1_reboot_safe.ino`, `hw_selftest` or `baseline_logger`. Do not delete or move any existing Firebase data, including `/baseline`.

## Hardware and constants (unchanged)

| Part | ESP32 pin | Notes |
|---|---|---|
| MQ135 #1 (channel mq1, NH3 curve) | GPIO 34 | RL1 = 2000 ohm |
| MQ135 #2 (channel mq2, CO2 curve) | GPIO 35 | RL2 = 1370 ohm |
| 16x2 I2C LCD | SDA 16, SCL 17, 0x27 | Same pin binding as hw_selftest |
| Active buzzer | GPIO 27 | HIGH = on |
| LED | GPIO 33 | HIGH = on |

Calibration constants:
- R0_MQ1 = 7802.26 ohm and R0_MQ2 = 3463.12 ohm. Keep these.
- VCC 5 V; ADC 12-bit (max 4095), reference 3.3 V, 11 dB attenuation.
- Rs = RL x (VCC - Vout) / Vout; ppm = a x (Rs/R0)^b.
- NH3 curve: a = 102.2, b = -2.473. CO2 curve: a = 116.6020682, b = -2.769034857.

Both channels are MQ135 sensors. The MQ135 is not selective. "NH3" and "CO2" name the datasheet curve used to compute ppm; they are not measured gases. State this in the run metadata (see below).

There is no temperature or humidity sensor. Do not add those fields, not even as null.

## Firebase project and sign-in

- Project: food-spoilage-7b60a.
- The app team authorises writes by Firebase user UID. Anonymous sign-in creates a new UID on each new sign-in, so it cannot be used for this sketch. Sign in with a dedicated **email/password user** instead, using `signInWithPassword`.
  - Steve creates the user in the Firebase console (Authentication, Email/Password provider).
  - Steve adds `DEVICE_EMAIL` and `DEVICE_PASSWORD` to `secrets.h`.
- Refresh the ID token with the refresh token before it expires. Sign in again only if the refresh fails.
- At the first successful sign-in, print the UID to Serial once, labelled `Producer UID`. Steve sends this UID to the app team. Never print the email, password, API key or tokens.
- If `DEVICE_EMAIL` is missing or still a placeholder, keep logging locally and say clearly on Serial and the LCD that uploads are disabled.

## Firebase paths written by the firmware

| Path | Method | Content |
|---|---|---|
| `telemetry/FRIDGE001/latest` | PUT | The newest complete measurement envelope |
| `telemetry/FRIDGE001/runs/<runId>/samples/<measurementId>` | PATCH, batched | Every envelope (history) |
| `experiments/<experimentId>/runs/<runId>` | PATCH | Run metadata (below) |
| `experiments/<experimentId>/events/<eventId>` | PUT | Restart events written by the board (below) |

Never write to `devices/FRIDGE001/latest` or `decisionHistory`. The app team reserves them for assessments. Never write to `/baseline`, `/hwtest` or `/test1`.

The firmware never writes `experiments/<experimentId>/meta` or `observations`. Steve writes those with `experiment_tool.py`.

The app team configures the write rules for these paths using the producer UID. Until they confirm, uploads will fail with HTTP 401 or 403. Handle that like any other failed upload: keep the data in LittleFS and retry.

## Identifiers

- **experimentId and sampleId.** Set them over Serial with the `e` command (prompt for both). Store them in NVS so they survive power loss, as the spec requires for the same meal sample.
  - If none is set, mode is `calibration` and experimentId is `CAL-<bootCount>`.
  - Show the experimentId on the LCD at boot.
- **runId.** Use `R<bootCount>`, zero-padded to 5 digits (for example `R00012`). It is known at boot without NTP, so every run has an ID even if time never syncs. In the baseline test, a run that never synced had run_id 0. A new boot is always a new runId.
- **measurementId.** Use `<runId>-<uptime_s>`, for example `R00012-002640`. It is fixed when the record is made, so a retried upload reuses it.
- **eventId.** Use `<runId>-restart`.

## Timing

Make every period a named constant and report each one in the run metadata.
- Sample both sensors every 2 s.
- Build one record every 60 s.
- PUT `latest` every 60 s, as the spec asks. WiFi stays connected.
- Upload history every 10 minutes and on `u`.
- `expectedUpdateSeconds` in each envelope = the actual latest-upload period (60).

## Time

- All outer timestamps are Unix milliseconds. `sample.epoch` stays in Unix seconds and must describe the same moment: `measuredAtMs = epoch x 1000`.
- Set `uploadedAtMs` with the Firebase server timestamp (`{".sv": "timestamp"}`). That way upload time never depends on the board clock.
- **Never upload a record without a real epoch.** Records made before the first NTP sync are kept in LittleFS with uptime only. After the sync, rebuild their epoch from the uptime difference, as baseline_logger does, and then upload them.
- Add `timeSource`: `ntp` or `rebuilt`. The spec allows extra fields. This one tells the app which times were reconstructed.
- If time never syncs during a run, those records stay on the board and in the `d` dump. Do not guess a time.

## Measurement envelope

Follow section 4 of the app team's spec exactly for field names and nesting:
- `schemaVersion`, `deviceId`, `experimentId`, `sampleId`, `runId`, `measurementId`;
- `measuredAtMs`, `uploadedAtMs`, `expectedUpdateSeconds`, `mode`;
- `calibration {state, version}`;
- `quality {state}`;
- `sample {epoch, uptime_s, mq1 {...}, mq2 {...}}`. Each channel holds `n`, `ppm`, `rail`, `rs_r0` and the `raw`, `rs` and `volt` blocks, each with mean, min, max and sd.

Additions the spec allows:
- `timeSource`;
- `sample.free_heap`, `sample.unsent` and `sample.gap_s` (when a sampling gap above 5 s occurred);
- per-channel `quality` and `faults` inside `mq1` and `mq2`.

Values:
- `mode`: `food_test` when an experimentId is set, otherwise `calibration`. Those are the only two values allowed.
- `calibration.version`: `"aug-2026-r0"`. `calibration.state`:
  - `collecting_baseline` in calibration mode;
  - `pending_validation` otherwise.

  The August R0 was captured during warm-up. The October baseline measured new values that have not been adopted, so `validated` would overstate it.

### Quality rules

Evaluate per channel on every record, in this order:
1. **fault** if any of these hold. Add each matching flag to the channel's `faults`:
   - mean voltage below 0.05 V: `no_signal`. In the baseline test, boots 8 to 10 read like this with the ESP32 off the sensor board.
   - rail count above 0: `saturation`.
   - mean voltage above 3.2 V: `near_adc_limit`.
2. **settling** if uptime is under 3 h (`SETTLE_S` = 10800). The baseline test found sensor 1 still rising 215 minutes after power-on in one run.
3. **valid** otherwise.

Also add the flag `high_noise` when the voltage SD within the minute is above `NOISE_V_SD` = 0.02 V. That is roughly 4 times the largest within-minute SD seen in clean air. The flag does not change the state.

The envelope's top-level `quality.state` is the worst channel state, ranked fault, then settling, then valid. Make every threshold a named constant.

### latest

- PUT `latest` only with the record just made, never with a backfilled one. Then an older buffered record can never overwrite a newer one.
- Backfilled records go to the history path only.

## Run metadata

Write to `experiments/<experimentId>/runs/<runId>` at the first successful upload of each run, and update it when NTP syncs.
- **Identity:** deviceId, experimentId, sampleId, runId, purpose (`food_test` or `calibration`), firmware `fridge_monitor`, firmwareVersion `1.0.0`.
- **Boot:** bootCount, resetReason, bootEpochMs (once known), and `timeSyncedAtMs`.
- **Hardware:** adcMax 4095, adcRefV 3.3, supplyV 5.0, and attenuation.
- **Channels:** for each one, model `MQ135`, curveGas (`NH3` or `CO2`), `selective: false`, rlOhm, r0Ohm, curveA and curveB.
- **Calibration:** `resistanceUnit: "ohm"`, calibrationVersion, calibrationState, and `calibrationValidAfterRestart: false`. The board cannot check this itself; the app decides.
- **Periods:** samplePeriodS 2, recordPeriodS 60, latestPeriodS 60, historyPeriodS 600.
- **Paths:** `samplesPath` and `latestPath`, written as the full path strings.
- **Stabilisation:** settleS 10800 and the quality thresholds.

## Restart events

At each boot with an experimentId set, write `experiments/<experimentId>/events/<runId>-restart` once time is known:

```json
{"experimentId": "...", "runId": "R00013", "type": "device_restart",
 "observedAtMs": 0, "resetReason": "POWERON",
 "notes": "Board restarted. Outage start and duration unknown to the board."}
```

- `observedAtMs` is the boot time in ms.
- If the reset reason is BROWNOUT, say so in `notes`.
- The board cannot tell a sensor-power cut from a refrigeration-power cut. Leave that to Steve's own events.

## Local display and alerts

- LCD line 1 shows the mode and quality, e.g. `FOOD SETTL 1h12m`, `FOOD VALID`, `CAL VALID` or `FAULT MQ1`.
- LCD line 2 shows both voltages and the unsent count, e.g. `0.42V 0.71V U3`.
- LED gives a short blink every 2 s while logging, and a double blink while the last upload failed.
- Buzzer stays silent. The board makes no spoilage decision, so it sounds no spoilage alarm. One short beep marks the end of settling.

## Serial (115200)

- Keep the baseline_logger behaviour: one line per record, plus `?`, `u` and `d`.
- Add `e`: set or clear experimentId and sampleId (clearing returns to calibration mode).
- Add `x`: show the current experimentId, sampleId, runId and producer UID.

## Part 2: experiment_tool.py

A command-line script for Steve, run on his PC.
- It reads `secrets.h` for the API key, database URL, device email and password.
- It signs in the same way as the board and never prints credentials.

Commands:
- **new-experiment.** Prompts for each field in section 3 of the spec and writes `experiments/<experimentId>/meta`.
  - Required: experimentId, sampleId, food name, storage location, startedAt.
  - Optional: cookedAt, storedAt, container, recipe, portion with unit, covered or open, prior storage or reheating.
  - Fixed values: category `cooked_meal`, preparation `cooked`, purpose `food_test`, status `running`, schemaVersion 1.
  - Times are typed in WAT (`YYYY-MM-DD HH:MM`) and stored as Unix ms. Optional fields left blank are omitted, never filled with a default.
- **observe.** Writes an observation.
  - Label must be one of unknown, fresh, spoiled or inconclusive.
  - Also takes the method, notes and the time in WAT (default now).
- **event.** Writes an event with a type, time and notes. Allowed types:
  - `power_outage`, with a separate choice of sensor power or refrigeration power;
  - `storage_change`;
  - `sample_replaced`;
  - `door_opened`;
  - `other`.
- **status.** Sets the experiment status: running, interrupted, completed or abandoned.
- **show.** Prints the experiment meta, runs, events and observations, and the newest latest envelope, so Steve can send the handoff items in section 6 of the spec.

Use PATCH or PUT so that nothing else is overwritten. Never delete.

## Final notes to include

- How to create the device user in the console, and how to read the producer UID from Serial.
- A note that the app team must add write rules for `telemetry/FRIDGE001` and `experiments` for that UID before uploads succeed.
- The exact RTDB paths, for the handoff to the app team.
- The flash warning from baseline_logger: keep "Erase All Flash" disabled, and dump before reflashing with a different partition scheme.
- Anything in this brief or the app team's spec that could not be done as written.
