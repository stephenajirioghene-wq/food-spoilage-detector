# FRIDGE001 monitor firmware: notes

Deliverables:
- `fridge_monitor/fridge_monitor.ino`, with a copy of `secrets.h`.
- `experiment_tool.py`.

`baseline_logger`, `hw_selftest`, `lcd_test` and the main firmware were not modified. Nothing in either deliverable deletes Firebase data. The firmware only uses PUT and PATCH. The tool only uses PATCH, and PUT to keys it has just checked are empty.

## 1. Create the device user and read the producer UID

1. Open the Firebase console for **food-spoilage-7b60a** and go to **Authentication â†’ Sign-in method**. Enable **Email/Password**.
2. Go to **Authentication â†’ Users â†’ Add user**. Give it a dedicated address (for example a `fridge001@â€¦` address you control) and a long random password. Don't use your own account.
3. Put the address and password into `secrets.h` as `DEVICE_EMAIL` and `DEVICE_PASSWORD`. Copy the file to `fridge_monitor/secrets.h` as well. Never send either value to anyone.
4. Flash `fridge_monitor` and open the Serial Monitor at 115200 baud, with the line ending set to **Newline**. At the first successful sign-in after each boot it prints:
   ```
     Producer UID: <28-character UID>
   ```
   You can also type `x` at any time. The UID also appears in the console under Authentication â†’ Users (the "User UID" column).
5. Send that UID to the app team. It's an identifier, not a secret. Never send the password, the API key or a service-account key.

`experiment_tool.py` signs in as the same user, so its writes are covered by the same rules.

## 2. Write rules the app team must add before uploads work

The app team's prepared read-rules patch does **not** grant these writes. Until they add write rules for the producer UID on

- `telemetry/FRIDGE001` (`latest` and `runs/â€¦`), and
- `experiments` (`<experimentId>/runs`, `events`, `meta`, `observations`),

every upload fails with HTTP 401 "Permission denied". The board keeps everything in LittleFS and retries:

- `latest` every minute;
- history, run metadata and events every 10 minutes.

Once the rules exist, the backlog uploads by itself. While writes are refused, `?` shows `writes: DENIED by database rules`.

The PDF asks you to *confirm* write access before using the new paths. The board can't check that for you. If your current rules happen to let any signed-in user write there, uploads would start as soon as you flash. Wait for the app team's confirmation before starting a real cooked-meal trial.

## 3. Exact RTDB paths (for the handoff to the app team)

| Path | Written by | Method | Content |
|---|---|---|---|
| `telemetry/FRIDGE001/latest` | board | PUT, every 60 s | newest complete measurement envelope (only the record just made) |
| `telemetry/FRIDGE001/runs/<runId>/samples/<measurementId>` | board | PATCH, 10 envelopes per request, every 10 min and on `u` | every envelope (history), including backfilled ones |
| `experiments/<experimentId>/runs/<runId>` | board | PATCH | run metadata, sent at the first upload and again when the clock is set |
| `experiments/<experimentId>/events/<runId>-restart` | board | PUT | `device_restart` event, food_test runs only |
| `experiments/<experimentId>/meta` | experiment_tool.py | PATCH | experiment metadata (section 3 of the spec) |
| `experiments/<experimentId>/observations/OBS-<unixMs>` | experiment_tool.py | PUT (new key) | independent food observation |
| `experiments/<experimentId>/events/EVT-<unixMs>` | experiment_tool.py | PUT (new key) | power_outage, storage_change, sample_replaced, door_opened, other |

Identifier formats:
- **runId**: `R` + 5 digits, e.g. `R00012`.
- **measurementId**: `<runId>-<uptime_s, 7 digits>`, e.g. `R00012-0002640`.
- **experimentId and sampleId**: set with `e` on the board. They are 1â€“40 characters from `A-Z a-z 0-9 - _`.
- **Calibration mode**: the experimentId is `CAL-<runId>` (e.g. `CAL-R00012`) and the sampleId is `NONE`.

The board never writes to:
- `devices/FRIDGE001/latest`;
- `decisionHistory`;
- `/baseline`, `/hwtest` or `/test1`;
- `experiments/<id>/meta` or `observations`.

