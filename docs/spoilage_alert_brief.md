# Task brief: fridge_monitor 1.2.0, on-board spoilage alert

## Purpose

FRIDGE001 exists to detect spoilt food. Firmware 1.0.0 and 1.1.0 only measure and upload, because the app team's requirements say the app makes the spoilage decision. The app's scoring is not ready, so today nothing in the system says "spoilt". The device must be able to show that on its own, at the defence and in unattended trials.

1.2.0 adds an on-board spoilage alert. The board scores each food experiment against that experiment's own clean-air baseline. It then shows the result on the LCD, sounds the buzzer at Critical and uploads its verdict as an extra field. Everything 1.1.0 uploads stays exactly as it is, so the app team's data and scoring are unaffected.

Deliverables:
1. `fridge_monitor/fridge_monitor.ino`, version 1.2.0, built on the current 1.1.0 sketch in the same folder.
2. `fridge_monitor_1.2.0_notes.md` in the project folder. It covers what changed, the compile result with program and flash use, the bench test results and any conflict with the documents below.

## Documents, in priority order

1. `app_upload_requirements_v1.pdf` (the app team's requirements; text copy in `FRIDGE001_firmware_1.1.0_package/requirements/`)
2. This brief
3. `fridge_monitor_1.1.0_notes.md`, `wifi_setup_notes.md`, `fridge_monitor_notes.md`
4. `FRIDGE001_firmware_1.1.0_package/context/calibration_data_findings_2026-10-17.md` and `rice_test_RICE-SPOILT-001_2026-10-20.md` (the data behind the thresholds)

The PDF says the hardware does not need to calculate spoilage decisions. It does not forbid it. It does reserve `devices/FRIDGE001/latest` and `decisionHistory` for the app's assessments. 1.2.0 must never write to either path. Report any other conflict in the notes; do not resolve it silently.

Do not modify `food_spoilage_test1_reboot_safe.ino`, `hw_selftest`, `baseline_logger`, `experiment_tool.py` or either `secrets.h`. Back up the 1.1.0 sketch as `fridge_monitor_v1.1.0_backup/fridge_monitor_v1.1.0_backup.ino` (without `secrets.h`) before editing.

## Part 1: partition scheme

1.1.0 uses 1,305,628 bytes, 99.6% of the default 1.25 MB app partition. 1.2.0 will not fit.

- Build for **ESP32 Dev Module, partition scheme "No OTA (2MB APP/2MB SPIFFS)"**, esp32 core 3.3.12, "Erase All Flash" disabled. LittleFS mounts on the `spiffs` partition label, as now. Confirm this in the notes. If that scheme is not available in core 3.3.12, pick the closest scheme with at least a 1.9 MB app and at least 1.4 MB for LittleFS, and say which one in the notes.
- Changing the scheme reformats LittleFS. NVS survives, so the run counter, boot counter, Wi-Fi details and experiment IDs carry over.
- Put the required Tools menu settings at the top of the sketch in the header comment.
- Report program use and free LittleFS space after the change.

## Part 2: baseline

The calibration findings showed that one fixed R0 cannot work: clean-air Rs moved by a factor of about 2 between days. So each experiment is scored against its own baseline, taken in the minutes before the food goes in.

- Keep a rolling buffer of the last `BASELINE_WINDOW_RECORDS` = 15 one-minute records: mean Rs for each channel, its quality and the record time.
- When an experiment starts (setup page or Firebase command, the same shared start logic as 1.1.0), compute each channel's baseline as the **median** of the mean Rs over the records in the buffer that meet all of these:
  - the channel's quality is `valid` (settling over, no fault);
  - the record is from the current boot;
  - the record is no older than 20 minutes.
- A channel's baseline is usable when at least `BASELINE_MIN_RECORDS` = 10 records qualify. If neither channel has a usable baseline, the experiment still starts. The alert state is `no_baseline` for the whole experiment, and the setup page says why: "Baseline not ready: wait until the LCD shows CAL VALID for 15 min before starting".
- Store the baseline in NVS with the experimentId: per channel Rs in ohm and the number of records used, plus the window start and end times in Unix ms. A reboot during the experiment keeps it. Clear it when the experiment ends.
- Show the baseline on the setup page status block and in Serial `?` and `x`.

Steve's procedure becomes: power on with the fridge empty, wait until the LCD has shown `CAL VALID` for at least 15 minutes, then put the food in and press Start. With `SETTLE_S` at 30 minutes, the earliest start is 45 minutes after power-on. The findings put the plateau at 45 to 60 minutes, so this is consistent. Leave `SETTLE_S` at 1800.

## Part 3: score

Spoiling food releases gases that lower MQ135 Rs, so the score measures how far Rs has fallen below the baseline, on a log scale:

- For each channel with a usable baseline and a `valid` record: `ratio = Rs_mean / Rs_baseline`, then `pct = 100 x log10(ratio) / log10(FULL_SCALE_RATIO)`, clamped to 0 to 100.
- `FULL_SCALE_RATIO` = 0.10. This means Rs at one tenth of its baseline scores 100%.
- The experiment score is the higher of the two channel scores. A channel with a fault, or still settling after a reboot, is left out. If both are left out, the state is `settling` or `fault` (see Part 4), and the last verdict is kept, not cleared.
- A channel at the ADC rail still counts. Mark it `saturated: true` in the uploaded verdict, because the real score may be higher.

The existing bands stay as they are: Normal under 40%, Warning 40 to 80%, Critical 80% and over. With `FULL_SCALE_RATIO` = 0.10, Warning starts when Rs falls to 40% of baseline (a 2.5x fall), and Critical when it falls to 16% (a 6.3x fall).

Why these values (put this in the notes; it is the defence of the thresholds):
- Spoilt rice, R00062: MQ1 fell to 0.032 of baseline, which scores 100% (Critical) and crossed 50% of baseline within 2 minutes. MQ2 fell to 0.233, which scores 63% (Warning).
- Clean air with no food: the largest single step seen within a run was a 28% fall on MQ1 alone (R00007, 17 Oct). That is a ratio of 0.72 and scores 14% (Normal). Slow drift was about 2% per hour.
- These come from one spoilt sample and a few empty runs. They are starting values, not validated thresholds. Make `FULL_SCALE_RATIO`, `WARN_PCT`, `CRIT_PCT` and the persistence counts named constants at the top of the sketch so they can be changed after a fresh-to-spoilt trial.

## Part 4: alert states and persistence

Alert state, one of: `off` (calibration mode, no experiment), `no_baseline`, `settling`, `fault`, `normal`, `warning`, `critical`.

- Evaluate once per one-minute record, only in food-test mode.
- To move up a band, the score must be in or above that band for `RISE_RECORDS` = 3 records in a row. That rejects single-record jumps like the unexplained MQ1 step during the rice test.
- To move down a band, it must be below the band's threshold minus `HYSTERESIS_PCT` = 10 for `FALL_RECORDS` = 5 records in a row. For example, Critical drops to Warning only below 70%.
- After a reboot during an experiment, the state is `settling` until `SETTLE_S` has passed. Then scoring resumes against the stored baseline. Keep the last band from before the reboot in NVS and show it on the LCD as held (see Part 5), so a power cut does not hide a Critical result.
- Log every band change to Serial with the time, score and both channel ratios.

## Part 5: LCD, buzzer and LED

In food-test mode, line 1 shows the verdict, replacing `FOOD VALID`:

| State | Line 1 |
|---|---|
| normal | `FRESH 12%` |
| warning | `WARNING 55%` |
| critical | `SPOILT 92%` |
| no_baseline | `NO BASELINE` |
| settling | `FOOD SETTL 0h22m`, or `HELD SPOILT` / `HELD WARNING` if that was the last band |
| fault | `FAULT MQ1` and so on, as now |

Line 2 stays as in 1.1.0 (both voltages and the unsent count). The setup-page messages (`FOOD <id>`, `EXP ENDED`, `RELEASE WIFI BTN`) still take priority for their few seconds. Calibration mode is unchanged.

Buzzer and LED:
- On entering Warning: 2 short beeps, once.
- On entering Critical: 5 pulses, then 5 pulses every `CRIT_REMIND_MS` = 10 minutes while Critical, until the experiment ends or the alarm is silenced.
- LED: the normal logging blink in Normal; a slow blink (1 s on, 1 s off) in Warning; a fast blink (200 ms) in Critical.
- Silence: add a "Silence alarm" button to the setup page status block. It stops the Critical reminders for the rest of this Critical episode. It does not change the state, the LCD or the uploaded verdict. A new rise into Critical after it has dropped back to Warning sounds again.
- The settling-end beep and the button beeps stay as they are.

## Part 6: upload

Add one object, `deviceAlert`, to each telemetry envelope (history samples and `telemetry/FRIDGE001/latest`). It is an extra field the PDF does not define. List it in the notes for the app team, next to the other extra fields.

```json
"deviceAlert": {
  "method": "rs_drop_log_v1",
  "state": "warning",
  "scorePct": 55.2,
  "channels": {
    "mq1": {"ratio": 0.39, "pct": 40.9, "used": true, "saturated": false},
    "mq2": {"ratio": 0.53, "pct": 27.6, "used": true, "saturated": false}
  },
  "baseline": {"mq1RsOhm": 21800, "mq2RsOhm": 6170, "n": 15, "fromMs": 0, "toMs": 0},
  "fullScaleRatio": 0.10, "warnPct": 40, "critPct": 80,
  "sinceMs": 0,
  "silenced": false
}
```

The values above show the shape only. Do not treat them as measurements.

- In calibration mode, write `{"method": "rs_drop_log_v1", "state": "off"}` only.
- Add the baseline and the alert constants to the run record at `experiments/<experimentId>/runs/<runId>`, under `deviceAlert`.
- When the band changes, add an event at `experiments/<experimentId>/events/<runId>-alert-<n>` with `type` `device_alert`, `observedAtMs`, `fromState`, `toState` and `scorePct`. Use the existing outbox, so it survives Wi-Fi loss and power cuts like other events. If the PDF's event type list does not allow `device_alert`, say so in the notes and keep it.
- Never write to `devices/FRIDGE001/latest` or `decisionHistory`.
- Set `firmwareVersion` to `1.2.0`.

## Part 7: Serial

- `?` adds the alert state, score, both ratios and the baseline.
- Add `a`: print the last 15 band evaluations (time, score, both ratios, state).
- Add `A <pct>`: test only. For the next 10 records, force the experiment score to `<pct>` so the LCD, buzzer, LED and upload path can be checked without food. Mark these records `"test": true` in `deviceAlert` and print a warning on Serial. Ignore it in calibration mode.

## Part 8: bench test

Steve will run this with the board outside the enclosure on USB, then again in the enclosure. Report every step's result in the notes.

1. Before flashing, Steve saves Serial output from `d` and `r`, because the partition change reformats LittleFS.
2. Flash. Confirm program use, the LittleFS size and that NVS kept the boot and run counters (the new run must be above R00086).
3. Boot in calibration mode. The LCD shows `CAL SETTL`, then `CAL VALID`. `deviceAlert.state` is `off`.
4. Start an experiment less than 15 minutes after `CAL VALID`. The state is `no_baseline` and the page shows the reason.
5. End it. Wait 15 minutes of `CAL VALID`. Start again: a baseline is taken and shown on the page and in `?`.
6. `A 50`, then `A 90`: check the Warning and Critical display, beeps, LED, reminder timing, Silence, the band-change events in Firebase and the `test` flag.
7. Gas check: hold an alcohol swab or a marker pen near the sensors for about 3 minutes. Rs should fall and the real score should rise. Record the peak ratio for each channel. This checks the direction only; it is not a food result.
8. Press EN during an experiment. Check `settling` with the held band, then scoring resumes against the stored baseline.

## Out of scope

- No change to sampling, record format, upload timing, Wi-Fi setup or the experiment start and end logic, apart from what this brief adds.
- No temperature or humidity fields.
- No change to `SETTLE_S` or R0.
