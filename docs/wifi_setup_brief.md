# Task brief: Wi-Fi setup page with a latching button (fridge_monitor)

## Purpose

Let a user put FRIDGE001 on their own phone hotspot or home Wi-Fi without reflashing. A latching push button on the enclosure opens a setup network and web page on the ESP32. The user picks their network, types the password, and the board saves it and connects.

Add this to `fridge_monitor/fridge_monitor.ino`. Do not modify `food_spoilage_test1_reboot_safe.ino`, `hw_selftest` or `baseline_logger`. Everything in `fridge_monitor_brief.md` still applies.

## Hardware

| Part | ESP32 pin | Notes |
|---|---|---|
| Latching push button (Wi-Fi setup) | GPIO 4 | Other side to GND. `INPUT_PULLUP`. Latched in = LOW, released = HIGH |

GPIO 4 is free and is not a boot strapping pin. Existing pins are unchanged: MQ1 34, MQ2 35, LCD SDA 16 / SCL 17, buzzer 27, LED 33.

Latching switches often have 4 or 6 legs. Use the common leg and the normally-open leg; Steve confirms them with a multimeter in continuity mode.

## secrets.h changes

Add these two entries to `fridge_monitor/secrets.h`, in the same style as the existing entries, each with a comment so Steve can find and edit them:

- `SETUP_AP_SSID`, default `"FRIDGE001-Setup"`. Comment: name of the setup network the board creates when the Wi-Fi button is pressed. Up to 16 characters so it fits on one LCD line.
- `SETUP_AP_PASSWORD`, default `"fridge2026"`. Comment: password for the setup network. CHANGE THIS. 8 to 13 characters (WPA2 needs at least 8; 13 is the most that fits on the LCD after `PW:`). This password is shown on the LCD while setup is open.

Also add both entries, with placeholder values and the same comments, to any `secrets_example.h` or template in the sketch folder.

At boot, check the password length. If it is shorter than 8 or longer than 13 characters, do not open the setup network. Show `AP PW 8-13 CHARS` on the LCD when the button is pressed, and print the same on Serial (without printing the password).

## Button behaviour (latching)

A latching button holds its position, so the firmware reacts to the change of position, not to the position itself.

- Debounce: accept a new position only after it is stable for 50 ms.
- **Released to latched (HIGH to LOW):** open the setup portal.
- **Latched to released (LOW to HIGH):** close the portal if it is open, and return to normal Wi-Fi.
- **Latched at boot:** do not open the portal. Show `RELEASE WIFI BTN` on the LCD until it is released. The portal opens only on a fresh press, so a button left latched cannot keep the board in setup mode after a power cut.
- **Portal timeout:** close the portal 10 minutes after it opens, or 30 s after a successful save, even if the button is still latched. Then show `RELEASE WIFI BTN` until it is released. Pressing it again opens a new session.

## Setup portal

- Run the ESP32 as an access point plus station (`WIFI_AP_STA`) so it can scan and test networks while the page is open.
- Access point name `SETUP_AP_SSID` and WPA2 password `SETUP_AP_PASSWORD`, both from `secrets.h`. The password is shown on the LCD (below) but never printed on Serial.
- Use the built-in `WebServer` and `DNSServer`. Answer every DNS query with 192.168.4.1 so phones show the page automatically (captive portal). Respond to the usual Android, iOS and Windows connectivity-check URLs with a redirect to the page.
- The page is served from flash as a string constant, with no external files or internet resources. It must work on a phone screen.
- The page shows:
  - a list of scanned networks with signal strength (refresh button), plus a field to type a hidden network name;
  - a password field (show/hide toggle);
  - a note that the ESP32 only joins 2.4 GHz networks, so the hotspot must be set to 2.4 GHz;
  - the currently saved network name, never its password;
  - Save, and Forget saved network.
- On Save: try to connect with a 15 s limit. If it connects, store SSID and password in NVS (Preferences, namespace `wifi`) and show success with the RSSI. If it fails, do not store anything and show the failure reason (wrong password, not found, timeout).
- Limit SSID to 32 characters and password to 63. Escape any SSID shown on the page.

## Logging must not stop

- Sampling, the 60 s records and LittleFS logging continue while the portal is open. The web server and DNS server run in the main loop without blocking (handle clients between samples).
- Firebase uploads pause while the portal is open. The `unsent` count grows and is cleared by the normal upload once the board is back on Wi-Fi.
- Do not use the WiFiManager library's blocking mode.

## Which network the board uses

1. Saved network from NVS, if one exists.
2. If it fails to connect within 20 s, the network in `secrets.h`.
3. If both fail, keep logging locally and retry on the existing backoff.

Firebase credentials (API key, database URL, device email and password) stay in `secrets.h` and are never shown or changed by the page.

## Records

- Add `wifiSource` (`saved` or `secrets`) and the connected SSID to the run metadata at each connection. Never record passwords.
- Write an event `experiments/<experimentId>/events/<runId>-wifi-<epoch>` with type `other` and notes "Wi-Fi changed to <SSID> via setup page" when a save succeeds and an experiment is set. This explains upload gaps during a trial.

## LCD, LED and buzzer

While the portal is open, the 16x2 LCD alternates between two screens every 4 s:

| Screen | Line 1 | Line 2 |
|---|---|---|
| A: join details | `FRIDGE001-Setup` (the `SETUP_AP_SSID` value) | `PW:fridge2026` (the `SETUP_AP_PASSWORD` value) |
| B: status | `WIFI SETUP 9:41` (time left) | `192.168.4.1` |

- Start on screen A, so the join details are the first thing the user sees.
- Once a phone has joined the setup network (station count above 0), add a `*` at the end of line 1 on screen B.
- After save: `WIFI OK` with the SSID on line 2 (cut to 16 characters), or `WIFI FAIL` with the reason on line 2.
- Button latched with the portal closed: `RELEASE WIFI BTN`.
- The password appears on the LCD only while the portal is open. It never appears in normal mode.
- LED: fast blink (every 250 ms) while the portal is open.
- Buzzer: one short beep when the portal opens and two when a save succeeds.

## Serial

- `w`: print Wi-Fi status: source, SSID, RSSI, IP, portal state. No passwords.
- `W`: forget the saved network (asks for `y` to confirm).

## Test procedure (for the final notes)

1. Edit `SETUP_AP_PASSWORD` in `secrets.h` to your own 8 to 13 character password and flash.
2. Press the latch button. The LCD shows the setup network name and `PW:` with your password, then `WIFI SETUP` with the timer and `192.168.4.1`.
3. Connect a phone to the setup network using the password on the LCD. The page opens by itself, or open 192.168.4.1. The `*` appears on the status screen.
4. Enter a wrong hotspot password. The page shows the failure and nothing is saved.
5. Enter the phone hotspot correctly. The LCD shows `WIFI OK`. Release the button. Uploads resume and `unsent` falls to 0.
6. Switch the board off and on. It joins the hotspot without help.
7. Turn the hotspot off and reboot. The board falls back to `secrets.h` and keeps logging.
8. Leave the button latched and cut the power. On power-up the LCD shows `RELEASE WIFI BTN` and no portal opens.
9. Set a 6-character setup password, flash, and press the button. The LCD shows `AP PW 8-13 CHARS` and no setup network appears.
10. Check in Firebase that no records are missing across the setup session.

## Before you finish

- Compile for ESP32 Dev Module on esp32 core 3.3.12, as before.
- List anything in this brief you had to assume or could not follow.
