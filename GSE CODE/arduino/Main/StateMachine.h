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
  IGN_TEST,
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
  "IGN_TEST",
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
#define LOX_INJECTOR_PIN    2
#define LOX_DUMP_PUL_PIN    3
#define LOX_DUMP_DIR_PIN    10
#define LOX_VENT_PIN        4
#define KERO_VENT_PUL_PIN   5
#define KERO_VENT_DIR_PIN   11
#define PRESS_VALVE_PIN     6
#define KERO_INJECTOR_PIN   7

// ---------------- ACTUATOR SETTINGS ----------------
#define LOX_INJECTOR_POS_0  180
#define LOX_INJECTOR_POS_1  80

#define LOX_VENT_POS_0      35
#define LOX_VENT_POS_1      130

#define LOX_DUMP_STEPS      10000
#define KERO_VENT_STEPS     10000
#define STEPPER_PULSE_US    200   // half-period (µs)

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

// ── Non-blocking stepper context ─────────────────────────────────────────────
struct StepperCtx {
  uint8_t  pulPin;
  uint8_t  dirPin;
  long     stepsLeft;
  bool     targetState;   // the logical state we are moving TO
  bool     pulHigh;
  unsigned long nextUs;
  bool     running;
};

static StepperCtx g_loxDump  = { LOX_DUMP_PUL_PIN,  LOX_DUMP_DIR_PIN,  0, false, false, 0, false };
static StepperCtx g_keroVent = { KERO_VENT_PUL_PIN, KERO_VENT_DIR_PIN, 0, false, false, 0, false };

// Kick off a non-blocking stepper move (ignored if already at target state).
static void startStepper(StepperCtx &ctx, bool targetState, long steps) {
  if (ctx.running) return;               // already moving — do not interrupt
  ctx.targetState = targetState;
  ctx.stepsLeft   = steps;
  ctx.pulHigh     = false;
  ctx.nextUs      = micros();
  ctx.running     = true;
  digitalWrite(ctx.dirPin, targetState ? LOW : HIGH);
  delayMicroseconds(50);                 // direction setup time (≪ PULSE_US)
}

// Advance one step per call; call every loop iteration.
static void tickStepper(StepperCtx &ctx) {
  if (!ctx.running) return;
  if ((long)(micros() - ctx.nextUs) < 0) return;

  if (ctx.pulHigh) {
    digitalWrite(ctx.pulPin, LOW);
    ctx.pulHigh = false;
    ctx.nextUs += STEPPER_PULSE_US;
    if (--ctx.stepsLeft == 0) {
      ctx.running = false;
    }
  } else {
    digitalWrite(ctx.pulPin, HIGH);
    ctx.pulHigh = true;
    ctx.nextUs += STEPPER_PULSE_US;
  }
}

// Tick both steppers — call at the top of stateMachineLoop().
static void tickAllSteppers() {
  tickStepper(g_loxDump);
  tickStepper(g_keroVent);
}

// ─────────────────────────────────────────────────────────────────────────────

static const char* stateToString_int(int s) {
  if (s < 0 || s >= STATE_COUNT) return "UNKNOWN";
  return STATE_NAMES[s];
}

static bool parseStateString_int(const char* cmd, int &out) {
  for (int i = 0; i < STATE_COUNT; i++) {
    if (strcmp(cmd, STATE_NAMES[i]) == 0) { out = i; return true; }
  }
  return false;
}

void broadcastState() {
  Serial.print("STATE ");
  Serial.println(stateToString_int(currentState));
}

// --------------------------------------------------
const unsigned long PI_GET_TIMEOUT_MS = 15UL * 60UL * 1000UL;
static unsigned long g_lastPiGetMs = 0;

void markPiGetReceived() { g_lastPiGetMs = millis(); }

bool isPiGetTimedOut() {
  return (millis() - g_lastPiGetMs) >= PI_GET_TIMEOUT_MS;
}

static bool stateRequiresPiHeartbeat(int state) {
  return (state == PERSONNEL_DEPARTING ||
          state == ARMED ||
          state == FIRE);
}

