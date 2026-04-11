// StateMachine.ino
#include <Arduino.h>
#include <string.h>
#include <Servo.h>

extern void sendMessage(const char *msg);
extern int  promptUserSelect(const String& prompt, const String options[], int optionCount);
extern int  askPassword();
extern void promptCancel();

enum SystemState {
  STARTUP = 0,
  PURGE,
  WAIT_FOR_FILL,
  FILL,
  PERSONNEL_DEPARTING,
  ARMED,
  FIRE,
  ABORT,
  PP1,
  PP2,
  TEST_COMPLETE,
  STATE_COUNT
};

static int currentState = (int)STARTUP;

static const char* STATE_NAMES[STATE_COUNT] = {
  "STARTUP",
  "PURGE",
  "WAIT_FOR_FILL",
  "FILL",
  "PERSONNEL_DEPARTING",
  "ARMED",
  "FIRE",
  "ABORT",
  "PP1",
  "PP2",
  "TEST_COMPLETE"
};

// ---------------- PIN DEFINITIONS ----------------
#define IGNITOR1_PIN        8
#define IGNITOR2_PIN        9
#define LOX_INJECTOR_PIN    2   // servo
#define LOX_DUMP_PUL_PIN    3   // stepper pulse
#define LOX_DUMP_DIR_PIN    10  // stepper direction
#define LOX_VENT_PIN        4   // servo
#define KERO_VENT_PUL_PIN   5   // stepper pulse
#define KERO_VENT_DIR_PIN   11  // stepper direction
#define PRESS_VALVE_PIN     6
#define KERO_INJECTOR_PIN   7

// ---------------- ACTUATOR SETTINGS ----------------
#define LOX_INJECTOR_POS_0  0
#define LOX_INJECTOR_POS_1  90

#define LOX_VENT_POS_0      0
#define LOX_VENT_POS_1      90

#define LOX_DUMP_STEPS      200
#define KERO_VENT_STEPS     200
#define STEPPER_PULSE_US    800

Servo loxInjectorServo;
Servo loxVentServo;

// order:
// [0] ignitor1
// [1] ignitor2
// [2] lox injector servo
// [3] lox dump stepper
// [4] lox vent servo
// [5] kero vent stepper
// [6] pressurizing valves
// [7] kero injector valve
bool g_outputState[8] = {0,0,0,0,0,0,0,0};

static const char* stateToString_int(int s) {
  if (s < 0 || s >= STATE_COUNT) return "UNKNOWN";
  return STATE_NAMES[s];
}

static bool parseStateString_int(const char* cmd, int &out) {
  for (int i = 0; i < STATE_COUNT; i++) {
    if (strcmp(cmd, STATE_NAMES[i]) == 0) {
      out = i;
      return true;
    }
  }
  return false;
}

void broadcastState() {
  Serial.print("STATE ");
  Serial.println(stateToString_int(currentState));
}

// --------------------------------------------------
const unsigned long PI_GET_TIMEOUT_MS = 1UL * 60UL * 1000UL;
static unsigned long g_lastPiGetMs = 0;

void markPiGetReceived() {
  g_lastPiGetMs = millis();
}

bool isPiGetTimedOut() {
  unsigned long now = millis();
  return (now - g_lastPiGetMs) >= PI_GET_TIMEOUT_MS;
}

static bool stateRequiresPiHeartbeat(int state) {
  return (state == PERSONNEL_DEPARTING ||
          state == ARMED ||
          state == FIRE);
}

// -------------------------
static bool g_elevatedPending = false;
static int  g_elevatedTarget  = -1;

// ---------------- HELPER FUNCTIONS ----------------
void pulseStepperToState(uint8_t pulPin, uint8_t dirPin, bool targetState, int steps) {
  digitalWrite(dirPin, targetState ? HIGH : LOW);
  delayMicroseconds(50);

  for (int i = 0; i < steps; i++) {
    digitalWrite(pulPin, HIGH);
    delayMicroseconds(STEPPER_PULSE_US);
    digitalWrite(pulPin, LOW);
    delayMicroseconds(STEPPER_PULSE_US);
  }
}

