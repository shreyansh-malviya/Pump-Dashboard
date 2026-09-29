/*
 * pump_control.ino  —  ESP32 two-motor pump starter controller
 * =====================================================================
 * Controls TWO independent pump motors, each through ONE active-low
 * relay wired in series with that motor's contactor coil, downstream of
 * the thermal overload contact (the overload stays HARDWIRED and is NOT
 * controlled by software).
 *
 * Each motor can be turned on/off from TWO sources that drive the SAME
 * state machine (so all safety rules below apply to both):
 *   1. Physical Start/Stop buttons on the panel.
 *   2. The password-protected web dashboard served by the ESP32 — its
 *      buttons call the same cmdStart(i) / cmdStop(i).
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
 *  - Start button: momentary NO, between START pin and GND.
 *      INPUT_PULLUP -> idle reads HIGH, pressed reads LOW.
 *  - Stop button: momentary NC, between STOP pin and GND. FAIL-SAFE:
 *      INPUT_PULLUP -> closed (not pressed) reads LOW  = "run allowed"
 *                      open  (pressed OR wire cut) reads HIGH = "STOP".
 *      A broken stop wire therefore forces the motor OFF and keeps it off.
 *  - Common ground: ESP32 GND, relay module GND, and button GND all tied.
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

// ---- Pin assignments (ESP32 safe GPIOs; avoids strapping pins 0/2/12/15,
//      flash pins 6-11, and input-only pins 34-39 which lack pull-ups) ----
static const MotorPins MOTOR_PINS[] = {
  // name        relay  start  stop
  { "Motor-A",   25,    32,    33 },
  { "Motor-B",   26,    27,    14 },
};
static const uint8_t MOTOR_COUNT = sizeof(MOTOR_PINS) / sizeof(MOTOR_PINS[0]);

static const uint8_t LED_PIN = 2;   // onboard LED: ON while any motor runs

// ---- Timing constants ----
static const uint32_t DEBOUNCE_MS   = 30;            // button debounce window
static const uint32_t MIN_OFF_MS    = 5000;          // 5 s min gap stop -> next start
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

// Connect to WiFi (non-fatal: buttons work regardless). Blocks up to
// timeoutMs; called before the watchdog is armed.
static void connectWiFi(uint32_t timeoutMs) {
  Serial.printf("WiFi: connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
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

// ======================= SETUP / LOOP ================================
void setup() {
  // 1) Force every relay OFF as the very first action after reset.
  for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
    pinMode(MOTOR_PINS[i].relayPin, OUTPUT);
    digitalWrite(MOTOR_PINS[i].relayPin, RELAY_OFF);
  }
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  delay(50);
  Serial.println("\n=== Pump controller (2 motors) booting — all relays OFF ===");

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
  server.handleClient();         // serve LAN dashboard requests (non-blocking)

  // MQTT (public path): keep the connection up and pump the client.
  if (WiFi.status() == WL_CONNECTED) {
    mqttReconnect();
    mqtt.loop();
  }

  uint32_t now = millis();

  bool anyRunning = false;

  for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
    Motor& m = motors[i];

    // ---- Read buttons -> feed the command API ----
    m.startBtn.update(now, DEBOUNCE_MS);
    bool stopChanged = m.stopBtn.update(now, DEBOUNCE_MS);

    // Start button: falling edge (HIGH->LOW) = a fresh press = start request.
    if (m.prevStart == HIGH && m.startBtn.stable == LOW) {
      cmdStart(i);
      Serial.printf("[%s] start button pressed\n", m.name);
    }
    m.prevStart = m.startBtn.stable;

    // Stop: level-based. HIGH = stop/cut = a continuous stop condition.
    bool stopActive = (m.stopBtn.stable == HIGH);
    if (stopChanged && stopActive) {
      cmdStop(i);
      Serial.printf("[%s] stop asserted (button pressed or wire cut)\n", m.name);
    }

    // ---- State machine ----
    switch (m.state) {
      case M_OFF:
        // A stop condition clears any pending start and blocks starting.
        if (stopActive || m.stopReq) {
          m.startReq = false;
          m.stopReq  = false;
        } else if (m.startReq) {
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
        if (stopActive || m.stopReq) {
          m.stopReq  = false;
          m.startReq = false;
          transition(m, M_OFF, stopActive ? "stop button / wire cut" : "stop requested");
        } else if (MAX_RUN_MS > 0 && (now - m.startedAt) >= MAX_RUN_MS) {
          m.startReq = false;
          transition(m, M_LOCKOUT, "max run time exceeded");
        }
        break;

      case M_LOCKOUT:
        // Latched off. A manual Stop assert (or dashboard cmdStop) clears
        // the lockout back to a normal OFF state. Starts are ignored.
        if ((stopChanged && stopActive) || m.stopReq) {
          m.stopReq  = false;
          m.startReq = false;
          transition(m, M_OFF, "lockout reset");
        } else {
          m.startReq = false;
        }
        break;
    }

    if (m.isRunning()) anyRunning = true;
  }

  digitalWrite(LED_PIN, anyRunning ? HIGH : LOW);

  // Publish state to MQTT on change, and as a heartbeat every 15 s.
  if (mqtt.connected() && (g_stateDirty || (now - g_lastPubMs) >= 15000)) {
    publishState();
  }
}
