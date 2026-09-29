/*
 * fountain_control.ino  —  ESP32 two-motor fountain starter controller
 * =====================================================================
 * Controls TWO independent fountain motors, each through ONE active-low
 * relay wired in series with that motor's contactor coil, downstream of
 * the thermal overload contact (the overload stays HARDWIRED and is NOT
 * controlled by software).
 *
 * Each motor can be turned on/off from TWO sources that drive the SAME
 * state machine:
 *   1. Physical Start/Stop buttons on the panel (implemented here).
 *   2. The web dashboard (later phase) — it just calls cmdStart(i) /
 *      cmdStop(i). No WiFi/web code is included in this bring-up sketch.
 *
 * This is a FOUNTAIN, not a pumping/irrigation system: there is no
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
 *    disable (a fountain may legitimately run for hours).
 *
 * Serial debug: 115200 baud, prints every state transition and its
 * cause. Bring-up aid, not a permanent feature.
 *
 * NOTE: All 3-phase mains / contactor-coil wiring must be designed and
 * signed off by a licensed electrician. This sketch switches only the
 * low-voltage control side and must not bypass the overload relay.
 */

#include "esp_task_wdt.h"
#include "motor.h"

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
// Physical buttons (in loop) and the future web dashboard both call these.
void cmdStart(uint8_t i) { if (i < MOTOR_COUNT) motors[i].startReq = true; }
void cmdStop(uint8_t i)  { if (i < MOTOR_COUNT) motors[i].stopReq  = true; }

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
  Serial.println("\n=== Fountain controller (2 motors) booting — all relays OFF ===");

  // 2) Init motors (buttons, state).
  for (uint8_t i = 0; i < MOTOR_COUNT; i++) motors[i].begin(MOTOR_PINS[i]);

  // 3) Enable the Task Watchdog. The API differs across Arduino-ESP32
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
}
