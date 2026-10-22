# FRIDGE001 Wi-Fi setup page: notes

Changed files:
- `fridge_monitor/fridge_monitor.ino`: latching button, setup portal, saved-network fallback, `w` and `W`.
- `fridge_monitor/secrets.h`: `SETUP_AP_SSID` and `SETUP_AP_PASSWORD` added at the end, with comments.

`food_spoilage_test1_reboot_safe.ino`, `hw_selftest` and `baseline_logger` were not modified. There is no `secrets_example.h` or template in the sketch folder, so nothing else needed the new entries.

## 1. Wiring

| Part | ESP32 pin | Other side |
|---|---|---|
| Latching push button | GPIO 4 | GND |

Use the common leg and the normally-open leg. Check them with a multimeter in continuity mode: they must beep only while the button is latched in. No resistor is needed (the firmware uses `INPUT_PULLUP`).

## 2. How it behaves

- **Press (latch in):** the board opens the setup network `SETUP_AP_SSID` with the password `SETUP_AP_PASSWORD`, and the page at `192.168.4.1`. One short beep. The LED blinks fast (every 250 ms).
- **Release:** the portal closes and the board goes back to normal Wi-Fi and uploads.
- **Latched at power-up:** no portal. The LCD shows `RELEASE WIFI BTN` until the button is released. Press it again to open setup.
- **Timeout:** the portal closes after 10 minutes, or 30 s after a successful save, even if the button is still latched. The LCD then shows `RELEASE WIFI BTN`.
- **Wrong setup password length:** if `SETUP_AP_PASSWORD` is not 8 to 13 characters, pressing the button shows `AP PW 8-13 CHARS` and no setup network appears. Serial shows the same message, without the password.

LCD while the portal is open (alternates every 4 s, starting with A):

| Screen | Line 1 | Line 2 |
|---|---|---|
| A | `FRIDGE001-Setup` | `PW:<your password>` |
| B | `WIFI SETUP 9:41` (`*` at the end once a phone has joined) | `192.168.4.1` |

While a network is being tested, the LCD shows `WIFI TESTING` and the SSID. After the test it shows `WIFI OK` and the SSID, or `WIFI FAIL` and the reason (`wrong password`, `not found` or `timeout`). `WIFI FAIL` stays for 10 s, then the two screens come back.

The setup password is shown on the LCD only while the portal is open. It is never printed on Serial, never sent to the page and never uploaded.

## 3. Which network the board uses

1. The network saved with the setup page (NVS namespace `wifi`), if there is one.
2. If that fails within 20 s, `WIFI_SSID` from `secrets.h`.
3. If both fail, the board keeps logging to flash and retries every 60 s, as before.

A network is saved only after the board has actually joined it (15 s limit). A failed test saves nothing and keeps the old saved network.

"Forget saved network" (on the page, or `W` then `y` on Serial) deletes the saved network. The board then uses `secrets.h` again.

## 4. Records

- Run metadata (`experiments/<experimentId>/runs/<runId>`) gets `wifiSource` (`saved` or `secrets`) and `wifiSsid`, and is PATCHed again after every new connection. Passwords are never recorded.
- After a successful save during a food test, the board writes `experiments/<experimentId>/events/<runId>-wifi-<epoch>` with `type: "other"` and notes `Wi-Fi changed to <SSID> via setup page`. It goes up with the next history upload after the portal closes.
- While the portal is open, sampling, the 60 s records and LittleFS logging carry on. Firebase uploads (`latest`, history, run metadata, events) pause. `unsent` grows, and the normal upload clears it once the portal has closed.

## 5. Serial

- `w`: Wi-Fi status: connected SSID and source, RSSI, IP, saved network name, secrets.h network name, portal state, button position. No passwords.
- `W`: forget the saved network. Type `y` to confirm, anything else (or `.`) to cancel.

## 6. Test procedure

