/*
 * motor.h — types for the two-motor pump controller.
 *
 * These live in a header (not the .ino) so the Arduino build's auto-
 * generated function prototypes, which reference `Motor`, are placed
 * AFTER these definitions and therefore compile.
 */
#pragma once
#include <Arduino.h>

// ---- Relay logic (active-low module) ----
static const uint8_t RELAY_ON  = LOW;   // IN=LOW  -> relay energized (motor ON)
static const uint8_t RELAY_OFF = HIGH;  // IN=HIGH -> relay off        (boot default)

// Pin bundle for one motor.
struct MotorPins {
  const char* name;
  uint8_t     relayPin;   // active-low relay IN
  uint8_t     startPin;   // momentary NO -> GND (INPUT_PULLUP, pressed = LOW)
  uint8_t     stopPin;    // momentary NC -> GND (INPUT_PULLUP, HIGH = stop/cut)
};

// ---------------------------------------------------------------------
// Debouncer — simple millis()-based debounce. Reports the current STABLE
// level and whether it changed on this update (for edge detection).
// ---------------------------------------------------------------------
struct Debouncer {
  uint8_t  pin;
  int      stable;     // last debounced level
  int      lastRaw;    // last raw sample
  uint32_t tLastRaw;   // time lastRaw was observed

  void begin(uint8_t p) {
    pin      = p;
    pinMode(pin, INPUT_PULLUP);
    stable   = digitalRead(pin);
    lastRaw  = stable;
    tLastRaw = millis();
  }

  // Returns true if the stable level CHANGED on this call.
  bool update(uint32_t now, uint32_t debounceMs) {
    int raw = digitalRead(pin);
    if (raw != lastRaw) {
      lastRaw  = raw;
      tLastRaw = now;
    }
    if ((now - tLastRaw) >= debounceMs && raw != stable) {
      stable = raw;
      return true;
    }
    return false;
  }
};

// ---- Motor state machine ----
enum MotorState { M_OFF, M_RUNNING, M_LOCKOUT };

inline const char* stateName(MotorState s) {
  switch (s) {
    case M_OFF:     return "OFF";
    case M_RUNNING: return "RUNNING";
    case M_LOCKOUT: return "LOCKOUT";
  }
  return "?";
}

struct Motor {
  const char* name;
  uint8_t     relayPin;
  Debouncer   startBtn;   // NO: pressed = LOW
  Debouncer   stopBtn;    // NC: HIGH = stop/cut

  MotorState  state;
  uint32_t    stoppedAt;  // millis() when it last entered OFF/LOCKOUT
  uint32_t    startedAt;  // millis() when it last entered RUNNING
  int         prevStart;  // previous debounced START level (for edge detect)

  // Latched command requests from ANY source (buttons or dashboard).
  volatile bool startReq;
  volatile bool stopReq;

  void begin(const MotorPins& p) {
    name     = p.name;
    relayPin = p.relayPin;

    pinMode(relayPin, OUTPUT);
    digitalWrite(relayPin, RELAY_OFF);   // OFF before anything else

    startBtn.begin(p.startPin);
    stopBtn.begin(p.stopPin);

    state     = M_OFF;
    stoppedAt = millis();
    startedAt = 0;
    prevStart = HIGH;   // released
    startReq  = false;
    stopReq   = false;
  }

  void applyRelay(uint8_t level) { digitalWrite(relayPin, level); }
  bool isRunning() const { return state == M_RUNNING; }
};
