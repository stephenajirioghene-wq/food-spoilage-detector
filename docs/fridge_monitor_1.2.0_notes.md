# FRIDGE001 firmware 1.2.0: notes (on-board spoilage alert)

Changed files:
- `fridge_monitor/fridge_monitor.ino`: firmware 1.2.0, built on the 1.1.0 sketch.
- `fridge_monitor/setup_page.html`: alert and baseline rows, the no-baseline reason and the Silence button in the Experiment status block; one hint line on the Start tab.
- `fridge_monitor/setup_page.h`: regenerated with `python make_setup_page.py` (18,457 bytes, 6,088 gzipped).

**Added on 22 Oct 2026 at Steve's request: CAL READY.** The settling messages no longer say "put the food in". The board beeps once (long) and shows `CAL READY` when the 15-minute clean-air baseline window is full. See section 1, "CAL READY".

**Added on 22 Oct 2026 at Steve's request (not in the brief): a food reference.** It is a second baseline taken from the food's own early minutes and uploaded with each record's ratio to it. It is record only: the alert, its bands, LCD, buzzer and events still use the clean-air baseline. See section 1, "Food reference", and decision D9.

Backup: `fridge_monitor_v1.1.0_backup/fridge_monitor_v1.1.0_backup.ino` (identical to the 1.1.0 sketch, SHA-256 checked). `setup_page.h` and `setup_page.html` of 1.1.0 are next to it so the backup compiles. `secrets.h` is not in the backup.

Not modified: both `secrets.h` files, `baseline_logger`, `hw_selftest`, `experiment_tool.py`. There is no `food_spoilage_test1_reboot_safe.ino` in this folder, so it could not have been touched.

The firmware still never writes `devices/FRIDGE001/latest` or `decisionHistory` (no string in the sketch names either path, other than the header comment saying so), and still never sends DELETE.

## Compile and flash result

- **Compile:** ESP32 Dev Module, esp32 core 3.3.12, ArduinoJson 7.4.3, Partition Scheme **No OTA (2MB APP/2MB SPIFFS)** (`PartitionScheme=no_ota`), Erase All Flash **Disabled** (`EraseFlash=none`), `--warnings all`, clean build. **No warnings or errors from the sketch.**
  - Program: **1,335,132 bytes, 63% of 2,097,152** (1.1.0: 1,305,628 bytes, 99.6% of the old 1.25 MB). About 762 KB is left. Without the food reference and CAL READY it was 1,329,280 bytes.
  - Globals: 53,920 bytes (16%), 273,760 left for local variables.
  - Setup page: 18,457 bytes, served gzipped as 6,088 bytes.
- **Reflashed with CAL READY on 22 Oct 2026 at about 01:09 WAT** (same scheme, Erase All Flash disabled). LittleFS kept (runs R00087 to R00091), boot #121, run **R00092**. `?` shows the new line `Baseline if started now: mq1 0, mq2 0 valid records (10 needed, 15 for CAL READY) - NOT ready: keep the food out`. CAL READY itself was not reached: it needs 45 min, and MQ2 has no signal on this bench.
- **Reflashed with the food reference on 22 Oct 2026 at about 00:37 WAT** (same scheme, Erase All Flash disabled). This time LittleFS was kept: runs R00087 to R00089 were still on flash, R00089's unsent record uploaded, and the new boot is #119, run **R00090**. Everything below about the first flash still applies.
- **Flashed to COM14 on 22 Oct 2026 at about 00:08 WAT**, Erase All Flash disabled. esptool verified all four images.
  - The post-upload reset booted once (boot 116, run **R00087**) before my Serial reset (boot 117, run **R00088**). R00087 has no records, only its run metadata.
  - **NVS kept:** boot counter (116, 117), run counter (R00087, R00088, both above R00086), Wi-Fi, producer UID, `rejected` count.
  - `runId check: highest runId in Firebase is R00086`. NTP sync, `latest` PUT OK, history upload OK.
  - **LittleFS reformatted:** `LittleFS: 8192 of 1966080 bytes used` at boot, `1953792 free` in `?`. The outbox and `/rejected.jsonl` are empty (`r` dumps 0 lines). Your saved `d` and `r` dumps are now the only copy of those 105 rejected records.
  - `d` shows the run metadata with `firmwareVersion` `1.2.0` and the new `deviceAlert` constants block. Each calibration record holds `"deviceAlert":{"method":"rs_drop_log_v1","state":"off"}`.
  - `?` shows `spoilage alert: off (calibration). Baseline if started now: mq1 0, mq2 0 valid records of 10 needed - NOT ready` (settling).
