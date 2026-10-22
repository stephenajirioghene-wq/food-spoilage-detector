# Task brief: run food experiments from the setup page and from Firebase commands (fridge_monitor final firmware)

## Purpose

FRIDGE001 is sealed in its enclosure, so Serial is no longer practical. Today a new food test needs the Serial `e` command, so tests get logged in calibration mode and have to be relabelled afterwards (as happened with RICE-SPOILT-001 on 20 Oct 2026).

Extend the existing Wi-Fi setup page (latching button on GPIO 4, see `wifi_setup_brief.md`) so that a phone can do the whole food-test workflow:
1. start an experiment and enter the meal details;
2. add food observations;
3. log events (door opened, storage change, power cut);
4. end the experiment.

The board then writes all the records the app team's PDF asks for, with no PC and no Serial.

The app will also start and end experiments by sending commands through Firebase (Part 2b). The page and the Firebase commands must call the **same** start and end functions, with the same checks. Only the input differs.

Work in `fridge_monitor/fridge_monitor.ino`. Everything in `fridge_monitor_brief.md` and `wifi_setup_brief.md` still applies, except where this brief changes it. Those two briefs describe 1.0.0 as specified. The running 1.0.0 differs from them in two known ways: `SETTLE_S` is 3600 (1 h, not 3 h), and runId is not `R<bootCount>`. Keep 1 h settling. Do not modify `food_spoilage_test1_reboot_safe.ino`, `hw_selftest` or `baseline_logger`. Do not delete or move existing Firebase data.

This brief also covers three bugs found in the 15 to 20 Oct data (Part 3, 4a and 4b). The evidence is in the `evidence/` folder of this package.

**Change to the earlier brief:** `fridge_monitor_brief.md` said the firmware never writes `experiments/<experimentId>/meta` or `observations`. That rule is replaced. The board now writes them from what the user enters on the page. `experiment_tool.py` keeps working and can still be used from a PC.

## Part 1: the page

Keep the existing Wi-Fi section exactly as it is. Add an **Experiment** section below it, with tabs or buttons for the four actions. The page must still work on a phone screen with no internet resources.

At the top of the Experiment section, show the current state:
- mode (`calibration` or `food_test`), experimentId, sampleId, food name;
- runId, quality state and time left in settling;
- board time in WAT, and whether time is synced.

### 1a. Start experiment

Only available when no experiment is running. If one is running, show "End the current experiment first".

| Field | Required | Notes |
|---|---|---|
| Food name | yes | Free text, e.g. `cooked rice`. Up to 40 characters. |
| experimentId | yes | Pre-filled suggestion: food name in capitals, spaces to `-`, then `-YYYYMMDD-NN`, e.g. `COOKED-RICE-20261021-01`. NN counts up per day; keep the counter in NVS. User may edit. |
| sampleId | yes | Pre-filled suggestion: `<experimentId>-S01`. User may edit. |
| Storage location | yes | Free text, default `refrigerator, same compartment as the device`. |
| Container | no | Free text, e.g. `plastic bowl`. |
| Covered or open | no | Choice: covered, open, not recorded. |
| Portion | no | Number plus unit choice (g, kg, ml, pieces). |
| Cooked at | no | Date and time in WAT. Leave blank if unknown. |
| Stored at | no | Date and time in WAT when the sample entered storage. Default: blank. A "now" button fills in the board time. |
| Recipe or ingredients | no | Free text, up to 120 characters. |
| Prior storage or reheating | no | Free text, up to 120 characters. |
| Other food in the compartment | no | Free text. The PDF asks to record all dishes sharing the compartment. |
| Notes | no | Free text, up to 200 characters. |