1. Edit `SETUP_AP_PASSWORD` in `fridge_monitor/secrets.h` to your own 8 to 13 character password and flash.
2. Press the latch button. The LCD shows the setup network name and `PW:` with your password, then `WIFI SETUP` with the timer and `192.168.4.1`.
3. Connect a phone to the setup network using the password on the LCD. The page opens by itself, or open `192.168.4.1`. The `*` appears on the status screen.
4. Enter a wrong hotspot password. The page shows the failure and nothing is saved.
5. Enter the phone hotspot correctly. The LCD shows `WIFI OK`. Release the button. Uploads resume and `unsent` falls to 0.
6. Switch the board off and on. It joins the hotspot without help.
7. Turn the hotspot off and reboot. The board falls back to `secrets.h` and keeps logging.
8. Leave the button latched and cut the power. On power-up the LCD shows `RELEASE WIFI BTN` and no portal opens.
9. Set a 6-character setup password, flash, and press the button. The LCD shows `AP PW 8-13 CHARS` and no setup network appears.
10. Check in Firebase that no records are missing across the setup session.

For step 5 the hotspot must be 2.4 GHz. On an iPhone, turn on **Maximise Compatibility**. On Android, set the band to 2.4 GHz.

## 7. Assumptions and things not done exactly as written

1. **The phone may drop off the setup network during a test.** The ESP32 has one radio. When it joins a network on a different channel, the setup network moves to that channel, and the phone can lose it for a few seconds. Most phones rejoin by themselves. If the page stops updating, check the LCD: it always shows `WIFI OK` or `WIFI FAIL`. Then reopen `192.168.4.1`.
2. **Failure reasons** come from the ESP32's disconnect code. Some routers and hotspots report a wrong password only as a handshake timeout or not at all, so a wrong password can sometimes show as `timeout`.
3. **Open networks** (no password) can be saved. The page rejects passwords of 1 to 7 characters, because WPA2 needs at least 8.
4. **SSID and password limits** are 32 and 63 bytes, checked on the page and again on the board. Names with non-ASCII characters count as more than one byte each.
5. **Firebase uploads stay paused for the whole portal session**, including after a successful save, until the portal closes (release, or 30 s after the save). This follows "uploads pause while the portal is open".
6. **The Wi-Fi change event waits in RAM** until the clock is set. If the board reboots before the clock is ever set, that one event is lost. The run metadata still records the new network.
7. **The scan list** shows up to 25 networks, strongest first, one entry per name. Hidden networks don't appear; type their name in the field.
8. **A board with no secrets.h network** can now upload once a network is saved with the page, as long as the Firebase settings and device user are filled in.
9. **`firmwareVersion` stays `1.0.0`**, because `fridge_monitor_brief.md` fixes that value. Use `wifiSource` in the run metadata to tell the runs made after this change apart.
10. **The top-level `secrets.h`** (the one `experiment_tool.py` reads) does not have the two new entries. If you copy it over `fridge_monitor/secrets.h` again, add the two entries back. Otherwise the button shows `AP PW 8-13 CHARS`.

## 8. Verification

- **Compile:** builds for ESP32 Dev Module on esp32 core 3.3.12 with `--warnings all`, and the sketch itself gives no warnings.
  - Program: 1,255,196 bytes, **95%** of the default 1.25 MB app partition (it was 91% before). About 55 KB is left, so there is little room for more features. If a later change doesn't fit, use the "Minimal SPIFFS" partition scheme. Changing the scheme reformats LittleFS, so type `d` and save the dump first.
  - Globals: 52,632 bytes (16%).
- **Crash found on the board and fixed (19 Oct 2026).** Boot #18 panicked right after `UPLOAD (boot)` with `assert failed: udp_new_ip_type ... Required to lock TCPIP core functionality!`, then rebooted with reset reason `PANIC`.
  - Cause: a bug in esp32 core 3.3.12. The first `hostByName()` after the IP state changes calls `dns_clear_cache()` without the lwIP lock. If SNTP still has a DNS lookup for `pool.ntp.org` pending, its callback runs from the Arduino loop task and lwIP asserts. It is a timing race, so most boots survive it.
  - Workaround in the sketch: SNTP now starts only after the first name lookup on a live connection, instead of at boot. Sign-in, refresh and database requests return at once while WiFi is down, so no lookup ever runs without an IP. That is what triggers the cache clear again on the next reconnect.
  - A small risk remains: if WiFi drops in the instant between that check and the request, the next reconnect can still race. If the crash comes back, the next step is an older or newer core.
- **LCD not found** in that log, because the ESP32 was off the sensor board at the time. Expect `LCD: not found` and `no_signal` until it's back on the board.
- **Button and portal not tested on the board yet.** The portal, the captive-page redirects on real phones, NVS saving and the fallback have not been exercised on hardware.
