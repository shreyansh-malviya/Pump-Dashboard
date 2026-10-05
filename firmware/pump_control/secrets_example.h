/*
 * secrets_example.h — committed template.
 * Copy this file to `secrets.h` (which is gitignored) and fill in real
 * values. The sketch #includes "secrets.h".
 */
#pragma once

// ---- WiFi (primary) the ESP32 joins (station mode) ----
#define WIFI_SSID   "your-wifi-name"
#define WIFI_PASS   "your-wifi-password"

// ---- WiFi (fallback — tried if primary fails) ----
#define WIFI_SSID2  "your-fallback-wifi-name"
#define WIFI_PASS2  "your-fallback-wifi-password"

// ---- LAN dashboard login (HTTP Basic auth; local page only) ----
#define DASH_USER   "admin"
#define DASH_PASS   "choose-a-strong-dashboard-password"

// ---- MQTT broker (e.g. HiveMQ Cloud). TLS port is usually 8883. ----
#define MQTT_SERVER   "xxxxxxxx.s1.eu.hivemq.cloud"
#define MQTT_PORT     "8883"
#define MQTT_USER     "your-mqtt-device-username"
#define MQTT_PASS     "your-mqtt-device-password"
