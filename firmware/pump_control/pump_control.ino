/*
 * pump_control.ino  —  ESP32 three-motor pump starter controller
 * =====================================================================
 * Controls THREE independent pump motors (2x 5 HP, 1x 7.5 HP), each through ONE active-low
 * relay wired in series with that motor's contactor coil, downstream of
 * the thermal overload contact (the overload stays HARDWIRED and is NOT
 * controlled by software).
 *
 * Each motor can be turned on/off from TWO sources that drive the SAME
 * state machine:
 *   1. One physical push button per motor (momentary NO) that TOGGLES:
 *      press when off -> start, press when running/locked -> stop.
 *   2. The password-protected web dashboard / MQTT -> cmdStart(i)/cmdStop(i).
 *
 * WiFi/dashboard credentials live in secrets.h (gitignored). If WiFi is
 * unavailable the pump is still fully controllable from the physical
 * buttons; the web layer is best-effort and never blocks motor control.
 *
 * REACHING IT "FROM ANYWHERE" — two layers:
 *   - LOCAL: the ESP32 serves the page on the LAN (http://pump.local
 *     or its IP), Basic-auth gated.
 *   - PUBLIC: the ESP32 also makes an OUTBOUND TLS connection to an MQTT
 *     broker (HiveMQ Cloud). Because it dials out, no port-forwarding or
 *     public IP is needed — it works through NAT/CGNAT. A public website
 *     (see web/index.html) talks to the same broker over WebSockets, so
 *     the pump can be controlled from anywhere in the world.
 *
 * MQTT command auth: The ESP32 trusts all commands arriving on the
 * pump/cmd topic. You MUST configure your MQTT broker to only allow
 * authorized users (like a dedicated admin user) to publish to this topic,
 * using the broker's Access Control Lists (ACLs).
 *
 * This is a PUMP, not a pumping/irrigation system: there is no
 * tank-level auto mode and no dry-run/float logic. Control is purely the
 * two manual sources above.
 *
 * ---------------------------------------------------------------------
 * WIRING ASSUMPTIONS  (verify against your hardware before powering on)
 * ---------------------------------------------------------------------
 *  - Board: ESP32 Dev Module (3.3 V logic). Relay module MUST be a
 *    3.3 V-trigger-compatible, opto-isolated, ACTIVE-LOW module.
 *  - Active-low relay: IN = LOW  -> relay ENERGIZED (motor ON)
 *                      IN = HIGH -> relay OFF        (motor OFF)  <-- boot default
 *  - One push button per motor: momentary NO, between the button pin and GND.
 *      INPUT_PULLUP -> idle reads HIGH, pressed (shorts to GND) reads LOW.
 *      A press TOGGLES the motor (off->start, running/lockout->stop). No
 *      cut-wire fail-safe; a broken button wire simply can't command.
 *  - One run LED per motor (LED -> resistor -> GND, active-HIGH): ON while running.
 *  - One state LED (connectivity): solid = MQTT online, slow blink = WiFi only,
 *      fast blink = connecting, off = no WiFi.
 *  - Common ground: ESP32 GND, relay/button/LED grounds all tied.
 *  - Feed relay coils from a separate 5 V supply (JD-VCC), sharing ground
 *    with the ESP32. Add an external ~10 k pull-UP on each relay IN line
 *    so the relay stays de-energized during the boot glitch.
 *
 * ---------------------------------------------------------------------
 * SAFETY BEHAVIOUR
 * ---------------------------------------------------------------------
 *  - On boot the relays are driven OFF (HIGH) BEFORE anything else, and
 *    the controller never auto-starts on power-up.
 *  - The ESP32 Task Watchdog is enabled; a firmware hang reboots the
 *    chip, and on reboot the relays default OFF.
 *  - Minimum OFF time between a stop and the next start protects the
 *    contactor/motor from rapid cycling.
 *  - Maximum continuous run time: after this the motor is force-stopped
 *    and latched into LOCKOUT, requiring a manual Stop press (or
 *    dashboard reset) before it can start again. Set MAX_RUN_MS = 0 to
 *    disable (a pump may legitimately run for hours).
 *
 * Serial debug: 115200 baud, prints every state transition and its
 * cause. Bring-up aid, not a permanent feature.
 *
 * NOTE: All 3-phase mains / contactor-coil wiring must be designed and
 * signed off by a licensed electrician. This sketch switches only the
 * low-voltage control side and must not bypass the overload relay.
 */

