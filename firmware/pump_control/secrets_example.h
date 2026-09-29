/*
 * secrets_example.h — committed template.
 * Copy this file to `secrets.h` (which is gitignored) and fill in real
 * values. The sketch #includes "secrets.h".
 */
#pragma once

// ---- WiFi the ESP32 joins (station mode) ----
#define WIFI_SSID   "your-wifi-name"
#define WIFI_PASS   "your-wifi-password"

// ---- Dashboard login (HTTP Basic auth) + MQTT command password ----
#define DASH_USER   "admin"
#define DASH_PASS   "choose-a-strong-dashboard-password"

// ---- MQTT broker (e.g. HiveMQ Cloud). TLS port is usually 8883. ----
#define MQTT_SERVER   "xxxxxxxx.s1.eu.hivemq.cloud"
#define MQTT_PORT     "8883"
#define MQTT_USER     "your-mqtt-username"
#define MQTT_PASS     "your-mqtt-password"

// ---- MQTT Login ----
#define MQTT_SERVER   ""
#define MQTT_PORT   ""
#define MQTT_USER   ""
#define MQTT_PASS   ""