- **Found on the board, not changed:**
  - **F1. `rejected` count is now wrong.** NVS `rejN` is still 105 and every envelope says `sample.rejected: 105`, but the reformat erased `/rejected.jsonl`. Firmware 1.2.0 does not reset the count, because the brief keeps 1.1.0's upload fields as they are and the count matches your saved `r` dump. If you want it to read 0 (the file's real content), that needs a one-line change at boot or an NVS clear of `rejN`. Your call; tell the app team either way.
  - **F2. The board looks to be off the sensor board.** `LCD: not found at 0x27/0x3F`, MQ2 mean 0.002 V (`no_signal`), and MQ1 noisy with readings at 0 (`rail` 8 to 12, `high_noise`). Put it back on the sensor board before Part 8; nothing on the LCD, buzzer or gas response could be checked here.
  - `GET control/FRIDGE001/command -> HTTP 401 Permission denied`: still the missing read rule from 1.1.0 (Firebase commands cannot work yet).
  - One Serial line arrived garbled in my capture (`????...`) and the next one was clean; this looks like the USB link or my capture, not the firmware.

## 1. What changed

### Part 1: partition scheme
- Tools menu settings are in the header comment: **ESP32 Dev Module, core 3.3.12, Partition Scheme "No OTA (2MB APP/2MB SPIFFS)", Erase All Flash Before Sketch Upload: Disabled.**
- The scheme exists in core 3.3.12 (`no_ota.csv`):

  | Partition | Offset | Size |
  |---|---|---|
  | nvs | 0x9000 | 0x5000 (same as the default scheme, so NVS survives) |
  | app0 | 0x10000 | 2,097,152 bytes |
  | spiffs | 0x210000 | 1,966,080 bytes (1.875 MB, not quite the "2MB" in the menu name) |
  | coredump | 0x3F0000 | 64 KB |

- LittleFS still mounts on the `spiffs` label (`LittleFS.begin(true)`, unchanged). The partition moves from 0x290000 to 0x210000, so the first boot finds no file system and formats it. NVS (boot counter, run counter, Wi-Fi, experiment IDs, command history, rejected count) is at the same place and is kept.
- The boot banner now prints `LittleFS: <used> of <total> bytes used`, and `?` shows the free space.

### Part 2: baseline
- A rolling buffer holds the last 15 one-minute records of this boot (RAM): mean Rs and quality of each channel, and the record's uptime.
- At an experiment start (setup page or Firebase command: the same `startExperimentChecked()`), each channel's baseline is the **median** mean-Rs of the buffered records that are `valid` for that channel and no more than 20 min old. The buffer is in RAM, so every record in it is from the current boot.
- A channel is usable with at least 10 such records. If neither is usable, the experiment still starts with alert state `no_baseline` for the whole experiment. The page shows "Baseline not ready: wait until the LCD shows CAL VALID for 15 min before starting", in the Start result and in the status block.
- NVS keeps the baseline with the experimentId (`blExp`, `blR1`/`blR2` ohm, `blN1`/`blN2` records used, `blW` records in the window, `blFrom`/`blTo` Unix ms of the first and last record used). A reboot keeps it. The experiment end removes it.
- Shown on the setup page status block, in `?` and in `x`. In calibration mode the page and `?` also say whether a Start now would get a baseline, and whether CAL READY has been reached.
- An experiment started under 1.1.0 and still running at the flash has no baseline in NVS: it shows `no_baseline` until it ends.

### CAL READY: when the food goes in (added at Steve's request, 22 Oct 2026)
- **Problem:** 1.1.0's end-of-settling message said "settling period over - put the food in" and beeped. The baseline then needs another 15 min of clean air. Food put in at that beep, with Start pressed later, ends up in the baseline. With very spoilt food that can hide the whole alarm.
- **Settling messages (calibration mode):**
  - 5 min before the end: `*** settling ends in 5 min - then wait 15 min for the baseline (CAL READY); keep the food OUT ***` (3 beeps, as before).
  - At the end: `*** settling period over (30 min) - wait 15 min for the baseline; keep the food OUT until CAL READY ***` (1 beep, as before).
  - In food-test mode (after a reboot during an experiment, food already in) they say `scoring resumes` instead.
- **CAL READY:** in calibration mode, checked at every record. It is reached when every channel that has valid records has a full window (15 valid records, no more than 20 min old), and at least one channel has. Then:
  - one **long beep** (600 ms, unlike every other beep);
  - Serial `*** CAL READY: 15 min of clean air - put the food in now, then press Start ***`;
  - LCD line 1 `CAL READY` instead of `CAL VALID`;
  - the page's Baseline row says "CAL READY: put the food in now, then Start", and `?` says the same.