Every run's metadata includes `samplesPath` and `latestPath` as full path strings, so the app can find the history from the run record.

## 4. Flash warning (carried over from baseline_logger)

- Keep **Tools â†’ Erase All Flash Before Sketch Upload: Disabled**. Erasing wipes every record that hasn't been uploaded yet. It also wipes the NVS counters: the experimentId/sampleId, the upload pointers, and the run counter.
- Keep the **default partition scheme**. If you ever change it, type `d` first and save the dump, because the LittleFS partition will be reformatted.
- **Before you flash fridge_monitor over baseline_logger:** type `?` in baseline_logger and check that `unsent (all)` is 0. If you have no internet, type `d` and save the dump. fridge_monitor leaves baseline_logger's `/r*.jsonl` files on flash and counts them in `?`, but it can't upload them.

## 5. Daily use

- **Start a trial.**
  1. Run `python experiment_tool.py new-experiment`.
  2. On the board, type `e`, then the same experimentId and sampleId.
  3. The board starts a new run and shows `FOOD SETTL â€¦` on the LCD.
- **Observations and events:**
  - `python experiment_tool.py observe --exp <id>`
  - `python experiment_tool.py event --exp <id>`

  An event defaults to the runId currently in `latest`.
- **Same meal after a power cut.** Do nothing. The experimentId and sampleId survive in NVS. The new boot gets a new runId and writes a `device_restart` event.
- **New meal or new trial.** Use `new-experiment` with a new experimentId and sampleId, then `e`. Previous records are kept.
- **End a trial.**
  1. Run `python experiment_tool.py status --exp <id> completed`.
  2. Type `e`, then `-`, to put the board back into calibration mode.
- **Handoff.** `python experiment_tool.py show --exp <id>` prints the meta, runs, events, observations and the newest `latest` envelope. That covers the items the PDF's section 6 asks for.

Serial commands:
- `?` shows the status.
- `u` uploads now.
- `d` dumps everything between the `===== BEGIN/END FRIDGE DUMP =====` lines.
- `e` sets the experiment. Enter `-` to clear it or `.` to cancel.
- `x` shows the IDs and the producer UID.

## 6. What could not be done as written, and other decisions

These were agreed before implementation.

1. **The flash would fill up during a multi-day trial.** A record is about 650 bytes, which is roughly 0.9 MB a day, and LittleFS holds about 1.4 MB. baseline_logger never deleted the *current* run.
   - Each run is now split into 6-hour segment files (`/f<run>_<seg>.jsonl`).
   - Uploaded, closed segments are deleted oldest-first once LittleFS is over 75% full.
   - With no internet at all, the board can buffer about 1.5 days before records stop being saved. Serial then shows `!! NOT SAVED TO FLASH`.
2. **Program space.** The sketch uses 1,197,096 bytes, 91% of the default 1.25 MB app partition. baseline_logger uses 88%.
3. **Old baseline files** are left untouched (see section 4).
4. **Calibration mode.** The experimentId is `CAL-<runId>` and the sampleId is `NONE`. The PDF says sampleId is required but the brief gave no value. Run metadata is still written under `experiments/CAL-â€¦/runs/â€¦`, without a `meta` record. The app should ignore `CAL-` experiments in the cooked-meal dataset.
5. **`e` starts a new run without rebooting** (`runStartReason: "experiment_change"`), so one runId never mixes two experiments. Uptime keeps counting, so the settling clock isn't restarted. No `device_restart` event is written for this kind of run.
6. **runIds continue above baseline_logger's boot counter** and above any run already on flash. The first run will therefore be higher than R00001.
   - Remaining risk: if "Erase All Flash" is used, the counter can only restart from the highest baseline count. runIds and measurementIds could then repeat ones already in Firebase.
   - A repeated measurementId overwrites the old envelope.
   - The board can't check for this, because it may not have read access.
   - So: never erase flash.