#include "esp_task_wdt.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include "motor.h"
#include "secrets.h"    // WiFi, dashboard, and MQTT credentials (gitignored)

// ======================= CONFIGURATION ===============================

// ---- Pin assignments (ESP32). One relay, one toggle button, one run LED per
//      motor, plus a connectivity state LED. GPIO5/15/2 are strapping pins but
//      safe in these roles (relay boots OFF; LEDs boot harmless). ----
static const MotorPins MOTOR_PINS[] = {
  // name        relay  button  led
  { "5 HP #1",    5,     21,    15 },
  { "5 HP #2",   18,     26,     2 },
  { "7.5 HP",    19,     25,     4 },
};
static const uint8_t MOTOR_COUNT = sizeof(MOTOR_PINS) / sizeof(MOTOR_PINS[0]);

static const uint8_t STATE_LED_PIN = 16;   // connectivity LED (RX2 / GPIO16)

// ---- Timing constants ----
static const uint32_t DEBOUNCE_MS   = 30;            // button debounce window
static const uint32_t MIN_OFF_MS    = 0;             // min gap stop -> next start (0 = disabled)
static const uint32_t MAX_RUN_MS    = 2UL * 60UL * 60UL * 1000UL;  // 2 h; 0 = disabled
static const uint32_t WDT_TIMEOUT_S = 8;             // watchdog timeout (seconds)

static Motor motors[MOTOR_COUNT];

// ---- Command API — the single entry point for BOTH control sources ----
// Physical buttons (in loop) and the web dashboard both call these.
void cmdStart(uint8_t i) { if (i < MOTOR_COUNT) motors[i].startReq = true; }
void cmdStop(uint8_t i)  { if (i < MOTOR_COUNT) motors[i].stopReq  = true; }

// ======================= WEB DASHBOARD ===============================
// Minimal, no CSS. Basic-auth gated. Buttons POST to /set, which calls the
// same command API the physical buttons use, so all safety rules still hold.
static WebServer  server(80);
static const char* MDNS_HOST = "pump";   // -> http://pump.local/