Validation:
- IDs: only `A-Z`, `0-9` and `-`, 3 to 32 characters. Convert lower case to upper case. Reject anything else; Firebase keys cannot contain `. $ # [ ] /`.
- Refuse an experimentId already used on this board. Keep a list of used IDs in NVS or LittleFS.
- Optional fields left blank are omitted from the record, never filled with a default. Times entered in WAT are stored as Unix ms.
- Refuse to start if board time is not synced. Show "Waiting for time sync" instead. `startedAtMs` must be a real time.

On Start:
1. Store experimentId, sampleId and food name in NVS, so they survive power loss, as the existing `e` command does.
2. End the current run and start a **new runId without rebooting** (see "Run changes" below), with `runStartReason` `experiment_start`.
3. Write `experiments/<experimentId>/meta`. Use the field names and fixed values in section 3 of the PDF: schemaVersion 1, deviceId, category `cooked_meal`, preparation `cooked`, purpose `food_test`, status `running`, and `startedAtMs` = the time Start was applied.
4. Write an event `<runId>-experiment-start`, type `other`, with notes "Experiment started from the setup page".
5. Show a confirmation with the IDs, and the reminder: "Put the food in after settling ends. Then use Log event, Storage change."

### 1b. Add observation

Only available while an experiment is running.
- Label: unknown, fresh, spoiled or inconclusive (the PDF's four labels only).
- Method: choice of smell, visual, smell and visual, other (free text).
- Observed at: defaults to now; may be edited in WAT.
- Notes: free text, up to 200 characters.
- Writes `experiments/<experimentId>/observations/<sampleId>-obs-<epoch>` with experimentId, sampleId, observedAtMs, label, method and notes.
- The page must not suggest a label from the sensor readings. The PDF says observations must be independent of the sensor.

### 1c. Log event

Only available while an experiment is running.
- Type:
  - `storage_change` (food in, food out, moved);
  - `door_opened`;
  - `power_outage`, with a further choice: sensor power, refrigeration power, both, or unknown;
  - `sample_replaced`;
  - `other`.
- Time: defaults to now; may be edited in WAT. Add a "door closed at" time for `door_opened`.
- Notes: free text, up to 200 characters.
- Writes `experiments/<experimentId>/events/<runId>-<type>-<epoch>` with experimentId, runId, type, observedAtMs and notes. Add the extra fields where they apply, for example `powerScope`.
- `sample_replaced` must say on the page that the PDF requires a NEW experiment for a new sample. Offer to end the current experiment.

### 1d. End experiment

- Choice of final status: completed, interrupted or abandoned.
- Optional notes.
- Ask for confirmation.

On End:
1. Patch `experiments/<experimentId>/meta` with the status and `endedAtMs`.
2. Write an event `<runId>-experiment-end`.
3. Clear experimentId and sampleId in NVS and return to calibration mode.
4. Start a new runId without rebooting, with `runStartReason` `experiment_end`.

## Part 2: rules that apply to all four actions

### Run changes without reboot
- Starting or ending an experiment closes the current run and opens a new runId. Use the same counter as at boot, so runIds stay unique and increasing.
- The new run metadata goes to `experiments/<experimentId>/runs/<runId>` with `runStartReason` and `runStartUptimeS` set to the uptime at the change.
- Settling depends on **uptime since boot**, not on the run. A run started after the first hour is valid straight away, because the heaters are already warm.

### Offline
- Every record the page creates is written to LittleFS first and uploaded through the existing queue, like measurement records. It keeps its original times.
- If Wi-Fi is down, the page still works. Show "Saved on the board, will upload when online".

### Access and security
- Page actions only work while the setup portal is open, which needs the physical button and the setup network password.
- Uploads continue to use the board's own Firebase user. Note for the final handoff: the app team must allow that user to write `experiments/<experimentId>/meta`, `observations` and `events`, as well as `runs`.

### LCD, LED and buzzer
- After Start: `FOOD <experimentId>` on line 1 for 5 s (cut to 16 characters), then the normal display.
- After End: `EXP ENDED` for 5 s.
- One short beep on any successful action. Two short beeps if an action is refused.

## Part 2b: commands through Firebase (for the app)

The database already has `control/FRIDGE001/command`, which is currently an empty string. Ask Steve whether the app team has already fixed a format for this node. If they have, follow theirs and report any differences from this section. If not, use the format below and list it in the final notes for the app team.

### Command format

The app writes an object to `control/FRIDGE001/command`:

| Field | Notes |
|---|---|
| `commandId` | Unique string from the app, e.g. `CMD-1792500000000`. Same character rules as the IDs. |
| `type` | `start_experiment` or `end_experiment` |
| `issuedAtMs` | When the app sent it (Unix ms) |
| `issuedBy` | Free text, e.g. `app` |
| `payload` | `start_experiment`: the same fields as the Start form in 1a, with the same names as the meta record (experimentId, sampleId, food name, storage location and the optional fields). `end_experiment`: status and notes. |

Observations and events do not need the board. The app can write them straight to `experiments/<experimentId>/observations` and `events`, so there are no commands for them.

### Board behaviour

1. Read `control/FRIDGE001/command` every 60 s, after the latest upload.
2. Ignore an empty string or a missing node.
3. Ignore a `commandId` the board has already handled. Keep the last 20 commandIds in NVS.
4. Run the same checks as the page:
   - IDs valid and not used before;
   - time synced;
   - no start while an experiment is running;
   - no end when none is running.
5. If the checks pass, apply the command with the same functions as the page. That means the new runId, meta, the start or end event, the LCD message and the beep. Set the event notes to "Experiment started by Firebase command <commandId>" (or ended).
6. Write an acknowledgement to `control/FRIDGE001/ack/<commandId>`:
   - `status`: `applied` or `rejected`
   - `reason`: when rejected
   - `appliedAtMs`, `experimentId`, `runId`
7. Then set `control/FRIDGE001/command` back to an empty string. Write the acknowledgement before clearing, so a command is never lost without a record.
8. The experiment's `startedAtMs` is the time the board applied the command, not `issuedAtMs`. Record both in the start event, so a delay while the board was offline is visible.

### Page and app together

- If the page and the app both try to start an experiment, whichever reaches the board first wins and the other is refused with a reason.
- The page shows the last command handled (commandId, type, status, time), so the user can see whether the app changed anything.

### Access

Note for the final handoff: the board's Firebase user needs to read `control/FRIDGE001/command`, write it back to an empty string, and write `control/FRIDGE001/ack`. The app needs to write `command` and read `ack`. The app team sets these rules.

### Serial
Keep `e` and `x` working for bench use. `e` must follow the same rules as the page: refuse while an experiment is running, start a new run, and write meta with the food name only.

## Part 3: fix the stuck upload backlog

In run R00062 (19 Oct 22:47 to 20 Oct 02:08) the `unsent` count stayed between 106 and 115 the whole time, while live and history uploads were succeeding. It looks as though about 106 older records are retried and never accepted. The per-minute unsent count is in `evidence/unsent_backlog_R00062.csv`. Steve will also supply a Serial `d` dump taken before reflashing; use it to identify the stuck records.
1. Find which records they are, and why the upload fails. Check the path, the time (0 or not rebuilt), the HTTP status and the rules.
2. Records that can still be uploaded: upload them.
3. Records rejected permanently (for example HTTP 400, or a path the rules will never accept): move them to a separate `rejected` file in LittleFS. Do not retry them forever. Log the reason on Serial. Add a `rejected` count next to `unsent` in each envelope.
4. Never delete data. A Serial command must still dump the rejected file.

## Part 4: fix reused run IDs and boot times out of order

### 4a. Run IDs were reused, and old data was overwritten

The evidence is in `evidence/run_id_reuse_R00013.json`:
- The run counter and boot counter started again from 1 at about 03:39 WAT on 16 Oct (`CAL-R00001`, bootCount 1). That was probably a reflash.
- Runs R00013, R00014 and R00016 already existed from 15 to 16 Oct.
- On 19 Oct the new sequence reached R00013 again. `telemetry/FRIDGE001/runs/R00013/samples` now mixes two runs:
  - uptime 60 and 120 s are from 19 Oct, and overwrote the 15 Oct records with the same measurementIds;
  - uptime 180 s onward are from 15 Oct.
- `experiments/CAL-R00013/runs/R00013` and `CAL-R00015` were also overwritten by the 19 Oct boots.

Also, runId no longer equals bootCount, as `fridge_monitor_brief.md` asked. For example R00062 is bootCount 88, so the two counters are separate.

Requirements:
1. A runId must **never** repeat on this device, including after a reflash, a partition change or an NVS erase.
2. Before the first write of a new run, check Firebase. Use a shallow GET of `telemetry/FRIDGE001/runs/<runId>` and of the run record. If either already exists from another boot, move to the next free runId and log it on Serial.
3. Never overwrite an existing sample or run record that belongs to a different run. Use the REST ETag / `if-match` support or an equivalent check if needed.
4. Explain in the final notes how runId is now assigned, why the counters reset on 16 Oct, and whether runId should equal bootCount. If not, record both in the run metadata, as now.
5. Do not repair the old R00013 data in Firebase. Report it only.

### 4b. Boot times out of order

In the export, R00063 (bootCount 89) has a boot time of 02:10:25 on 20 Oct, but R00064 (bootCount 90) has 02:09:00. A later boot cannot start earlier. The evidence is in `evidence/boot_order_bug_run_records.json`.
1. Find where `bootEpochMs` is calculated and when it is written, including for boots that synced time late or not at all.
2. Make sure `bootEpochMs` and any rebuilt sample time only ever come from a time sync in **the same boot**, never from a value stored by an earlier boot.
3. Explain in the final notes what caused it.

## Test procedure (for the final notes)

1. Flash. Let it boot and sync time. Press the button and join the setup network.
2. Try Start with a lower-case ID containing a dot. It is rejected or converted as described.
3. Start an experiment with only the required fields. Check in Firebase:
   - `meta` holds only the fields entered plus the fixed values;
   - a new runId with `runStartReason` `experiment_start`;
   - envelopes now show `mode` `food_test` with the new IDs.
4. Try Start again. It is refused.
5. Add an observation and a `door_opened` event. Check both in Firebase with the right times.
6. Turn the hotspot off, log an event, and turn the hotspot back on. The event uploads with its original time.
7. Cut the power for a minute. After the reboot the IDs are kept and a new runId appears. Settling starts again because uptime reset.
8. End the experiment as completed. Check that meta shows `completed` and `endedAtMs`, mode is back to `calibration`, and a new runId has `runStartReason` `experiment_end`.
9. In the Firebase console, write a `start_experiment` command by hand. Within about a minute:
   - the acknowledgement shows `applied`;
   - the command node is back to an empty string;
   - a new experiment with meta and a new runId exists.
   Write the same command again with the same commandId. It is ignored. Write an `end_experiment` command. It ends the experiment.
10. With an experiment running from the page, send a `start_experiment` command. The acknowledgement shows `rejected` with the reason.
11. Check that `unsent` falls to 0, or that the remaining records sit in `rejected` with a reason.
12. Check across several reboots that `bootEpochMs` increases with bootCount.
13. Run-ID reuse check:
    - in the Firebase console, create an empty placeholder at `telemetry/FRIDGE001/runs/<the next runId>`;
    - reboot;
    - the board must skip to the following runId and leave the placeholder untouched;
    - then delete the placeholder by hand.

## Before you finish

- Compile for ESP32 Dev Module on esp32 core 3.3.12, as before.
- Bump `firmwareVersion` to `1.1.0` and say what changed.
- List anything you had to assume or could not follow, and anything in this brief that conflicts with the app team's PDF.