void applyOutputs(const bool cmd[8]) {
  // Ignitor 1
  if (cmd[0] != g_outputState[0]) {
    digitalWrite(IGNITOR1_PIN, cmd[0] ? HIGH : LOW);
    sendMessage(cmd[0] ? "IGNITOR 1 ON" : "IGNITOR 1 OFF");
    g_outputState[0] = cmd[0];
  }

  // Ignitor 2
  if (cmd[1] != g_outputState[1]) {
    digitalWrite(IGNITOR2_PIN, cmd[1] ? HIGH : LOW);
    sendMessage(cmd[1] ? "IGNITOR 2 ON" : "IGNITOR 2 OFF");
    g_outputState[1] = cmd[1];
  }

  // LOX injector servo
  if (cmd[2] != g_outputState[2]) {
    loxInjectorServo.write(cmd[2] ? LOX_INJECTOR_POS_1 : LOX_INJECTOR_POS_0);
    sendMessage(cmd[2] ? "LOX INJECTOR POSITION 1" : "LOX INJECTOR POSITION 0");
    g_outputState[2] = cmd[2];
  }

  // LOX dump stepper
  if (cmd[3] != g_outputState[3]) {
    pulseStepperToState(LOX_DUMP_PUL_PIN, LOX_DUMP_DIR_PIN, cmd[3], LOX_DUMP_STEPS);
    sendMessage(cmd[3] ? "LOX DUMP POSITION 1" : "LOX DUMP POSITION 0");
    g_outputState[3] = cmd[3];
  }

  // LOX vent servo
  if (cmd[4] != g_outputState[4]) {
    loxVentServo.write(cmd[4] ? LOX_VENT_POS_1 : LOX_VENT_POS_0);
    sendMessage(cmd[4] ? "LOX VENT POSITION 1" : "LOX VENT POSITION 0");
    g_outputState[4] = cmd[4];
  }

  // Kero vent stepper
  if (cmd[5] != g_outputState[5]) {
    pulseStepperToState(KERO_VENT_PUL_PIN, KERO_VENT_DIR_PIN, cmd[5], KERO_VENT_STEPS);
    sendMessage(cmd[5] ? "KERO VENT POSITION 1" : "KERO VENT POSITION 0");
    g_outputState[5] = cmd[5];
  }

  // Pressurizing valves
  if (cmd[6] != g_outputState[6]) {
    digitalWrite(PRESS_VALVE_PIN, cmd[6] ? HIGH : LOW);
    sendMessage(cmd[6] ? "PRESSURIZING VALVES ON" : "PRESSURIZING VALVES OFF");
    g_outputState[6] = cmd[6];
  }

  // Kero injector valve
  if (cmd[7] != g_outputState[7]) {
    digitalWrite(KERO_INJECTOR_PIN, cmd[7] ? HIGH : LOW);
    sendMessage(cmd[7] ? "KERO INJECTOR VALVE ON" : "KERO INJECTOR VALVE OFF");
    g_outputState[7] = cmd[7];
  }
}

void initStateMachine() {
  currentState = STARTUP;
  g_elevatedPending = false;
  g_elevatedTarget  = -1;

  pinMode(IGNITOR1_PIN, OUTPUT);
  pinMode(IGNITOR2_PIN, OUTPUT);

  pinMode(LOX_DUMP_PUL_PIN, OUTPUT);
  pinMode(LOX_DUMP_DIR_PIN, OUTPUT);
  pinMode(KERO_VENT_PUL_PIN, OUTPUT);
  pinMode(KERO_VENT_DIR_PIN, OUTPUT);

  pinMode(PRESS_VALVE_PIN, OUTPUT);
  pinMode(KERO_INJECTOR_PIN, OUTPUT);

  digitalWrite(IGNITOR1_PIN, LOW);
  digitalWrite(IGNITOR2_PIN, LOW);
  digitalWrite(LOX_DUMP_PUL_PIN, LOW);
  digitalWrite(LOX_DUMP_DIR_PIN, LOW);
  digitalWrite(KERO_VENT_PUL_PIN, LOW);
  digitalWrite(KERO_VENT_DIR_PIN, LOW);
  digitalWrite(PRESS_VALVE_PIN, LOW);
  digitalWrite(KERO_INJECTOR_PIN, LOW);

  loxInjectorServo.attach(LOX_INJECTOR_PIN);
  loxVentServo.attach(LOX_VENT_PIN);

  bool startupOutputs[8] = {0,0,0,0,0,0,0,0};
  applyOutputs(startupOutputs);

  g_lastPiGetMs = millis();

  sendMessage("State machine initialized");
  broadcastState();
}

void setStateOverride(const char *cmd) {
  int next;
  if (!parseStateString_int(cmd, next)) {
    sendMessage("Unknown ELEVATED state");
    return;
  }

  promptCancel();
  g_elevatedPending = true;
  g_elevatedTarget  = next;
  sendMessage("[ELEVATED] Password required");
}