// Returns false and sends a 401 if the client is not authenticated.
static bool requireAuth() {
  if (!server.authenticate(DASH_USER, DASH_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

static String buildPage() {
  String h;
  h.reserve(1400);
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<meta http-equiv='refresh' content='3'>";   // live-ish status, no JS/CSS
  h += "<title>Pump Control</title></head><body>";
  h += "<h1>Pump Control</h1>";
  h += "<p>Status auto-refreshes every 3 seconds.</p>";
  for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
    Motor& m = motors[i];
    h += "<h2>";
    h += m.name;
    h += " &mdash; ";
    h += stateName(m.state);
    h += "</h2><form method='POST' action='/set'>";
    h += "<input type='hidden' name='id' value='";
    h += i;
    h += "'><button name='cmd' value='start'>Start</button> ";
    h += "<button name='cmd' value='stop'>Stop</button></form>";
  }
  h += "</body></html>";
  return h;
}

static void handleRoot() {
  if (!requireAuth()) return;
  server.send(200, "text/html", buildPage());
}

static void handleSet() {
  if (!requireAuth()) return;
  int    id  = server.hasArg("id") ? server.arg("id").toInt() : -1;
  String cmd = server.arg("cmd");
  if (id >= 0 && id < (int)MOTOR_COUNT) {
    if      (cmd == "start") { cmdStart((uint8_t)id); Serial.printf("[web] start %s\n", motors[id].name); }
    else if (cmd == "stop")  { cmdStop((uint8_t)id);  Serial.printf("[web] stop  %s\n", motors[id].name); }
  }
  server.sendHeader("Location", "/");   // back to the status page
  server.send(303, "text/plain", "ok");
}

// Shared JSON snapshot of all motors (used by /status and by MQTT).
static String stateJson() {
  String j = "{\"motors\":[";
  for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
    if (i) j += ",";
    j += "{\"id\":";
    j += i;
    j += ",\"name\":\"";
    j += motors[i].name;
    j += "\",\"state\":\"";
    j += stateName(motors[i].state);
    j += "\"}";
  }
  j += "]}";
  return j;
}

// JSON snapshot — handy for scripts / a future richer frontend.
static void handleStatus() {
  if (!requireAuth()) return;
  server.send(200, "application/json", stateJson());
}

// Connect to WiFi — tries primary, then fallback. Non-fatal: buttons work
// regardless. Blocks up to timeoutMs per network; called before the WDT is armed.
static bool tryConnect(const char* ssid, const char* pass, uint32_t timeoutMs) {
  Serial.printf("WiFi: trying \"%s\" ...", ssid);
  WiFi.begin(ssid, pass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// Diagnostic: list every 2.4 GHz network the ESP32 can actually see.
// If a target SSID is missing here, it is 5 GHz or out of range — not a
// password problem. Flags whether our two SSIDs are visible.
static bool g_seen1 = false, g_seen2 = false;   // set by scanWiFi(): are our SSIDs visible?

static void scanWiFi() {
  g_seen1 = false; g_seen2 = false;
  Serial.println("WiFi: scanning (2.4 GHz only — the ESP32 cannot see 5 GHz)...");
  int n = WiFi.scanNetworks();
  if (n <= 0) { Serial.println("  (no networks found)"); return; }
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid == WIFI_SSID)  g_seen1 = true;
    if (ssid == WIFI_SSID2) g_seen2 = true;
    Serial.printf("  %2d) ch%-2d  %4d dBm  %-4s  \"%s\"\n",
                  i + 1, WiFi.channel(i), WiFi.RSSI(i),
                  (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "enc"),
                  ssid.c_str());
  }
  Serial.printf("WiFi: \"%s\" visible: %s | \"%s\" visible: %s\n",
                WIFI_SSID,  g_seen1 ? "YES" : "NO (5 GHz or out of range)",
                WIFI_SSID2, g_seen2 ? "YES" : "NO (5 GHz or out of range)");
  WiFi.scanDelete();
}

static void connectWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  scanWiFi();

  // Only attempt an SSID that the scan actually saw — this avoids wasting
  // ~15 s timing out on a network that isn't present (so the primary/fallback
  // order no longer matters; it connects to whichever is in range).
  bool ok = false;
  if (g_seen1) ok = tryConnect(WIFI_SSID, WIFI_PASS, timeoutMs);
  if (!ok && g_seen2) {
    WiFi.disconnect(true);
    delay(300);
    ok = tryConnect(WIFI_SSID2, WIFI_PASS2, timeoutMs);
  }
  if (!ok && !g_seen1 && !g_seen2)
    Serial.println("WiFi: neither SSID in range this scan — will retry on reconnect.");

  if (ok) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(MDNS_HOST)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("Dashboard (LAN): http://%s.local/  or  http://%s/\n",
                    MDNS_HOST, WiFi.localIP().toString().c_str());
    }
  } else {
    Serial.println("WiFi NOT connected — physical buttons still work; will keep retrying.");
  }
}

// Active WiFi self-repair: if the link drops it doesn't just wait — it keeps
// actively reconnecting, escalates to a full radio reset + rescan if that
// fails, and as a last resort reboots (only when no motor is running, so a
// pump is never dropped to fix WiFi). All non-blocking except a ~2 s rescan
// during the escalation step, which is rare and watchdog-guarded.
static uint32_t g_lastWifiTry     = 0;
static bool     g_wifiTryFallback = false;
static uint32_t g_wifiDownSince   = 0;   // millis when the link went down (0 = up)
static uint16_t g_wifiFails       = 0;   // consecutive failed attempts

