# FRIDGE001 firmware 1.1.0: notes

Changed or new files:
- `fridge_monitor/fridge_monitor.ino`: firmware 1.1.0.
- `fridge_monitor/setup_page.html`: the setup page source. The Wi-Fi section is unchanged and the Experiment section is new.
- `fridge_monitor/setup_page.h`: generated from the page. Do not edit it.
- `make_setup_page.py`: run `python make_setup_page.py` after any edit to `setup_page.html`.

Not modified: `food_spoilage_test1_reboot_safe.ino`, `hw_selftest`, `baseline_logger`, `secrets.h` (neither copy), `experiment_tool.py`. The 1.0.0 sketch is still in `fridge_monitor_v1.0.0_backup/`.

The firmware never sends DELETE. The only Firebase value it replaces that is not its own is `control/FRIDGE001/command`, which it sets to `""` after a command (see section 5).

## 1. What changed in 1.1.0

- `firmwareVersion` is `1.1.0`.
- **Settling is now 30 min** (`SETTLE_S` 1800), changed at Steve's request on 20 Oct 2026. Brief 03 said to keep 1 h. See the recommendations.
  - The warning beeps now come 5 min before the 30 min ends.
  - Run metadata reports `settleS: 1800`.
- **`hardware.temperatureHumidity` removed** from the run metadata (brief 01: no temperature or humidity fields at all).
  - Runs already in Firebase keep the field. The board never deletes fields.
  - That includes runs up to R00076 made by the first 1.1.0 builds.