void forceAbort() {
  promptCancel();
  g_elevatedPending = false;
  g_elevatedTarget  = -1;

  bool abortOutputs[8] = {0,0,0,0,0,0,0,0};
  applyOutputs(abortOutputs);

  currentState = ABORT;
  sendMessage("ABORT triggered");
  broadcastState();
}

void stateMachineLoop() {

  // Auto-abort on Pi timeout
  if (stateRequiresPiHeartbeat(currentState) && isPiGetTimedOut()) {
    promptCancel();
    g_elevatedPending = false;
    g_elevatedTarget  = -1;

    bool abortOutputs[8] = {0,0,0,0,0,0,0,0};
    applyOutputs(abortOutputs);

    currentState = ABORT;
    sendMessage("ABORT triggered: no GET received from Raspberry Pi within timeout");
    broadcastState();
    return;
  }

  if (g_elevatedPending) {
    int pw = askPassword();
    if (pw == -1) return;
    if (pw == 1) {
      g_elevatedPending = false;
      currentState = g_elevatedTarget;
      g_elevatedTarget = -1;
      sendMessage("[ELEVATED] Password accepted");
      broadcastState();
    }
    return;
  }

  switch ((SystemState)currentState) {

    case STARTUP: {
      static const String opts[] = { "PURGE" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);

      int r = promptUserSelect("STARTUP: Next? (PURGE)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PURGE;
      broadcastState();
      break;
    }

    case PURGE: {
      bool outputs[8] = {0,0,0,1,1,1,0,0};
      applyOutputs(outputs);

      sendMessage("PURGE INITIALIZED");
      sendMessage("PURGE COMPLETE");
      currentState = WAIT_FOR_FILL;
      broadcastState();
      break;
    }

    case WAIT_FOR_FILL: {
      static const String opts[] = { "FILL" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);

      int r = promptUserSelect("WAIT_FOR_FILL: Next? (FILL)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = FILL;
      broadcastState();
      break;
    }

    case FILL: {
      static const String opts[] = { "PERSONNEL_DEPARTING" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);

      int r = promptUserSelect("FILL: Next? (PERSONNEL_DEPARTING)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PERSONNEL_DEPARTING;
      broadcastState();
      break;
    }

    case PERSONNEL_DEPARTING: {
      static const String opts[] = { "ARMED" };
      static bool gateArmed = false;

      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);

      if (!gateArmed) {
        int r = promptUserSelect("PERSONNEL_DEPARTING: Next? (ARMED)", opts, 1);
        if (r == -1) break;

        if (r == 1) {
          gateArmed = true;
          promptCancel();
        } else {
          break;
        }
      }

      if (gateArmed) {
        int pw = askPassword();
        if (pw == -1) break;

        if (pw == 1) {
          gateArmed = false;
          currentState = ARMED;
          sendMessage("Password accepted");
          broadcastState();
        }
      }
      break;
    }

    case ARMED: {
      static const String opts[] = { "FIRE" };
      bool outputs[8] = {0,0,1,0,0,0,1,1};
      applyOutputs(outputs);

      int r = promptUserSelect("ARMED: Next? (FIRE)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = FIRE;
      broadcastState();
      break;
    }

    case FIRE: {
      static const String opts[] = { "PP1" };
      bool outputs[8] = {1,1,1,0,0,0,1,1};
      applyOutputs(outputs);

      int r = promptUserSelect("FIRE: Next? (PP1)", opts, 1);
      if (r == -1) break;

      if (r == 1) {
        currentState = PP1;
      }

      broadcastState();
      break;
    }

    case ABORT: {
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);

      sendMessage("ABORT INITIALIZED");
      sendMessage("ABORT COMPLETE");
      currentState = PP1;
      broadcastState();
      break;
    }

    case PP1: {
      static const String opts[] = { "PP2" };
      bool outputs[8] = {0,0,0,1,1,1,0,0};
      applyOutputs(outputs);

      int r = promptUserSelect("PP1: Next? (PP2)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PP2;
      broadcastState();
      break;
    }

    case PP2: {
      static const String opts[] = { "TEST_COMPLETE" };
      bool outputs[8] = {0,0,0,0,1,0,0,0};
      applyOutputs(outputs);

      int r = promptUserSelect("PP2: Next? (TEST_COMPLETE)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = TEST_COMPLETE;
      broadcastState();
      break;
    }

    case TEST_COMPLETE: {
      static const String opts[] = { "STARTUP" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);

      int r = promptUserSelect("TEST_COMPLETE: Next? (STARTUP)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = STARTUP;
      broadcastState();
      break;
    }
  }
}