static bool anyMotorRunning() {
  for (uint8_t i = 0; i < MOTOR_COUNT; i++) if (motors[i].state == M_RUNNING) return true;
  return false;
}

static void wifiMaintain(uint32_t now) {
  static bool wasConnected = false;
  bool conn = (WiFi.status() == WL_CONNECTED);

  if (conn) {
    if (!wasConnected) { Serial.print("[wifi] (re)connected, IP: "); Serial.println(WiFi.localIP()); }
    wasConnected = true; g_wifiDownSince = 0; g_wifiFails = 0;
    return;
  }
  if (wasConnected) { Serial.println("[wifi] link lost — recovering..."); wasConnected = false; g_wifiDownSince = now; }
  if (g_wifiDownSince == 0) g_wifiDownSince = now;

  if (now - g_lastWifiTry < 10000) return;   // one action every 10 s
  g_lastWifiTry = now;
  g_wifiFails++;

  // Step 2: every ~60 s still down -> reset the radio and re-scan (diagnose).
  if (g_wifiFails % 6 == 0) {
    Serial.println("[wifi] self-repair: resetting radio + rescanning...");
    esp_task_wdt_reset();
    WiFi.persistent(false);
    WiFi.disconnect(true, true);      // drop + forget
    WiFi.mode(WIFI_OFF); delay(200); WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    scanWiFi();                        // ~2 s; sets g_seen1/g_seen2 and logs visibility
    esp_task_wdt_reset();
    if      (g_seen1) WiFi.begin(WIFI_SSID,  WIFI_PASS);
    else if (g_seen2) WiFi.begin(WIFI_SSID2, WIFI_PASS2);
    else              Serial.println("[wifi] neither SSID visible — AP down / out of range");
    return;
  }

  // Step 1: normal active retry, alternating SSIDs (non-blocking).
  if (g_wifiTryFallback) { Serial.printf("[wifi] retry \"%s\"\n", WIFI_SSID2); WiFi.begin(WIFI_SSID2, WIFI_PASS2); }
  else                   { Serial.printf("[wifi] retry \"%s\"\n", WIFI_SSID);  WiFi.begin(WIFI_SSID,  WIFI_PASS);  }
  g_wifiTryFallback = !g_wifiTryFallback;

  // Step 3: last resort — down > 10 min AND nothing running -> reboot to self-heal.
  if ((now - g_wifiDownSince) > 600000UL && !anyMotorRunning()) {
    Serial.println("[wifi] down >10 min, all motors off — rebooting to self-heal");
    delay(50);
    ESP.restart();
  }
}

// ======================= MQTT (public control path) ==================
// The ESP32 dials OUT to the broker (TLS 8883), so no port-forwarding is
// needed. It subscribes to a command topic and publishes retained state.
//   Command topic  pump/cmd    payload:  "<id>:<cmd>"
//                                            e.g. "0:start"
//   State topic    pump/state  payload:  stateJson() (retained)
//   Status topic   pump/status "online"/"offline" (retained, LWT)
#define TOPIC_CMD    "pump/cmd"
#define TOPIC_STATE  "pump/state"
#define TOPIC_STATUS "pump/status"

static WiFiClientSecure tlsClient;
static PubSubClient      mqtt(tlsClient);
static bool              g_stateDirty  = true;   // publish state on next chance
static uint32_t          g_lastMqttTry = 0;
static uint32_t          g_lastPubMs   = 0;

static void publishState() {
  if (!mqtt.connected()) return;
  String j = stateJson();
  mqtt.publish(TOPIC_STATE, (const uint8_t*)j.c_str(), j.length(), true /*retain*/);
  g_lastPubMs  = millis();
  g_stateDirty = false;
}