// -------------------------
static bool g_elevatedPending = false;
static int  g_elevatedTarget  = -1;

// ---------------- TIMED STATE RESET FLAGS ----------------
static bool purgeInitialized = false;
static bool fireInitialized  = false;
static bool armedInitialized  = false;
static bool abortInitialized = false;

// ---------------- HELPER FUNCTIONS ----------------

// applyOutputs: sets digital/servo outputs immediately;
// starts non-blocking stepper moves when the commanded state changes.
void applyOutputs(const bool cmd[8]) {
  digitalWrite(IGNITOR1_PIN,    cmd[0] ? HIGH : LOW);
  digitalWrite(IGNITOR2_PIN,    cmd[1] ? HIGH : LOW);
  digitalWrite(PRESS_VALVE_PIN, cmd[6] ? HIGH : LOW);
  digitalWrite(KERO_INJECTOR_PIN, cmd[7] ? HIGH : LOW);

  loxInjectorServo.write(cmd[2] ? LOX_INJECTOR_POS_1 : LOX_INJECTOR_POS_0);
  loxVentServo.write(cmd[4] ? LOX_VENT_POS_1 : 
  );

  // Only kick off a stepper move when the target state changes.
  if (cmd[3] != g_outputState[3]) {
    startStepper(g_loxDump,  cmd[3], LOX_DUMP_STEPS);
  }
  if (cmd[5] != g_outputState[5]) {
    startStepper(g_keroVent, cmd[5], KERO_VENT_STEPS);
  }

  for (int i = 0; i < 8; i++) g_outputState[i] = cmd[i];
}

void resetTimedStates() {
  purgeInitialized = false;
  fireInitialized  = false;
  armedInitialized = false;
  abortInitialized = false;
}

void initStateMachine() {
  currentState = STARTUP;
  g_elevatedPending = false;
  g_elevatedTarget  = -1;
  resetTimedStates();

  pinMode(IGNITOR1_PIN,      OUTPUT);
  pinMode(IGNITOR2_PIN,      OUTPUT);
  pinMode(LOX_DUMP_PUL_PIN,  OUTPUT);
  pinMode(LOX_DUMP_DIR_PIN,  OUTPUT);
  pinMode(KERO_VENT_PUL_PIN, OUTPUT);
  pinMode(KERO_VENT_DIR_PIN, OUTPUT);
  pinMode(PRESS_VALVE_PIN,   OUTPUT);
  pinMode(KERO_INJECTOR_PIN, OUTPUT);

  digitalWrite(IGNITOR1_PIN,      LOW);
  digitalWrite(IGNITOR2_PIN,      LOW);
  digitalWrite(LOX_DUMP_PUL_PIN,  LOW);
  digitalWrite(LOX_DUMP_DIR_PIN,  LOW);
  digitalWrite(KERO_VENT_PUL_PIN, LOW);
  digitalWrite(KERO_VENT_DIR_PIN, LOW);
  digitalWrite(PRESS_VALVE_PIN,   LOW);
  digitalWrite(KERO_INJECTOR_PIN, LOW);

  loxInjectorServo.attach(LOX_INJECTOR_PIN);
  loxVentServo.attach(LOX_VENT_PIN);

  bool startupOutputs[8] = {0,0,0,0,0,0,0,0};
  applyOutputs(startupOutputs);

  delay(500);

  // Home the LOX dump stepper to its start position before normal operation.
  // DIR HIGH, 10000 steps — runs non-blocking via tickAllSteppers() in the main loop.
  digitalWrite(LOX_DUMP_DIR_PIN, HIGH);
  delay(5);  // let DIR settle before pulses begin
  g_loxDump.targetState = false;  // treated as "state 0" once move completes
  g_loxDump.stepsLeft   = LOX_DUMP_STEPS;
  g_loxDump.pulHigh     = false;
  g_loxDump.nextUs      = micros();
  g_loxDump.running     = true;

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
  resetTimedStates();
  currentState = ABORT;
  sendMessage("ABORT triggered");
  broadcastState();
}