- If a record stops being valid (fault), CAL READY is lost, Serial says so, the LCD goes back to `CAL VALID`, and the beep sounds again when it is reached again.
- **Ending an experiment now empties the record buffer.** Before this, ending and immediately starting again would take the new baseline from records with the old food still in. Now a new experiment always needs 15 fresh minutes, signalled by CAL READY.
- A Start before CAL READY works as before: usable with 10 records, `no_baseline` below that. CAL READY asks for 15 so the full window is clean air.
- A channel at `FAULT` (for example MQ2 disconnected) keeps the LCD on `FAULT MQ2`, as in 1.1.0. The long beep and Serial line still come when the other channel is ready.

### Food reference (added at Steve's request, record only)
- **Why:** the clean-air baseline measures how far Rs has fallen since there was no food. A reference from the fresh food measures how far it has fallen since the food was fresh, without the food's normal smell. Uploading both lets a fresh-to-spoilt trial show which one tracks spoilage better, before any threshold is set for the second.
- **When:** the window is the 15 records ending 45 min after Start (`FOOD_REF_DELAY_S`), so about minutes 30 to 45. By then warm food has cooled and its first burst of steam and volatiles is over. Same rules as the baseline: median mean-Rs of the records that are `valid`, of this boot, no more than 20 min old, at least 10 per channel.
- **Retry and give up:** it is tried at every record from the due time on. If too few records qualify (for example a reboot put the board back in settling), it keeps trying. It is given up 60 min after the due time (`FOOD_REF_LATE_S`): state `missed`. A late reference records its real window times.
- **Stored** in NVS with the experiment (`frSt`, `frDue`, `frR1`, `frR2`, `frN1`, `frN2`, `frW`, `frFrom`, `frTo`), so a reboot keeps it, and cleared at the end. It is also stored in the run's file on flash for the run record.
- **Uploaded:** `deviceAlert.foodRef` in every food-test envelope (state `pending` with `dueMs`, then `taken` with the reference and each record's `mq1Ratio` / `mq2Ratio`, or `missed`). The run record's `deviceAlert` gets `foodRefDelayS`, `foodRefLateS`, `foodRefRules` and, once taken, `foodRef`. It works the same in a `no_baseline` experiment.
- **No score, no band, no event, no LCD change.** There is no data yet for thresholds on this ratio.
- **Shown** in `?` and `x` (`food reference : pending, due ...` / the values), and on the page ("Food reference (record only)"). Serial prints `FOOD REFERENCE taken ...` or `FOOD REFERENCE missed ...` once.

### Part 3: score
- Per channel with a usable baseline: `ratio = Rs_mean / Rs_baseline`, `pct = 100 x log10(ratio) / log10(0.10)`, clamped 0 to 100. The experiment score is the higher channel score.
- A channel is **used** when uptime >= `SETTLE_S` and its Rs is defined (mean voltage at least 0.05 V). So a `no_signal` channel is left out, and so is every channel while settling after a reboot.
- A channel at the ADC rail **counts**, with `saturated: true`. Its Rs is computed at 3.3 V, an upper bound, so the real score can only be higher. See conflict C2.
- If no channel is used, the state is `settling` (uptime < `SETTLE_S`) or `fault`, and the last band is kept.

### Part 4: states and persistence
- States: `off`, `no_baseline`, `settling`, `fault`, `normal`, `warning`, `critical`. Evaluated once per one-minute record, in food-test mode only.
- Up a band: score at or above the band's threshold for `RISE_RECORDS` = 3 records in a row. Normal can go straight to Critical when the score is >= 80% for 3 records.
- Down: below the band's threshold minus 10 for `FALL_RECORDS` = 5 records in a row. Critical drops below 70%, Warning below 30%. If both counts are met, Critical goes straight to Normal.
- A record that is not scored (settling, fault) restarts both counts.
- Every band change is logged on Serial: `ALERT BAND warning -> critical at 2026-10-22 14:03 WAT: score 84.2%, mq1 ratio 0.140, mq2 ratio 0.310`.
- The band is in NVS (`alBand`), together with the Silence flag (`alSil`) and the event counter (`alN`). After a reboot the state is `settling` until `SETTLE_S`; then scoring resumes against the stored baseline, **starting from the held band** (so a Critical stays Critical until it really falls). See decision D3.
- All constants (`FULL_SCALE_RATIO`, `WARN_PCT`, `CRIT_PCT`, `HYSTERESIS_PCT`, `RISE_RECORDS`, `FALL_RECORDS`, `BASELINE_*`, `CRIT_REMIND_MS`) are named at the top of the sketch.

### Part 5: LCD, buzzer, LED
- Food-test line 1: `FRESH 12%`, `WARNING 55%`, `SPOILT 92%`, `NO BASELINE`, `FOOD SETTL 0h22m` or `HELD SPOILT` / `HELD WARNING`, `FAULT MQ1` / `FAULT MQ2` / `FAULT MQ1+MQ2`. Right after Start, before the first scored record, it shows `FRESH`.
- Records forced with `A` add ` TEST` (`SPOILT 90% TEST`). See decision D6.
- Line 2, the `FOOD <id>` / `EXP ENDED` / `RELEASE WIFI BTN` messages and calibration mode are unchanged.
- Buzzer: rising into Warning from Normal gives 2 short beeps once. Entering Critical gives 5 pulses, then 5 pulses every 10 min while Critical and not silenced. Settling and button beeps are unchanged.
- LED: the normal logging blink in Normal and all other states; 1 s on / 1 s off in Warning; 200 ms on / off in Critical. The setup portal still drives the LED itself (250 ms) while it is open.
- **Silence alarm** button in the page's status block, shown only while Critical and not silenced (`POST /exp/silence`). It stops the reminders for this Critical episode only; state, LCD and upload are unchanged. A new rise into Critical (after it dropped) sounds again. Like every page action, it needs the portal open (latch button and setup password).

### Part 6: upload
- `deviceAlert` is added to every envelope (history samples and `telemetry/FRIDGE001/latest`). It is stored with the record on flash, so backfilled records carry the verdict of their own minute. Field list in section 3.
- Calibration mode: `{"method": "rs_drop_log_v1", "state": "off"}` only.
- Run record `experiments/<experimentId>/runs/<runId>`: a `deviceAlert` object with the method, all constants, a `rules` text and, for food-test runs, the experiment's `baseline`. The baseline is also in the run's file on flash, so a later PATCH of the run record (for example at the experiment end) keeps it.
- Band change: event `experiments/<experimentId>/events/<runId>-alert-<n>` through the outbox (survives Wi-Fi loss and power cuts), `n` counting from 1 per experiment. Fields in section 3.
- `firmwareVersion` is `1.2.0`. Nothing else in the envelope, run record or other events changed.

### Part 7: Serial
- `?` adds the alert state, score, band, both ratios (with pct, used/not used and why, SATURATED) and the baseline. In calibration mode it says whether a Start would get a baseline now.
- `x` adds the baseline.
- `a`: the last 15 evaluations, oldest first (time, score, both ratios, state, TEST).
- `A <pct>` (for example `A 90`, then Enter): the next 10 records score `<pct>`. A warning is printed when it is set and at each forced record; the records carry `"test": true` in `deviceAlert`; band changes they cause are real events, also with `test: true`. Ignored in calibration mode and in a `no_baseline` experiment. A forced record still needs uptime >= `SETTLE_S`.
- Each `REC` line in food-test mode ends with the alert state and score (`AL=warning 55.2%`).

## 2. Conflicts and decisions (none resolved silently)

### Against the app team's PDF
- **C1. The PDF says the hardware "does not need to calculate food-spoilage decisions or percentages".** It does not forbid it. 1.2.0 calculates one, as an extra field only. The PDF reserves `devices/FRIDGE001/latest` and `decisionHistory` for assessments; 1.2.0 never writes either. The raw statistics, quality and calibration fields are unchanged, so the app's own scoring is not affected.
- **PDF section 6:** "Do not use the sensor's own prediction ... as a reference food-condition label." The board never writes an observation from its verdict, and the page's observation form still offers no label from the sensors. Each alert event's `notes` says "Device verdict only, not an independent food observation." The app team should keep `deviceAlert` and `device_alert` events out of their reference labels.
- **Event type `device_alert`.** The PDF has no list of allowed event types. Its only example is `device_restart`, and it asks for `observedAtMs`, `type`, `runId` and `notes`. So `device_alert` is not forbidden, but it is new: the app team must accept it. It is kept, as the brief says. Each alert event has `runId` and `notes`, as the PDF asks of events.
- **`deviceAlert` is an extra envelope field.** The PDF defines the envelope's required fields but does not forbid others (1.0.0 and 1.1.0 already add `timeSource`, `rejected` and others). It is listed for the app team in section 3.
- **Write access.** The PDF asks for producer write access to be confirmed before using paths. 1.2.0 writes no new path: alert events go under `experiments/<id>/events/`, which 1.1.0 already writes.

### Against the brief, or inside the brief
- **C2. "A channel with a fault is left out" against "a channel at the ADC rail still counts".** In 1.0.0 and 1.1.0 any reading at 4095 makes the channel's quality `fault` (`saturation`), and `near_adc_limit` (mean above 3.2 V) is also a fault. Both happen exactly when Rs falls a lot (spoiling food: MQ2 hit the rail in R00062). So the alert leaves out only channels with no Rs (`no_signal`); rail and near-limit channels count, marked `saturated` when any reading was at 4095. The uploaded `quality` is unchanged (still `fault`), so the app sees the same data as before.
- **C3. Baseline `n`.** The brief stores the number of records per channel, but its example has a single `baseline.n`. The upload has both: `n` = records of this boot in the 20-min window at the start (at most 15), `mq1N` / `mq2N` = valid records used per channel. A channel without a usable baseline has no `mq1RsOhm`/`mq2RsOhm`.
- **C4. "No change to the experiment start logic, apart from what this brief adds."** One page text changed with the new procedure. 1.1.0's Start result said "Put the food in after settling ends", which now contradicts "put the food in, then press Start". It now gives the baseline (or the not-ready reason) and "Log the food going in with Log event, Storage change." The Start tab has one hint line: keep the food out until CAL READY, then put it in and press Start. The brief's exact no-baseline message ("wait until the LCD shows CAL VALID for 15 min") is kept, and it is still true: CAL READY is the end of those 15 min. The checks and records of the start are unchanged.
- **C5. Firebase-command starts** take the baseline the same way, but only the page shows the no-baseline reason (the brief asks for the page). The ack format is unchanged; the app sees `deviceAlert.state: "no_baseline"` in the next envelope and the reason on Serial.

### Decisions where the brief left a choice
- **D1. "Entering Warning: 2 short beeps."** Only when rising from Normal. A drop from Critical to Warning is good news and is silent.
- **D2. `fromState` / `toState`** in the event are band names (`normal`, `warning`, `critical`). Moves into `settling`, `fault` or `no_baseline` are not band changes and make no event.
- **D3. After a reboot, scoring resumes from the held band,** not from Normal. A Critical result does not disappear after a power cut, and no duplicate `normal -> critical` event is made. If it is still Critical and not silenced, the reminder sounds as soon as scoring resumes, then every 10 min. The Silence flag survives a reboot.
- **D4. `sinceMs`** = when the current state began. It is taken from the record's own boot time at upload, so records made before the clock was set still get it; a pre-sync record whose run never synced has none (such runs are rejected anyway).
- **D5. Alert events made before this boot's time sync** carry the uptime in the outbox and get `observedAtMs` from the run's boot time at upload, the same way records are rebuilt. If that boot never syncs, the event goes to the rejected file with the reason.
- **D6. ` TEST` on the LCD** for forced records. It is not in the brief's table but keeps a bench test from being mistaken for a real verdict. It fits in 16 characters (`WARNING 50% TEST`).
- **D7. `band` and per-channel `reason`** are extra fields in `deviceAlert` (section 3). `band` is the verdict kept while the state is `settling` or `fault`.
- **D9. Food reference (Steve, 22 Oct 2026).** It was not in the brief, so it was added as extra data only: `deviceAlert.foodRef` plus run-record fields. The brief's verdict, thresholds, events and LCD are unchanged. The 30 to 45 min window and the 60 min give-up are my choices; both are named constants at the top of the sketch.
- **D8. `scorePct`** is present only when this record was scored. A `settling` or `fault` record has `band` but no score, so no stale number is uploaded as if measured.

## 3. For the app team: `deviceAlert` and the other extra fields

### Envelope (`telemetry/FRIDGE001/latest` and `.../runs/<runId>/samples/<measurementId>`)

```json
"deviceAlert": {
  "method": "rs_drop_log_v1",
  "state": "warning",
  "scorePct": 55.2,
  "band": "warning",
  "channels": {
    "mq1": {"ratio": 0.39, "pct": 40.9, "used": true, "saturated": false},
    "mq2": {"ratio": 0.53, "pct": 27.6, "used": true, "saturated": false}
  },
  "baseline": {"mq1RsOhm": 21800, "mq2RsOhm": 6170, "n": 15, "mq1N": 15, "mq2N": 15,
               "fromMs": 1792600000000, "toMs": 1792600840000},
  "fullScaleRatio": 0.1, "warnPct": 40, "critPct": 80,
  "sinceMs": 1792601020000,
  "silenced": false
}
```
The numbers show the shape only; they are not measurements.

| Field | Type | Meaning |
|---|---|---|
| `method` | string | `rs_drop_log_v1` (always present) |
| `state` | string | `off` (calibration mode, nothing else follows), `no_baseline`, `settling`, `fault`, `normal`, `warning`, `critical` (always present) |
| `scorePct` | number | experiment score of this record, 0 to 100, 1 decimal; only when this record was scored |
| `band` | string | verdict band held: `normal`, `warning` or `critical`; present when the experiment has a baseline (extra, D7) |
| `channels.mq1` / `.mq2` | object | `ratio` = Rs mean / baseline (4 decimals) and `pct` (only when computable), `used` (bool), `saturated` (bool, a reading at ADC 4095 this minute), `reason` when not used: `no_baseline`, `settling`, `fault` (extra, D7) |
| `baseline` | object | `mq1RsOhm`, `mq2RsOhm` (ohm, only for a usable channel), `n` (records in the window), `mq1N`, `mq2N` (records used), `fromMs`, `toMs` (Unix ms) |
| `fullScaleRatio`, `warnPct`, `critPct` | number | constants used: 0.1, 40, 80 |
| `sinceMs` | number | Unix ms when the current state began |
| `silenced` | bool | Critical reminders silenced on the setup page |
| `test` | bool | only present, `true`, when the score was forced with Serial `A`: **ignore these records for scoring** |
| `foodRef` | object | record-only reference from the food's own minutes 30 to 45 after Start. `state`: `pending` (with `dueMs`), `taken`, `missed`. When `taken`: `mq1RsOhm`, `mq2RsOhm`, `n`, `mq1N`, `mq2N`, `fromMs`, `toMs` (as `baseline`), and `mq1Ratio` / `mq2Ratio` = this record's Rs mean / reference (when Rs is defined). No score or band is derived from it. Food-test mode only |

Without a baseline (`no_baseline`): `channels` with `used: false, reason: "no_baseline"` and `baseline` with the counts only.

### Run record `experiments/<experimentId>/runs/<runId>`
`deviceAlert`: `method`, `fullScaleRatio`, `warnPct`, `critPct`, `hysteresisPct` (10), `riseRecords` (3), `fallRecords` (5), `critRemindS` (600), `baselineWindowRecords` (15), `baselineMinRecords` (10), `baselineMaxAgeS` (1200), `rules` (text), `foodRefDelayS` (2700), `foodRefLateS` (3600), `foodRefRules` (text), and for food-test runs `baseline` and, once taken, `foodRef` (same shape as in the envelope).

### Event `experiments/<experimentId>/events/<runId>-alert-<n>`
```json
{"experimentId": "...", "runId": "R000nn", "sampleId": "...", "type": "device_alert",
 "observedAtMs": 1792601020000, "fromState": "normal", "toState": "warning", "scorePct": 52.3,
 "method": "rs_drop_log_v1", "mq1Ratio": 0.3, "mq2Ratio": 0.6,
 "notes": "On-board spoilage alert (rs_drop_log_v1) changed from normal to warning. Device verdict only, not an independent food observation."}
```
`test: true` is added for forced records. `observedAtMs` is the time of the record that changed the band. `n` counts per experiment from 1.

### Extra fields carried over from 1.0.0 and 1.1.0 (unchanged)
- Envelope: `timeSource`, `sample.free_heap`, `sample.unsent`, `sample.rejected`, `sample.gap_s`, per-channel `quality` and `faults`.
- Run record: `runStartReason`, `runStartUptimeS`, `runStartedAtMs`, `runEndReason`, `runEndUptimeS`, `runEndedAtMs`, `runIdAssignment`, channel `note`, `ppmFormula`, `pin`, `stabilisation.rules`, `wifiSource`, `wifiSsid`.
- Meta and events: `endedAtMs`, `startedVia`, `endedVia`, `notRecorded`, `methodDetail`, `recordedAtMs`, `source`, `change`, `powerScope`, `doorClosedAtMs`, `issuedAtMs`, `issuedBy`, `commandId`, `resetReason`.

## 4. Why these thresholds (defence)

- **Spoilt rice, R00062:** MQ1 fell to 0.032 of baseline. That scores 100% (Critical), and it crossed 50% of baseline within 2 minutes. MQ2 fell to 0.233, which scores 63% (Warning). The experiment score is the higher one, so this sample would have reached Critical 3 records after the fall.
- **Clean air, no food:** the largest single step seen within a run was a 28% fall on MQ1 alone (R00007, 17 Oct). That is a ratio of 0.72, which scores 14% (Normal). Slow drift was about 2% per hour, roughly 1 point of score per hour.
- **Bands:** Warning from 40% (Rs at 40% of baseline, a 2.5x fall), Critical from 80% (16% of baseline, a 6.3x fall). 3 records in a row to rise reject single-minute jumps like the unexplained MQ1 step in the rice test; 5 records and a 10-point hysteresis to fall stop flicker at a boundary.
- **Why a per-experiment baseline:** clean-air Rs moved by a factor of about 2 between days (calibration findings, 17 Oct), so one fixed R0 cannot work.
- **Limits:** these come from one spoilt sample and a few empty runs. They are starting values, not validated thresholds. They should be re-checked after a fresh-to-spoilt trial; every constant is a named value at the top of the sketch. MQ135 is not selective, and temperature and humidity are not measured, so a door opening or other food in the compartment can move the score too.
- **Settling:** with `SETTLE_S` 1800 and 15 min of CAL VALID for the baseline, the earliest start is 45 min after power-on, which is where the findings put the plateau (45 to 60 min).

## 5. Bench test (Part 8): steps for you to run

Results so far: step 1 done by you. Step 2 done by me (above). **Steps 3 to 8 have not been run**: they need the sensor board and LCD, the latch button and a phone, and about 2 hours. Run them on USB outside the enclosure, then again in the enclosure, and record every result.

Serial Monitor at 115200 baud, line ending **Newline**. Keep the Firebase console open on `telemetry/FRIDGE001/latest` and `experiments/`.

1. **Dumps before flashing.** Done (you saved `d` and `r`).
2. **Flash and counters.** Done: 1,329,280 bytes (63%), LittleFS 1,966,080 bytes, runs R00087/R00088 above R00086. **To check:** after you put the board back on the sensor board and power it, the boot banner shows `fridge_monitor 1.2.0`, a boot number above 117 and a run above R00088.
3. **Calibration mode.**
   - The LCD shows `CAL SETTL 0h29m`, counting down. At 25 min, 3 beeps and Serial "settling ends in 5 min - then wait 15 min for the baseline (CAL READY); keep the food OUT". At 30 min, 1 beep, Serial "settling period over ... keep the food OUT until CAL READY", and the LCD shows `CAL VALID`.
   - About 15 min later: **one long beep**, Serial `*** CAL READY ...`, LCD `CAL READY`, page Baseline row "CAL READY: put the food in now, then Start". No food goes in before this.
   - In Firebase, `latest` has `deviceAlert: {method: "rs_drop_log_v1", state: "off"}` and nothing else in it.
   - `?` shows `spoilage alert : off (calibration). Baseline if started now: ...`. Note the time `CAL VALID` first appears.
4. **No baseline.** Less than 10 minutes after `CAL VALID`, press the latch button, open the page and start an experiment (any food name, for example `bench test`).
   - The Start result says "Baseline not ready: wait until the LCD shows CAL VALID for 15 min before starting ...".
   - The status block shows Spoilage alert `no baseline` and the same reason in red.
   - Release the button. The LCD shows `NO BASELINE`.
   - `latest` has `deviceAlert.state: "no_baseline"`, channels with `used: false, reason: "no_baseline"`, and `baseline` with `mq1N`/`mq2N` below 10 and no `mq1RsOhm`.
   - `A 50` prints "this experiment has no baseline ... nothing to force".
5. **Baseline taken.** End the experiment (End tab). The LCD shows `EXP ENDED`, then `CAL VALID`, **not** `CAL READY` (the buffer was emptied at the end; `?` shows 0 valid records).
   - Wait for the long beep and `CAL READY` (about 15 min). `?` (or the page) must say `CAL READY` with mq1 and mq2 at 15.
   - Start again with a new experimentId. Serial shows `BASELINE taken: mq1 ... ohm from 15 records, mq2 ...`.
   - The Start result and the status block show both baselines, the record counts and the window time.
   - `?` and `x` show the same baseline. The LCD shows `FRESH`, then `FRESH n%` after the next record.
   - The run record `experiments/<id>/runs/<runId>/deviceAlert/baseline` holds the same values.
   - `?` shows `food reference : pending, due <start + 45 min> WAT`, and `latest` has `foodRef: {state: "pending", dueMs}`.
   - **Food reference:** don't do step 7 (gas check) or step 8 (EN) until the reference is taken. A swab or a reboot in minutes 30 to 45 would end up in it. About 45 min after Start, Serial shows `FOOD REFERENCE taken (record only, ...)`, `?` and the page show its values, `latest` has `foodRef.state: "taken"` with `mq1Ratio`/`mq2Ratio` close to 1, and the run record gets `deviceAlert/foodRef`. Steps 6's forced scores don't affect it (they change the score, not Rs).
6. **Forced Warning and Critical.**
   - Type `A 50`. Serial prints the TEST MODE warning, then `!! TEST: score forced to 50.0%` at each record.
   - At the 3rd forced record:
     - `ALERT BAND normal -> warning ...` on Serial;
     - 2 short beeps;
     - LCD `WARNING 50% TEST`;
     - LED 1 s on / 1 s off;
     - in Firebase, event `<runId>-alert-1` with `type: device_alert`, `fromState: normal`, `toState: warning`, `test: true`;
     - `latest` has `state: "warning"`, `scorePct: 50`, `test: true`.
   - Type `A 90`. At the 3rd record:
     - `normal`/`warning -> critical` on Serial;
     - 5 pulses;
     - LCD `SPOILT 90% TEST`;
     - LED 200 ms blink;
     - event `-alert-2` in Firebase.
   - **Reminder:** note the time of the 5 pulses. Type `A 90` again before the 10 forced records run out, so Critical lasts more than 10 min. 5 pulses again 10 min after entry (`ALERT: Critical reminder` on Serial).
   - **Silence:** press the latch button. The status block shows Spoilage alert `SPOILT 90% ... TEST` and the **Silence alarm** button. Press it. The page says "Alarm silenced ...". The button disappears. `?` shows `silenced`, and `latest` shows `silenced: true`. LCD and state stay Critical. No reminder 10 min later. Release the button.
   - **Fall:** stop forcing (let the forced records run out). With a real score near 0, after 5 records Serial shows `critical -> normal`, an event `-alert-3` appears and the LCD shows `FRESH n%`. The LED goes back to the short logging blink.
   - Type `a`: it lists the last 15 evaluations with TEST on the forced ones.
7. **Gas check (direction only).** Note `?`'s ratios. Hold an alcohol swab or a marker pen near both sensors for about 3 minutes.
   - The `REC` lines show Rs1/Rs2 falling and `AL=... n%` rising.
   - Record the lowest ratio of each channel from `?` or `a` (peak score = 100 x log10(ratio) / -1).
   - If it rises 3 records in a row above 40%, Warning sounds (a real event, no `test`).
   - Take the swab away and see the score fall back.
   - This is not a food result.
8. **Reboot during an experiment.** During the experiment (best right after step 6 while it is Warning or Critical, or after step 7), press **EN**.
   - Serial: `Spoilage alert: baseline kept, settling, last band ...`.
   - The LCD shows `HELD SPOILT` / `HELD WARNING` (or `FOOD SETTL 0h29m` if the band was Normal) for 30 min.
   - `latest`: `state: "settling"`, `band` the held band, channels `reason: "settling"`, no `scorePct`. A `device_restart` event appears as before.
   - After 30 min, scoring resumes against the **same** baseline (`?` shows the same ohm values). A held Critical that was not silenced gives its 5-pulse reminder as soon as scoring resumes. No new band event unless the band really changes.
   - `?` still shows the same food reference after the reboot, and `latest` keeps giving `foodRef` ratios once settling is over.
   - Finish by ending the experiment. `?` then shows `spoilage alert : off` and NVS has no baseline or food reference.

Repeat steps 3 to 8 in the enclosure. In the enclosure, also check that the beeps and the LED can be heard and seen through it.

## 6. Remaining risks

- The food reference has not run on the board yet. Bench step 5 is its first test.
- If the food goes in long before Start, or is warm at minute 30, the reference includes that. Its window times are uploaded so this can be checked against the storage events.
- The thresholds are starting values from one spoilt sample (section 4).
- MQ1 jumps (like the unexplained step in the rice test) longer than 3 minutes would still raise a band.
- The baseline buffer is RAM only and is emptied at every experiment end. After a reboot or an end, a new experiment needs 15 fresh minutes (CAL READY) again. This is intended (clean air of the current boot), but it means waiting.
- CAL READY only says the last 15 min were valid; it cannot tell whether food was already in. It works only if the food stays out until the beep.
- Rail readings put a floor under the computed Rs, so a saturated channel's score is a lower bound (`saturated: true`).
- Alert events made before the clock is set wait in the outbox until it is set. If that boot never gets a time, they go to the rejected file.
- The board still cannot read commands (`HTTP 401` on `control/FRIDGE001/command`). Experiments can only be started from the setup page until the app team adds that read rule.