// Incoming command: "<id>:<cmd>". Trust the broker's ACLs.
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String s;
  s.reserve(len);
  for (unsigned int i = 0; i < len; i++) s += (char)payload[i];

  int c1 = s.indexOf(':');
  if (c1 < 0) { Serial.println("[mqtt] bad command format"); return; }

  int    id   = s.substring(0, c1).toInt();
  String cmd  = s.substring(c1 + 1);

  if (id < 0 || id >= (int)MOTOR_COUNT) { Serial.println("[mqtt] bad motor id"); return; }

  if      (cmd == "start") { cmdStart((uint8_t)id); Serial.printf("[mqtt] start %s\n", motors[id].name); }
  else if (cmd == "stop")  { cmdStop((uint8_t)id);  Serial.printf("[mqtt] stop  %s\n", motors[id].name); }
  else                     { Serial.println("[mqtt] unknown command"); }
}

static void mqttSetup() {
  tlsClient.setInsecure();   // skip cert validation (simple; see note below).
  // NOTE: setInsecure() trusts any server cert. To harden, pin HiveMQ's
  // root CA (ISRG Root X1 / Let's Encrypt) via tlsClient.setCACert(...).
  mqtt.setServer(MQTT_SERVER, (uint16_t)atoi(MQTT_PORT));
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
}

// Non-blocking reconnect, throttled to one attempt every 5 s.
static void mqttReconnect() {
  if (mqtt.connected()) return;
  uint32_t now = millis();
  if (now - g_lastMqttTry < 5000) return;
  g_lastMqttTry = now;

  String cid = "pump-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[mqtt] connecting to %s:%s ...\n", MQTT_SERVER, MQTT_PORT);
  esp_task_wdt_reset();      // TLS handshake can take a moment
  bool ok = mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS,
                         TOPIC_STATUS, 0, true, "offline");
  esp_task_wdt_reset();

  if (ok) {
    Serial.println("[mqtt] connected");
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD);
    g_stateDirty = true;     // push current state right away
  } else {
    Serial.printf("[mqtt] connect failed, rc=%d (retrying)\n", mqtt.state());
  }
}

// ---- Transition helper (drives the relay and logs the cause) ----
static void transition(Motor& m, MotorState to, const char* cause) {
  MotorState from = m.state;
  m.state = to;
  uint32_t now = millis();
  if (to == M_RUNNING) {
    m.startedAt = now;
    m.applyRelay(RELAY_ON);
  } else {                       // M_OFF or M_LOCKOUT
    m.stoppedAt = now;
    m.applyRelay(RELAY_OFF);
  }
  Serial.printf("[%s] %s -> %s  (%s)\n", m.name, stateName(from), stateName(to), cause);
  g_stateDirty = true;   // reflect the change to MQTT subscribers
}

// ---- Status LED (GPIO16), single LED with distinct patterns ----
//   READY      (WiFi + MQTT, no faults) : solid ON
//   NO SIGNAL  (no WiFi)                : fast blink   (~5 Hz)
//   CLOUD DOWN (WiFi up, MQTT not conn.): slow blink   (~1 Hz)
//   FAULT      (any motor in LOCKOUT)   : double-blink (blip-blip, then pause)
static void updateStateLed(uint32_t now) {
  uint8_t level;
  if (WiFi.status() != WL_CONNECTED) {
    level = ((now / 100) & 1) ? HIGH : LOW;            // NO SIGNAL: fast blink
  } else if (!mqtt.connected()) {
    level = ((now / 500) & 1) ? HIGH : LOW;            // CLOUD DOWN: slow blink
  } else {
    bool fault = false;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++)
      if (motors[i].state == M_LOCKOUT) { fault = true; break; }
    if (fault) {
      uint32_t p = now % 1500;                          // FAULT: double-blink
      level = (p < 120 || (p >= 300 && p < 420)) ? HIGH : LOW;
    } else {
      level = HIGH;                                     // READY: solid ON
    }
  }
  digitalWrite(STATE_LED_PIN, level);
}