void stateMachineLoop() {

  // ── Tick steppers every loop iteration ────────────────────────────────────
  tickAllSteppers();

  // ── Pi heartbeat watchdog ─────────────────────────────────────────────────
  if (stateRequiresPiHeartbeat(currentState) && isPiGetTimedOut()) {
    promptCancel();
    g_elevatedPending = false;
    g_elevatedTarget  = -1;
    resetTimedStates();
    currentState = ABORT;
    sendMessage("ABORT triggered: no GET received from Raspberry Pi within timeout");
    broadcastState();
    return;
  }

  // ── Elevated password gate ────────────────────────────────────────────────
  if (g_elevatedPending) {
    int pw = askPassword();
    if (pw == -1) return;
    if (pw == 1) {
      g_elevatedPending = false;
      currentState = g_elevatedTarget;
      g_elevatedTarget = -1;
      resetTimedStates();
      sendMessage("[ELEVATED] Password accepted");
      broadcastState();
    }
    return;
  }

  // ── State machine ─────────────────────────────────────────────────────────
  switch ((SystemState)currentState) {

    case STARTUP: {
      static const String opts[] = { "PURGE" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);
      int r = promptUserSelect("STARTUP: Next? (PURGE)", opts, 1);
      if (r == -1) break;
      if (r == 1) { resetTimedStates(); currentState = PURGE; broadcastState(); }
      break;
    }

    case PURGE: {
      static unsigned long purgeStartMs = 0;
      if (!purgeInitialized) {
        purgeStartMs    = millis();
        purgeInitialized = true;
        sendMessage("PURGE INITIALIZED");
      }
      unsigned long elapsed = millis() - purgeStartMs;
      if      (elapsed < 2UL*60UL*1000UL) { bool o[8]={0,0,0,0,1,0,0,0}; applyOutputs(o); }
      else if (elapsed < 3UL*60UL*1000UL) { bool o[8]={0,0,0,1,0,0,0,0}; applyOutputs(o); }
      else if (elapsed < 3UL*60UL*1000UL + 30UL*1000UL) { bool o[8]={0,0,1,0,0,0,0,0}; applyOutputs(o); }
      else {
        bool o[8]={0,0,0,0,0,0,0,0}; applyOutputs(o);
        purgeInitialized = false;
        sendMessage("PURGE COMPLETE");
        currentState = WAIT_FOR_FILL;
        broadcastState();
      }
      break;
    }

    case WAIT_FOR_FILL: {
      static const String opts[] = { "IGN_TEST", "FILL" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);
      int r = promptUserSelect("WAIT_FOR_FILL: Next? (IGN_TEST/FILL)", opts, 2);
      if (r == -1) break;
      if (r == 1) currentState = IGN_TEST;
      if (r == 2) currentState = FILL;
      resetTimedStates(); broadcastState();
      break;
    }

    case IGN_TEST: {
      static const String opts[] = { "WAIT_FOR_FILL" };
      bool outputs[8] = {1,1,0,0,0,0,0,0};
      applyOutputs(outputs);
      int r = promptUserSelect("IGN_TEST: Next? (WAIT_FOR_FILL)", opts, 1);
      if (r == -1) break;
      if (r == 1) { currentState = WAIT_FOR_FILL; resetTimedStates(); broadcastState(); }
      break;
    }

    case FILL: {
      static const String opts[] = { "PERSONNEL_DEPARTING" };
      bool outputs[8] = {0,0,0,0,1,0,0,0};
      applyOutputs(outputs);
      int r = promptUserSelect("FILL: Next? (PERSONNEL_DEPARTING)", opts, 1);
      if (r == -1) break;
      if (r == 1) { currentState = PERSONNEL_DEPARTING; resetTimedStates(); broadcastState(); }
      break;
    }

    case PERSONNEL_DEPARTING: {
      static const String opts[] = { "ARMED" };
      static bool gateArmed = false;
      bool outputs[8] = {0,0,0,0,1,0,0,0};
      applyOutputs(outputs);
      if (!gateArmed) {
        int r = promptUserSelect("PERSONNEL_DEPARTING: Next? (ARMED)", opts, 1);
        if (r == -1) break;
        if (r == 1) { gateArmed = true; promptCancel(); }
        else break;
      }
      if (gateArmed) {
        int pw = askPassword();
        if (pw == -1) break;
        if (pw == 1) {
          gateArmed = false;
          currentState = ARMED;
          resetTimedStates();
          sendMessage("Password accepted");
          broadcastState();
        }
      }
      break;
    }

    case ARMED: {
      static const String opts[] = { "FIRE" };
      static unsigned long armedStartMs = 0;
      if (!armedInitialized) {
        armedStartMs    = millis();
        armedInitialized = true;
        sendMessage("FIRE SEQUENCE START");
      }
      unsigned long elapsed = millis() - armedStartMs;
      if      (elapsed < 5000UL) { bool o[8]={0,0,0,0,0,0,0,0}; applyOutputs(o); }
      else                       { bool o[8]={0,0,0,0,0,0,1,0}; applyOutputs(o); }
      int r = promptUserSelect("ARMED: Next? (FIRE)", opts, 1);
      if (r == -1) break;
      if (r == 1) { currentState = FIRE; resetTimedStates(); broadcastState(); }
      break;
    }

    case FIRE: {
      static const String opts[] = { "PP1" };
      static unsigned long fireStartMs = 0;
      if (!fireInitialized) {
        fireStartMs    = millis();
        fireInitialized = true;
        sendMessage("FIRE SEQUENCE START");
      }
      unsigned long elapsed = millis() - fireStartMs;
      if      (elapsed < 20UL) { bool o[8]={1,1,0,0,0,0,1,1}; applyOutputs(o); }
      else if (elapsed < 30000UL) { bool o[8]={1,1,1,0,0,0,1,1}; applyOutputs(o); }
      else                     { bool o[8]={1,1,1,1,1,1,1,1}; applyOutputs(o); }
      int r = promptUserSelect("FIRE: Next? (PP1)", opts, 1);
      if (r == -1) break;
      if (r == 1) { fireInitialized = false; currentState = PP1; broadcastState(); }
      break;
    }

    case ABORT: {
      static unsigned long abortStartMs = 0;
      if (!abortInitialized) {
        abortStartMs    = millis();
        abortInitialized = true;
        sendMessage("ABORT INITIALIZED");
      }
      unsigned long elapsed = millis() - abortStartMs;
      if      (elapsed < 180UL*1000UL) { bool o[8]={0,0,0,1,1,0,0,0}; applyOutputs(o); }
      else if (elapsed < 240UL*1000UL) { bool o[8]={0,0,0,0,0,0,0,0}; applyOutputs(o); }
      else if (elapsed < 300UL*1000UL) { bool o[8]={0,0,0,0,0,1,0,0}; applyOutputs(o); }
      else {
        bool o[8]={0,0,0,0,0,0,0,0}; applyOutputs(o);
        abortInitialized = false;
        sendMessage("ABORT COMPLETE");
        currentState = PP1;
        broadcastState();
      }
      break;
    }

    case PP1: {
      static const String opts[] = { "PP2" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);
      int r = promptUserSelect("PP1: Next? (PP2)", opts, 1);
      if (r == -1) break;
      if (r == 1) { currentState = PP2; resetTimedStates(); broadcastState(); }
      break;
    }

    case PP2: {
      static const String opts[] = { "TEST_COMPLETE" };
      bool outputs[8] = {0,0,0,0,1,0,0,0};
      applyOutputs(outputs);
      int r = promptUserSelect("PP2: Next? (TEST_COMPLETE)", opts, 1);
      if (r == -1) break;
      if (r == 1) { currentState = TEST_COMPLETE; resetTimedStates(); broadcastState(); }
      break;
    }

    case TEST_COMPLETE: {
      static const String opts[] = { "STARTUP" };
      bool outputs[8] = {0,0,0,0,0,0,0,0};
      applyOutputs(outputs);
      int r = promptUserSelect("TEST_COMPLETE: Next? (STARTUP)", opts, 1);
      if (r == -1) break;
      if (r == 1) { currentState = STARTUP; resetTimedStates(); broadcastState(); }
      break;
    }
  }
}