- **Experiment section on the setup page.** It shows the state, then has four tabs: Start, Observation, Event, End. They work only while the portal is open, which needs the latching button and the setup password.
- **Firebase commands.** The board reads `control/FRIDGE001/command` every 60 s, right after the latest upload. It handles `start_experiment` and `end_experiment`, writes an ack to `control/FRIDGE001/ack/<commandId>`, then clears the command.
- **Serial.**
  - **`e` and `E` no longer start or end experiments** (Steve's decision, 20 Oct 2026; brief 03 asked to keep `e`). The board is sealed, so experiments are run only from the setup page or by app command. `e` and `E` now just print that. `?`, `u`, `d`, `x`, `w` and `W` are unchanged.
  - `r` dumps the rejected file only.
  - `d` now also dumps the outbox and the rejected file.
- **One set of functions.** The page and the commands call the same `startExperiment()` and `endExperiment()`, with the same checks.
- **New run without a reboot.** Starting or ending an experiment opens a new runId from the same counter (`runStartReason` `experiment_start` or `experiment_end`). The old run gets `runEndReason`, `runEndUptimeS` and `runEndedAtMs`. Settling follows uptime since boot, so it does not restart.
- **Outbox.** Everything the board creates (meta, observations, events) goes to `/outbox.jsonl` first. It uploads with the history, keeping its original times. The Wi-Fi-change event also goes there now, so a reboot no longer loses it.
- **LCD and buzzer.**
  - `FOOD <experimentId>` (cut to 16 characters) or `EXP ENDED` on line 1 for 5 s.
  - One beep when an action succeeds, two when it is refused.
- **The three bugs** are fixed (section 3).
- **Envelope.** `sample.rejected` sits next to `sample.unsent`.
- **Run metadata.** A `runIdAssignment` text explains how runIds are assigned.

## 2. Conflicts and decisions (none resolved silently)

### Brief 03 against the app team's PDF
1. **Serial `e` "writes meta with the food name only".** The PDF requires `storage.location` in every meta record. This no longer applies: Serial `e` was removed at Steve's request, and the page and commands always require a storage location.
2. **"Note the uncertainty" (PDF section 3).** The brief says to leave blank optional fields out. The PDF also asks to note when a time or measurement is unknown. Blank fields are still left out. Meta also gets a `notRecorded` list naming them (for example `["food.cookedAtMs","storage.cover"]`). Nothing is filled with a default.
3. **New board read access.** The PDF only covers write access. Brief 03 makes the board read `telemetry/FRIDGE001/runs` and the run records (runId check) and the command node. It also reads `experiments/<id>/meta/startedAtMs` (my addition, item 9 below). The app team must allow these reads (section 6).

No field name or allowed value in brief 03 contradicts the PDF. `endedAtMs`, `startedVia`, `endedVia`, `notRecorded`, `methodDetail`, `recordedAtMs`, `source`, `change`, `powerScope`, `doorClosedAtMs`, `issuedAtMs`, `issuedBy` and `commandId` are extra fields the PDF does not mention.

### Brief 03 against your rules, or against itself
4. **"Set `control/FRIDGE001/command` back to an empty string"** overwrites the app's command object. That sits close to your rule "never write code that deletes Firebase data", so the board is careful:
   - it first writes the ack with a full copy of the command (`ack/<commandId>/command`);
   - it clears the node only with `if-match` on the ETag it read, so a newer command the app wrote in between is never wiped (HTTP 412, read again next time).

   **Steve accepted this on 20 Oct 2026.**
5. **A duplicate commandId is "ignored"** exactly: no new ack, and the node is not cleared. It stays until the app writes the next command.
6. **Commands wait for a time sync in this boot** before they are read. Otherwise a start sent just after a reboot would be rejected with "Waiting for time sync". The time-sync check still runs (it is part of `startExperiment`).
7. **Uploads still pause while the portal is open** (brief 02). The page says "Saved on the board. It uploads when this setup page closes." while Wi-Fi is up, and "Saved on the board, will upload when online." while it is down. Commands are also not read while the portal is open.
8. **Observations and events need a time sync in this boot**, the same rule as Start, so that "now" is a real time.
9. **Meta is never overwritten.**
   - The Start meta is a PUT, so it holds only the entered and fixed fields (test step 3).
   - Before sending it, the board reads `experiments/<id>/meta/startedAtMs`. If another experiment already uses that ID in Firebase (for example one made with `experiment_tool.py`), the meta goes to the rejected file instead.
   - The End PATCH is sent only if `startedAtMs` matches the one this board wrote.
10. **Observation and event keys** use the board time when the record was saved, made unique per second. They do not use the entered time, so two observations entered for the same minute cannot overwrite each other.
11. **experimentId suggestion.**
    - The food name is upper-cased, spaces become `-`, other characters are dropped, and it is cut to 20 characters.
    - Then comes `-YYYYMMDD-NN` (WAT date). NN counts starts per day in NVS.
    - The sampleId suggestion is the experimentId cut to 28 characters plus `-S01`.
12. **Reserved IDs.** `CAL-...` and `NONE` are refused as experimentId or sampleId, because calibration mode uses them.
13. **commandId case.** The board checks it with the ID rules, ignoring case. The ack goes under the commandId exactly as sent. An invalid or missing commandId is acked under `ack/INVALID-<epoch>`.
14. **Ack fields.** The ack has `appliedAtMs` for rejected commands too (the time it was handled). `experimentId` and `runId` are the board's state after handling.
15. **Observation method.** It is stored as `smell`, `visual`, `smell_and_visual` or `other`. For `other` the free text goes in `methodDetail`.
16. **The test procedure has 13 steps** in brief 03. `context/FRIDGE001_firmware_vs_requirements.docx` says 12.
17. **Program size.** See section 8.

### Differences between 1.0.0 and briefs 01 and 02 (reported before any change, still true in 1.1.0)
- **Kept as before:**
  - `SETTLE_S` 3600.
  - 7-digit measurementIds.
  - `CAL-<runId>` and sampleId `NONE` in calibration mode.
  - Segment files and pruning.
  - `saturation` only at 4095.
  - The brief-02 LCD extras.
  - Uploads paused for the whole portal session.
- **Still against brief 01, kept:** the 3 beeps 5 minutes before settling ends. Brief 01 wants the buzzer silent except for one beep when settling ends.
- **Removed in 1.1.0 (Steve's decision):** `hardware.temperatureHumidity: "not measured"`. See the recommendations.
- **Fixed in 1.1.0:**
  - The time-sync and unsent-count bugs (section 3).
  - `firmwareVersion` was never bumped.
  - The Wi-Fi-change event being lost on a reboot before the clock was set.

## 3. The three bugs

### Part 3: backlog stuck at about 106
- **Which records:** from `littlefs_dump_before_1.1.0.txt`, R00002 (99 records, boot 2) and R00006 (6 records, boot 6). Every record is `epoch: 0` and neither run has a `bootEpochMs`. That is 105 records, and `unsent` = 105 + the current record = 106. That is exactly the floor in `unsent_backlog_R00062.csv`.
- **Why they never went:** no HTTP request ever failed.
  - In 1.0.0, `uploadRun()` correctly refused to upload a run without a real time (`if (!r.bootEpochMs || !r.unsent) return true;`).
  - But it left those records counted in `unsent`, and `scanRun()` counted them again at every boot.
  - So the path, the rules and the HTTP status were never involved. The time was 0 and could never be rebuilt, because the boot that made them never synced.
- **Fix:**
  - At boot, any earlier run with no `bootEpochMs` has its run metadata and every record copied to `/rejected.jsonl`. Each line carries the reason "time never synced in boot N; no real epoch can be given". Only then are the original files removed.
  - The rejected file is never deleted. `d` and `r` dump it.
  - Each envelope carries `sample.rejected`.
  - In the dump, 21 more runs had no time but no records (R00009, R00011, R00014, ...). Only their run metadata lines go to the rejected file, and they don't count as records.
- **Also fixed:** a history chunk that gets HTTP 400 is retried one record at a time. The record Firebase refuses goes to the rejected file with the reason instead of being retried forever. An outbox record that gets 400 is handled the same way.
- **Runs with `epoch: 0` records that did sync** (R00045 and others) are not touched. Their times were rebuilt from their own boot's sync and they are already uploaded.
- **After flashing,** expect 105 rejected records, `unsent` falling to 0, and `rejected: 105` in the envelopes.

### Part 4a: runIds reused
- **Why the counters reset on 16 Oct.** At boot 1 on 16 Oct (03:39 WAT) four things were empty at once:
  - the NVS boot counter;
  - the NVS run counter;
  - LittleFS, so there was no run on flash to count above;
  - baseline_logger's NVS counter.

  Only a full flash erase clears all four. The likely cause is that the upload ran with **"Erase All Flash Before Sketch Upload: Enabled"**, or that `erase_flash` was run. A partition-scheme change alone reformats LittleFS but keeps NVS. The firmware cannot tell which it was.
- **What was overwritten:** samples at uptime 60 and 120 s under `telemetry/FRIDGE001/runs/R00013/samples`, and the run records `experiments/CAL-R00013/runs/R00013` and `CAL-R00015`. **Not repaired**, as the brief asks.
- **How runId is assigned now:**
  1. It starts from the NVS run counter. The counter is kept above baseline_logger's boot count and every run on flash, as in 1.0.0.
  2. Before the run's first write anywhere in Firebase (`latest`, run record, samples or events), `verifyRun()` runs:
     - a shallow GET of `telemetry/FRIDGE001/runs` (once per boot) finds the highest runId already there, and a new run of this boot starts above it;
     - a shallow GET of `telemetry/FRIDGE001/runs/<runId>` and a GET of `experiments/<experimentId>/runs/<runId>` must both return `null`, or it moves to the next number and logs it on Serial;
     - the run record's ETag is kept, and the first run-record write is a PUT with `if-match`, so the record cannot be taken in between (HTTP 412 picks a new runId).
  3. The records on flash don't contain the runId. The runId is added at upload time, so moving to another number changes nothing on flash.
  4. If the check can't be done (offline, or reads not allowed yet), that run's data waits on the board. Nothing is written under an unchecked runId.
- **runId and bootCount.** Should runId equal bootCount? No.
  - bootCount is incremented at the very start of `setup()`. The run counter moves only when the run starts, after LittleFS is mounted and scanned (several seconds with 1,300 records).
  - Brownouts during that window used a bootCount but no runId. That is why R00062 is bootCount 88: 26 boots died before their run started.
  - Experiments now also open new runs within one boot.
  - Both stay in the run metadata (`runId`, `bootCount`).

### Part 4b: boot times out of order
- **Where it was calculated:** `checkTimeSync()` set `bootEpochMs = now - millis()` the first time the clock read 2023 or later. It ran right after every Wi-Fi connect, and also in the portal loop.
- **What went wrong:**
  - After a soft reset (TASK_WDT, PANIC, SW) the ESP32 keeps the clock from the previous boot. The RTC slow clock carries it across the reset, and that clock drifts.
  - R00063 was a TASK_WDT reset after R00062's 3 h 20 min run. Its "sync" came 20.05 s after boot, which was simply the Wi-Fi connect. Its clock was about 85 s fast, so it got 02:10:25 while the next real boot (R00064) synced by NTP at 02:09:00.
  - R00063 had no records, so no sample time was affected. R00017 (PANIC) may carry a few seconds of the same error.
- **Fix:**
  - `setup()` sets the clock to 0.
  - Only the SNTP sync callback of this boot (`sntpSynced`) counts as a sync.
  - `bootEpochMs` and every rebuilt time come only from that sync. A run of another boot never takes it.
  - Serial warns if a boot time comes out earlier than an earlier run's.

## 4. Command format (for the app team)

No app-team format was in `requirements/`, so this is brief 03's format. The board writes `ack/<commandId>`.

```json
{"commandId": "CMD-1792500000000", "type": "start_experiment", "issuedAtMs": 1792500000000, "issuedBy": "app",
 "payload": {
   "experimentId": "COOKED-RICE-20261021-01", "sampleId": "COOKED-RICE-20261021-01-S01",
   "food": {"name": "cooked rice", "cookedAtMs": 1792490000000, "recipe": "...",
            "portion": {"amount": 250, "unit": "g"}, "priorStorage": "..."},
   "storedAtMs": 1792495000000,
   "storage": {"location": "refrigerator, same compartment as the device", "container": "plastic bowl",
               "cover": "open", "otherFood": "..."},
   "notes": "..."}}
```

`end_experiment`: `"payload": {"status": "completed" | "interrupted" | "abandoned", "notes": "..."}`.

**Rules:**
- IDs: `A-Z 0-9 -`, 3 to 32 characters. Lower case is accepted and converted, except in commandId (see 2.13).
- Required: `food.name`, `storage.location`, experimentId, sampleId. Everything else is optional.
- Times are Unix ms and must not be in the future.
- `cover`: `covered` or `open`.
- `portion.unit`: `g`, `kg`, `ml` or `pieces`.

**Ack:**
```json
{"commandId", "type", "status": "applied" | "rejected", "reason", "appliedAtMs", "experimentId", "runId",
 "command": {the full command as read}}
```

The experiment's `startedAtMs` is the time the board applied the command. The start event records `commandId`, `issuedAtMs` and `issuedBy`, so a delay while the board was offline is visible.

## 5. RTDB paths (handoff)

| Path | By | Method | Content |
|---|---|---|---|
| `telemetry/FRIDGE001/latest` | board | PUT, every 60 s | newest envelope |
| `telemetry/FRIDGE001/runs/<runId>/samples/<measurementId>` | board | PATCH, batches of 10 | every envelope |
| `experiments/<experimentId>/runs/<runId>` | board | PUT if-match once, then PATCH | run metadata |
| `experiments/<experimentId>/meta` | board | PUT at start, PATCH `status`/`endedAtMs` at end | experiment metadata |
| `experiments/<experimentId>/observations/<sampleId>-obs-<epoch>` | board (page) or app | PUT | observation |
| `experiments/<experimentId>/events/<runId>-restart` | board | PUT | `device_restart` |
| `experiments/<experimentId>/events/<runId>-experiment-start` / `-experiment-end` | board | PUT | type `other` |
| `experiments/<experimentId>/events/<runId>-<type>-<epoch>` | board (page) or app | PUT | storage_change, door_opened, power_outage, sample_replaced, other |
| `experiments/<experimentId>/events/<runId>-wifi-<epoch>` | board | PUT | Wi-Fi changed during a trial |
| `control/FRIDGE001/command` | app writes; board reads, then sets `""` | GET, PUT if-match | command |
| `control/FRIDGE001/ack/<commandId>` | board | PUT | acknowledgement |

**Reads by the board:**
- shallow `telemetry/FRIDGE001/runs` and `telemetry/FRIDGE001/runs/<runId>`;
- `experiments/<experimentId>/runs/<runId>`;
- `experiments/<experimentId>/meta/startedAtMs`;
- `control/FRIDGE001/command`.

## 6. Firebase rules the app team must add (for the producer UID)

**Writes** (the 1.0.0 ones, plus the new ones):
1. `telemetry/FRIDGE001/latest` and `telemetry/FRIDGE001/runs/$runId/samples`.
2. `experiments/$experimentId/runs/$runId`.
3. `experiments/$experimentId/meta` (new).
4. `experiments/$experimentId/observations/$id` (new).
5. `experiments/$experimentId/events/$id`.
6. `control/FRIDGE001/command`, only to set it to `""` (new).
7. `control/FRIDGE001/ack/$commandId` (new).

**Reads** (new in 1.1.0):

8. `telemetry/FRIDGE001/runs`, with the shallow query.
9. `experiments/$experimentId/runs/$runId`.
10. `experiments/$experimentId/meta`.
11. `control/FRIDGE001/command`.

**For the app's user:** write `control/FRIDGE001/command`, read `control/FRIDGE001/ack`. Writing observations and events directly is optional.

**On the board today:**
- Reads 8 and 9 already work: the runId check passed on the board.
- Read 11 is **missing**: the command read gets HTTP 401.
- Reads 8 and 9 must stay allowed. Without them, 1.1.0 keeps every new run's data on the board. The PDF also asks you to confirm write access before using the paths.

## 7. Test procedure

Compile for ESP32 Dev Module, esp32 core 3.3.12, default partition scheme, **"Erase All Flash" disabled**.

1. Flash. Let it boot and sync time.
   - Serial shows `REJECTED R00002: 99 records ...` and `REJECTED R00006: 6 records ...` (first boot only), then `runId check: highest runId in Firebase is R000xx`.
   - Press the button and join the setup network.
2. On Start, enter experimentId `cooked.rice-1`. It is rejected ("only A-Z, 0-9 and -") with two beeps. Enter `cooked-rice-test-01`: it is accepted as `COOKED-RICE-TEST-01`.
3. Start an experiment with only the required fields. Release the button. In Firebase:
   - `meta` holds only schemaVersion, experimentId, deviceId, sampleId, food {name, category, preparation}, startedAtMs, purpose, status `running`, storage.location, `notRecorded` and `startedVia`;
   - a new runId with `runStartReason` `experiment_start`;
   - the envelopes show `mode` `food_test` with the new IDs;
   - the LCD showed `FOOD <id>` for 5 s, with one beep.
4. Try Start again. It shows "End the current experiment first".
5. Add an observation and a `door_opened` event with a door-closed time. Check both in Firebase with the right times (WAT entered, Unix ms stored).
6. Turn the hotspot off and log an event. The page says "Saved on the board, will upload when online". Turn the hotspot back on and release the button. The event uploads with its original `observedAtMs`.
7. Cut the power for a minute. After the reboot:
   - `x` shows the same experimentId and sampleId with a new runId;
   - a `<runId>-restart` event appears;
   - the LCD shows `FOOD SETTL 0h29m`.
8. End the experiment as completed:
   - meta shows `completed` and `endedAtMs`;
   - mode is back to `calibration`;
   - a new runId has `runStartReason` `experiment_end`;
   - the LCD shows `EXP ENDED`.
9. In the Firebase console, write a `start_experiment` command by hand (format in section 4). Within about a minute:
   - `ack/<commandId>` shows `applied`;
   - `command` is `""`;
   - a new experiment with meta and a new runId exists.

   Write the same command again: it is ignored, with no new ack. Write an `end_experiment` command: it ends the experiment.
10. Start an experiment from the page, release the button, and send a `start_experiment` command. The ack shows `rejected` with "End the current experiment first (...)".
11. `?` shows `unsent` falling to 0, `rejected: 105`, and the envelopes carry `rejected: 105`. `r` dumps them.
12. Reboot several times, including a power cut and a reflash (a software reset). Check that `bootEpochMs` increases with `bootCount`.
13. Run-ID reuse check:
    - note the next runId (`x` shows the current one; the next is +1);
    - in the console, create a placeholder at `telemetry/FRIDGE001/runs/<next runId>`, for example the value `true` (RTDB cannot store an empty node);
    - reboot;
    - Serial shows `runId check: R000nn already exists in Firebase (samples) - not reused`, the board uses the following runId, and the placeholder is untouched;
    - then delete the placeholder by hand.

## 8. Recommendations

1. **Temperature and humidity.**
   - The board has no temperature or humidity sensor, and the run metadata no longer says so. Brief 01 forbids such fields.
   - Tell the app team in the handoff that FRIDGE001 does **not** measure temperature or humidity. The PDF (section 1) asks for them only if measured.
   - MQ135 readings depend strongly on both, and a fridge swings in both when the door opens and the compressor cycles.
   - So consider adding a small digital sensor (SHT31 or BME280 on the existing I2C bus, pins 16/17) in a later firmware. Its readings would then be real measurements the PDF allows.
2. **Settling of 30 min.**
   - `calibration_data_findings_2026-10-17.md` found the steep heater rise ends 10 to 15 min after power-on. It also found Rs is "within a few percent of its plateau by 45 to 60 minutes", and called 60 min defensible.
   - At 30 min, Rs may still be drifting a few percent. Readings marked `valid` at 30 to 60 min may include warm-up drift.
   - The app scores food against the median Rs of the 15 min before the food goes in. So with 30 min settling, put the food in at 45 min or later, or check in the data that Rs is flat over that window.
   - Tell the app team that `settleS` changed to 1800 on 20 Oct 2026. Runs from R00077 on use it, and earlier 1.1.0 runs used 3600.
3. **Serial on a sealed board.** Experiments can no longer be started or ended over Serial. If the setup page ever stops working, use an app command, or reflash. `x`, `?`, `d` and `r` remain for diagnosis on the bench.

## 9. Verification done and not done

- **Compile:** ESP32 Dev Module, esp32 core 3.3.12, ArduinoJson 7.4.3, `--warnings all`. No warnings from the sketch.
  - Program: **1,305,628 bytes (99.6%)** of the default 1.25 MB app partition. About 5 KB is left (1.0.0 was 1,255,560 bytes, 95%).
  - The current build (30 min settling, no temperatureHumidity, no Serial `e`/`E`) was flashed on 20 Oct 2026 at about 17:32 WAT. On boot 105 (R00079), Serial showed `settling 30 min`, and `e` only points to the setup page.
  - The page is served gzipped: 5.4 KB instead of 16.6 KB.
  - Any further feature needs a bigger app partition. Changing the scheme reformats LittleFS, so dump with `d` first.
- **Flashed to the board on COM14 (20 Oct 2026, ~03:50 WAT)** with Erase All Flash disabled. Seen on Serial over two boots:
  - `REJECTED R00002: 99 records` and `REJECTED R00006: 6 records` on the first boot. 105 rejected, and `unsent (all)` went to 0.
  - `runId check: highest runId in Firebase is R00069`, then R00070 on the next boot. The new runs R00070 and R00071 were accepted.
    - So the board **can** already read `telemetry/FRIDGE001/runs` and the run records.
    - R00069 was a 1.0.0 run after your dump. Its 5 unsent records uploaded.
  - NTP set the clock through the new SNTP callback. Boot 96 `bootEpochMs` 1792464599779 < boot 97 1792464959095.
  - `latest` PUT OK every minute. History upload OK.
  - `GET control/FRIDGE001/command -> HTTP 401 Permission denied`: **the command read rule is missing.** Firebase commands cannot work until the app team adds it.
- **Not exercised yet:**
  - the setup page on a phone (start, observation, event, end);
  - the outbox upload of meta, observations and events (needs write rules for `meta`, `observations` and `events`);
  - commands with ack and clear;
  - the runId skip with a placeholder (test step 13).

  Run the 13-step test.
- **Remaining risks:**
  - A rejected-file move interrupted by a power cut would copy those records twice on the next boot. Nothing is lost.
  - A run record that exists under a *different* experimentId with the same runId and has no samples is not seen by the runId check. The telemetry check covers every run that uploaded samples.