// ======================= SETUP / LOOP ================================
void setup() {
  // 1) Force every relay OFF as the very first action after reset.
  for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
    pinMode(MOTOR_PINS[i].relayPin, OUTPUT);
    digitalWrite(MOTOR_PINS[i].relayPin, RELAY_OFF);
  }
  pinMode(STATE_LED_PIN, OUTPUT);
  digitalWrite(STATE_LED_PIN, LOW);

  Serial.begin(115200);
  delay(50);
  Serial.println("\n=== Pump controller (3 motors) booting — all relays OFF ===");

  // 2) Init motors (buttons, state).
  for (uint8_t i = 0; i < MOTOR_COUNT; i++) motors[i].begin(MOTOR_PINS[i]);

  // 3) Join WiFi (best-effort) and start the dashboard. Done BEFORE the
  //    watchdog is armed so the connect wait can't trip it.
  connectWiFi(15000);
  server.on("/",       handleRoot);
  server.on("/set",    handleSet);
  server.on("/status", handleStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println("HTTP dashboard started on port 80.");

  // Configure the MQTT (public) path; first connect happens in loop().
  mqttSetup();

  // 4) Enable the Task Watchdog. The API differs across Arduino-ESP32
  //    major versions, so guard on the version macro.
#if defined(ESP_ARDUINO_VERSION) && \
    ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  // The core may already have initialized the TWDT; reconfigure if so.
  if (esp_task_wdt_init(&wdt_cfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt_cfg);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);   // subscribe this (loop) task

  Serial.printf("Ready. min-off=%lus  max-run=%lus (0=off)  wdt=%lus\n",
                (unsigned long)(MIN_OFF_MS / 1000),
                (unsigned long)(MAX_RUN_MS / 1000),
                (unsigned long)WDT_TIMEOUT_S);
}

void loop() {
  esp_task_wdt_reset();          // pet the watchdog
  uint32_t now = millis();
  server.handleClient();         // serve LAN dashboard requests (non-blocking)

  wifiMaintain(now);             // keep WiFi up; retry forever if it drops

  // MQTT (public path): keep the connection up and pump the client.
  if (WiFi.status() == WL_CONNECTED) {
    mqttReconnect();
    mqtt.loop();
  }

  for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
    Motor& m = motors[i];

    // ---- Read the single toggle button -> feed the command API ----
    // NO momentary: a press shorts the pin to GND (falling edge HIGH->LOW).
    // Press TOGGLES: off -> start, running/lockout -> stop.
    m.button.update(now, DEBOUNCE_MS);
    if (m.prevBtn == HIGH && m.button.stable == LOW) {
      if (m.state == M_OFF) { cmdStart(i); Serial.printf("[%s] button: start\n", m.name); }
      else                  { cmdStop(i);  Serial.printf("[%s] button: stop\n",  m.name); }
    }
    m.prevBtn = m.button.stable;

    // ---- State machine ----
    switch (m.state) {
      case M_OFF:
        m.stopReq = false;                 // already off; nothing to stop
        if (m.startReq) {
          m.startReq = false;
          if ((now - m.stoppedAt) >= MIN_OFF_MS) {
            transition(m, M_RUNNING, "start requested");
          } else {
            Serial.printf("[%s] start ignored: min-off not elapsed (%lu ms left)\n",
                          m.name,
                          (unsigned long)(MIN_OFF_MS - (now - m.stoppedAt)));
          }
        }
        break;

      case M_RUNNING:
        if (m.stopReq) {
          m.stopReq  = false;
          m.startReq = false;
          transition(m, M_OFF, "stop requested");
        } else if (MAX_RUN_MS > 0 && (now - m.startedAt) >= MAX_RUN_MS) {
          m.startReq = false;
          transition(m, M_LOCKOUT, "max run time exceeded");
        }
        break;

      case M_LOCKOUT:
        // Latched off after max-run. A Stop press (or dashboard stop) resets
        // it back to a normal OFF state. Starts are ignored.
        if (m.stopReq) {
          m.stopReq  = false;
          m.startReq = false;
          transition(m, M_OFF, "lockout reset");
        } else {
          m.startReq = false;
        }
        break;
    }

    // Per-motor run LED: ON while this motor is RUNNING.
    m.applyLed(m.isRunning() ? LED_ON : LED_OFF);
  }

  updateStateLed(now);

  // Publish state to MQTT on change, and as a heartbeat every 15 s.
  if (mqtt.connected() && (g_stateDirty || (now - g_lastPubMs) >= 15000)) {
    publishState();
  }
}
