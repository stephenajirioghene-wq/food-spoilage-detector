// FRIDGE001 private settings. Copy this file to secrets.h in the same folder
// and fill in your own values. secrets.h is in .gitignore: never commit it.

#pragma once

// WiFi (must be a 2.4 GHz network; the ESP32 cannot see 5 GHz)
#define WIFI_SSID              "your-wifi-name"
#define WIFI_PASSWORD          "your-wifi-password"

// Firebase: Project settings > General > Web API key
#define FIREBASE_API_KEY       "your-firebase-web-api-key"

// Firebase: Realtime Database > Data tab, the URL at the top
// Example form: https://<project-id>-default-rtdb.<region>.firebasedatabase.app
#define FIREBASE_DATABASE_URL  "https://your-project-id-default-rtdb.europe-west1.firebasedatabase.app/"

//device log ins
#define DEVICE_EMAIL    "your-device-user@example.com"
#define DEVICE_PASSWORD "your-device-user-password"

// Wi-Fi setup page: name of the setup network the board creates when the Wi-Fi
// button is pressed. Up to 16 characters so it fits on one LCD line.
#define SETUP_AP_SSID          "FRIDGE001-Setup"

// Wi-Fi setup page: password for the setup network. CHANGE THIS.
// 8 to 13 characters (WPA2 needs at least 8; 13 is the most that fits on the LCD
// after "PW:"). This password is shown on the LCD while setup is open.
#define SETUP_AP_PASSWORD      "change-me-123"