7. **The PDF requires confirmation of write access before using the new paths.** The firmware can't check this, so it's a procedure step for you (see section 2).
8. **`saturation`** is raised only for readings at 4095. `rail` still counts readings at both 0 and 4095, as before. A disconnected sensor therefore shows `no_signal`, not `saturation`.
9. **Events from experiment_tool** need a runId (the PDF requires one). The tool offers the runId from the current `latest` envelope as the default.
10. **measurementId** pads uptime to 7 digits (the brief's example has 6). This keeps keys in time order for 115 days instead of 11.5 days.
11. **Unknown times.** Optional times that are unknown are left out, never defaulted. The PDF also asks you to note the uncertainty, so `meta.notes` is available for that.

Other details:
- **Conflicts between the brief and the PDF.** There were no direct contradictions in field names or values. The brief's identifiers, quality states, calibration states and paths are all allowed by the PDF. The differences above are gaps (calibration sampleId, the event runId, the write-access confirmation) and limits of the board, not disagreements.
- **Extra fields.** These are additions the spec allows:
  - in the envelope: `timeSource`, `sample.free_heap`, `sample.unsent`, `sample.gap_s`, and per-channel `quality` and `faults`;
  - in the run metadata: `runStartReason`, `runStartUptimeS`, `runStartedAtMs`, a channel `note`, `ppmFormula`, `pin`, `hardware.temperatureHumidity: "not measured"` and `stabilisation.rules`;
  - in board events: `resetReason`.

  `faults` is left out when it would be empty, because Firebase doesn't store empty arrays.
- **Run metadata layout.** Identity, boot details and paths are top-level keys. Hardware, channels, calibration, periods and stabilisation are nested objects.
- **Times.** `uploadedAtMs` uses the Firebase server timestamp. `measuredAtMs` = `sample.epoch` Ã— 1000. Records made before the clock is set carry `timeSource: "rebuilt"`, with time = boot time + uptime. Records from a run whose clock was never set stay on the board and appear in the `d` dump only.
- **LCD.** In `FOOD SETTL 1h12m`, the time is how long settling still has to go (it counts down from 3h00m). The line shows `FAULT MQ1`, `FAULT MQ2` or `FAULT MQ1+MQ2` when a channel faults. With uploads disabled, line 2 ends in `NOUP`.
- **Sign-in.**
  - The refresh token is kept in NVS, tagged with a hash of the device email, so changing the account forces a fresh sign-in.
  - A rejected password is retried only every 10 minutes, so Firebase doesn't lock the account out.
  - Only Firebase's error code (e.g. `INVALID_LOGIN_CREDENTIALS`) is printed, never a credential or token.
- **Power draw.** WiFi now stays on all the time. The baseline runs had it off between hourly uploads. That changes how much current the board draws, and may slightly change the 5 V supply to the sensor heaters. Compare a calibration run with the October baseline before relying on R0.
- **Blocking.** One stalled HTTPS request can still block for up to about 18 s, and that shows up as `gap_s`. If WiFi is down, each reconnect attempt (once a minute) waits up to 20 s, but sampling continues during it.
- **Uptime wraps** after 49.7 days (`millis()` overflows). Trials are expected to be much shorter than that.

## 7. Verification done

- **Compile.** Both sketches compile for ESP32 Dev Module on core 3.3.12, with ArduinoJson 7.4.3 and LiquidCrystal I2C 1.1.2:
  - `fridge_monitor`: 1,197,096 bytes (91%), 51,864 bytes of globals. No warnings from the sketch itself.
  - `baseline_logger`: compiled unchanged as a reference check.
- **Python.** `python experiment_tool.py --self-test` passes on Python 3.15 and on Proteus's bundled Python. It checks:
  - secrets.h parsing and WATâ†”ms conversion;
  - the ID rules;
  - that optional fields are left out when blank;
  - the flattened PATCH keys.

  The tool also parsed the real `secrets.h` and found all four values present. They weren't printed.
- **Not run on the board yet.** Nothing has been run against Firebase. Sign-in, the server timestamp, the 401 handling and pruning have **not** been exercised on hardware.
- **Suggested first test (USB, about 15 minutes):**
  1. Watch for the `Producer UID` line.
  2. Expect `PUT telemetry/FRIDGE001/latest -> HTTP 401 Permission denied` until the rules exist.
  3. Check that `?` shows `writes: DENIED`.
  4. Check that `d` dumps the records.
  5. Once the rules are in place, type `u` and check all four board paths in the console.
