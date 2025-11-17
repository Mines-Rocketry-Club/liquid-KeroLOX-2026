// StateMachine.ino
//
// State machine with:
//   - Normal transitions via setStateFromCommand(const char*)
//   - Forced transitions via setStateOverride(const char*)
//   - Internal transitions via setState(int newStateInt, bool elevated)
//
// Serial protocol to Pi/GUI:
//   STATE <STATE_NAME>
//   MSG <text...>
//
// Pi/GUI sends commands like:
//   LEVEL_CHECK
//   SAFE_LOADED
//   PRESSURIZE_FUEL
//   FIRE
//   ELEVATE FIRE   (SerialHandler strips "ELEVATE " and passes "FIRE")

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

// Implemented in SerialHandler.ino
extern void sendMessage(const char *msg);

// Implemented in Telemetry.ino
extern float getTelemetry(const String& name);

// ----------------- STATE ENUM -----------------

enum SystemState {
  STATE_LEVEL_CHECK,          // STARTS HERE
  STATE_PREFILL,
  STATE_ABORT,
  STATE_TEST_PULSE,
  STATE_MANUAL,
  STATE_LOAD,
  STATE_SEQUENCE_START,       // STARTS THE LAUNCH SEQUENCE
  STATE_SEQUENCE_PRECHECKS,
  STATE_SEQUENCE_HOLD,        // HOLDING READY FOR FUEL PRESSURIZATION
  STATE_PRESSURIZE_FUEL,      // PRESSURIZES FUEL
  STATE_ARMED,
  STATE_FIRE,
  STATE_MISFIRE,
  STATE_VENT_PRECHECK,        // PREPARES TO VENT
  STATE_VENT_PRECHECK_FAILURE,
  STATE_CONFIRM_VENT,
  STATE_DEPRESSURIZE_FUEL,
  STATE_VENT,
  STATE_PAD_SHUTDOWN,
  STATE_SAFE_UNLOADED         // ENDS HERE
};

static SystemState currentState = STATE_LEVEL_CHECK;

// ----------------- LOOKUP TABLE -----------------

struct StateEntry {
  SystemState state;
  const char *name;   // canonical uppercase name
};

static const StateEntry STATE_TABLE[] = {
  { STATE_LEVEL_CHECK,           "LEVEL_CHECK" },
  { STATE_PREFILL,               "SAFE_LOADED" },        // serial command name
  { STATE_ABORT,                 "ABORT" },
  { STATE_TEST_PULSE,            "TEST_PULSE" },
  { STATE_MANUAL,                "MANUAL" },
  { STATE_LOAD,                  "LOAD" },
  { STATE_SEQUENCE_START,        "SEQUENCE_START" },
  { STATE_SEQUENCE_PRECHECKS,    "SEQUENCE_PRECHECKS" },
  { STATE_SEQUENCE_HOLD,         "SEQUENCE_HOLD" },
  { STATE_PRESSURIZE_FUEL,       "PRESSURIZE_FUEL" },
  { STATE_ARMED,                 "ARMED" },
  { STATE_FIRE,                  "FIRE" },
  { STATE_MISFIRE,               "MISFIRE" },
  { STATE_VENT_PRECHECK,         "VENT_PRECHECKS" },
  { STATE_VENT_PRECHECK_FAILURE, "VENT_PRECHECK_FAILURE" },
  { STATE_CONFIRM_VENT,          "CONFIRM_VENT" },
  { STATE_DEPRESSURIZE_FUEL,     "DEPRESSURIZE_FUEL" },
  { STATE_VENT,                  "VENT" },
  { STATE_PAD_SHUTDOWN,          "VENTING_COMPLETE" },
  { STATE_SAFE_UNLOADED,         "SAFE_UNLOADED" }
};

static const int STATE_COUNT = sizeof(STATE_TABLE) / sizeof(STATE_TABLE[0]);

// --------------------------------------------------
// FUNCTION PROTOTYPES
// --------------------------------------------------

static const char* systemStateToString(int s);
static bool        parseStateString(const char *cmd, int &outState);
static bool        isTransitionAllowed(int fromInt, int toInt);
static void        logTransition(int fromInt, int toInt, bool elevated);
void               broadcastState();
void               initStateMachine();
void               setStateFromCommand(const char *cmd);
void               setStateOverride(const char *cmd);
static void        onStateEnter(SystemState newState);
void               stateMachineLoop();
bool               setState(int newStateInt, bool elevated);

// --------------------------------------------------
// HELPER FUNCTIONS (use int instead of SystemState)
// --------------------------------------------------

// enum -> string (via int)
static const char* systemStateToString(int s) {
  SystemState st = (SystemState)s;
  for (int i = 0; i < STATE_COUNT; ++i) {
    if (STATE_TABLE[i].state == st) {
      return STATE_TABLE[i].name;
    }
  }
  return "UNKNOWN_STATE";
}

// string -> enum (as int).
// Returns true if understood, false if unknown.
static bool parseStateString(const char *cmd, int &outState) {
  // Skip leading spaces / tabs
  while (*cmd == ' ' || *cmd == '\t') {
    ++cmd;
  }

  char buf[32];
  int j = 0;

  // Uppercase copy of input, stop on CR/LF/NUL
  for (int i = 0; cmd[i] != '\0' && cmd[i] != '\r' && cmd[i] != '\n'; ++i) {
    if (j >= (int)sizeof(buf) - 1) break;
    buf[j++] = toupper((unsigned char)cmd[i]);
  }
  buf[j] = '\0';

  // Trim trailing spaces/tabs
  while (j > 0 && (buf[j - 1] == ' ' || buf[j - 1] == '\t')) {
    buf[--j] = '\0';
  }

  // Look up in table
  for (int i = 0; i < STATE_COUNT; ++i) {
    if (strcmp(buf, STATE_TABLE[i].name) == 0) {
      outState = (int)STATE_TABLE[i].state;
      return true;
    }
  }

  return false;
}

// Core rule engine: is from->to allowed? (int version)
static bool isTransitionAllowed(int fromInt, int toInt) {
  SystemState from = (SystemState)fromInt;
  SystemState to   = (SystemState)toInt;

  switch (to) {
    case STATE_LEVEL_CHECK:
      return false;

    case STATE_PREFILL:
      return (from == STATE_LEVEL_CHECK ||
              from == STATE_TEST_PULSE ||
              from == STATE_MANUAL);

    case STATE_ABORT:
      // ABORT always allowed from anything except late/terminal states
      return !(from == STATE_VENT_PRECHECK ||
               from == STATE_VENT_PRECHECK_FAILURE ||
               from == STATE_CONFIRM_VENT ||
               from == STATE_DEPRESSURIZE_FUEL ||
               from == STATE_VENT ||
               from == STATE_PAD_SHUTDOWN ||
               from == STATE_SAFE_UNLOADED);

    case STATE_TEST_PULSE:
      return (from == STATE_PREFILL ||
              from == STATE_MANUAL);

    case STATE_MANUAL:
      return (from == STATE_PREFILL ||
              from == STATE_TEST_PULSE);
    
    case STATE_LOAD:
      return (from == STATE_PREFILL ||
              from == STATE_TEST_PULSE ||
              from == STATE_MANUAL);

    case STATE_SEQUENCE_START:
      return (from == STATE_LOAD ||
              from == STATE_CONFIRM_VENT);

    case STATE_SEQUENCE_PRECHECKS:
      return (from == STATE_SEQUENCE_START || 
              from == STATE_SEQUENCE_PRECHECKS);

    case STATE_SEQUENCE_HOLD:
      return (from == STATE_SEQUENCE_PRECHECKS);

    case STATE_PRESSURIZE_FUEL:
      return (from == STATE_SEQUENCE_HOLD ||
              from == STATE_PRESSURIZE_FUEL);

    case STATE_ARMED:
      return (from == STATE_PRESSURIZE_FUEL);

    case STATE_FIRE:
      return (from == STATE_ARMED);

    case STATE_MISFIRE:
      return (from == STATE_FIRE);

    case STATE_VENT_PRECHECK:
      return (from == STATE_ABORT ||
              from == STATE_VENT_PRECHECK_FAILURE);

    case STATE_VENT_PRECHECK_FAILURE:
      return (from == STATE_VENT_PRECHECK);

    case STATE_CONFIRM_VENT:
      return (from == STATE_VENT_PRECHECK ||
              from == STATE_VENT_PRECHECK_FAILURE);

    case STATE_DEPRESSURIZE_FUEL:
      // Example logic; tweak as needed
      return (from == STATE_CONFIRM_VENT ||
              from == STATE_DEPRESSURIZE_FUEL);

    case STATE_VENT:
      return (from == STATE_DEPRESSURIZE_FUEL ||
              from == STATE_VENT);

    case STATE_PAD_SHUTDOWN:
      return (from == STATE_VENT ||
              from == STATE_FIRE);

    case STATE_SAFE_UNLOADED:
      return (from == STATE_PAD_SHUTDOWN);
  }

  return false;
}

// Log transitions nicely (int version)
static void logTransition(int fromInt, int toInt, bool elevated) {
  const char *fromName = systemStateToString(fromInt);
  const char *toName   = systemStateToString(toInt);

  char buf[96];
  if (elevated) {
    snprintf(buf, sizeof(buf), "[OVERRIDE] State %s -> %s", fromName, toName);
  } else {
    snprintf(buf, sizeof(buf), "State %s -> %s", fromName, toName);
  }
  sendMessage(buf);
}

// Broadcast current state to Pi/GUI
//   STATE <NAME>
//   MSG Allowed from <NAME>: ...
void broadcastState() {
  // Protocol line for parser
  Serial.print("STATE ");
  Serial.println(systemStateToString((int)currentState));

  // Human-readable allowed transitions
  char msg[160];
  strcpy(msg, "Allowed from ");
  strcat(msg, systemStateToString((int)currentState));
  strcat(msg, ":");

  for (int i = 0; i < STATE_COUNT; ++i) {
    int candidate = (int)STATE_TABLE[i].state;
    if (isTransitionAllowed((int)currentState, candidate)) {
      strcat(msg, " ");
      strcat(msg, systemStateToString(candidate));
    }
  }

  sendMessage(msg);
}

// --------------------------------------------------
// CORE STATE-CHANGE HELPER (int-based)
// --------------------------------------------------
//
// setState(newStateInt, elevated)
//
//  - newStateInt = (int) of SystemState
//  - elevated   = true  => skip rules, log as [OVERRIDE]
//  - elevated   = false => enforce rules via isTransitionAllowed()
//  - returns true if state actually changed, false on failure
//
bool setState(int newStateInt, bool elevated) {
  int prevInt    = (int)currentState;
  int desiredInt = newStateInt;

  // Enforce rules for non-elevated transitions
  if (!elevated) {
    if (!isTransitionAllowed(prevInt, desiredInt)) {
      char msg[120];
      snprintf(msg, sizeof(msg),
               "Transition %s -> %s NOT allowed.",
               systemStateToString(prevInt),
               systemStateToString(desiredInt));
      sendMessage(msg);
      return false;
    }
  }

  // No-op transition; do nothing
  if (prevInt == desiredInt) {
    return true;
  }

  SystemState newState = (SystemState)desiredInt;
  currentState = newState;
  onStateEnter(newState);
  logTransition(prevInt, desiredInt, elevated);
  broadcastState();
  return true;
}

// --------------------------------------------------
// PUBLIC API
// --------------------------------------------------

void initStateMachine() {
  // Initial state, no logging/rule-check to avoid noisy boot messages
  currentState = STATE_LEVEL_CHECK;
  onStateEnter(STATE_LEVEL_CHECK);
  broadcastState();
}

// Normal commands: obey rules (elevated = false)
void setStateFromCommand(const char *cmd) {
  // Ignore CSV-ish lines (contain commas)
  if (strchr(cmd, ',') != nullptr) {
    return;
  }

  int desiredInt;
  if (!parseStateString(cmd, desiredInt)) {
    char msg[80];
    snprintf(msg, sizeof(msg),
             "Unknown state command: %s", cmd);
    sendMessage(msg);
    return;
  }

  // Normal command: not elevated (rules enforced)
  setState(desiredInt, /*elevated=*/false);
}

// Override: skip rules, log as override (elevated = true)
void setStateOverride(const char *cmd) {
  int desiredInt;
  if (!parseStateString(cmd, desiredInt)) {
    char m[64];
    snprintf(m, sizeof(m), "Unknown override state: %s", cmd);
    sendMessage(m);
    return;
  }

  // Override: elevated logging, skip rules
  setState(desiredInt, /*elevated=*/true);
}

// --------------------------------------------------
// Called EXACTLY ONCE whenever we enter a new state.
// All "do once" logic here.
// --------------------------------------------------
static void onStateEnter(SystemState newState) {
  switch (newState) {
    case STATE_LEVEL_CHECK: {
      float level = getTelemetry("LOX_Level");
      if (isnan(level)) {
        sendMessage("ERROR: Telemetry for LOX_LEVEL does not exist.");
      }
      break;
    }

    case STATE_PREFILL:
      // sendMessage("Entering PREFILL");
      break;

    case STATE_ABORT:
      // sendMessage("ABORT triggered");
      break;

    case STATE_TEST_PULSE:
      // resetTestPulseTimer();
      break;

    case STATE_MANUAL:
      // enableManualMode();
      break;

    case STATE_LOAD:
      // sendMessage("Starting LOAD sequence");
      break;

    case STATE_SEQUENCE_START:
      // startSequenceTimer();
      break;

    case STATE_SEQUENCE_PRECHECKS:
      // verifySensorsBeforeHold();
      break;

    case STATE_SEQUENCE_HOLD:
      // sendMessage("In HOLD waiting to pressurize fuel");
      break;

    case STATE_PRESSURIZE_FUEL:
      // closeVentValve();
      // openFuelValve();
      break;

    case STATE_ARMED:
      // armIgnitionCircuit();
      break;

    case STATE_FIRE:
      // sendMessage("Ignition Fired!");
      break;

    case STATE_MISFIRE:
      // sendMessage("MISFIRE detected");
      break;

    case STATE_VENT_PRECHECK:
      // TODO ADD SETTING SYSTEM TO SAFE OUTPUT STATE
      break;

    case STATE_VENT_PRECHECK_FAILURE:
      // sendMessage("Vent Precheck FAILED");
      break;

    case STATE_CONFIRM_VENT:
      // waitForOperatorConfirmation();
      break;

    case STATE_DEPRESSURIZE_FUEL:
      // openVentValve();
      break;

    case STATE_VENT:
      // sendMessage("Venting fuel tank...");
      break;

    case STATE_PAD_SHUTDOWN:
      // shutdownPadSystems();
      break;

    case STATE_SAFE_UNLOADED:
      // sendMessage("Pad safe and unloaded");
      break;

    default:
      break;
  }
}

// --------------------------------------------------
// continuous per-state logic
// --------------------------------------------------
void stateMachineLoop() {
  switch (currentState) {
    case STATE_LEVEL_CHECK: {
      float level = getTelemetry("LOX_Level");
      if (level < 5.0f) {
        setState((int)STATE_PREFILL, false);
      } else if (level >= 5.0f) {
        setState((int)STATE_ABORT, false);
      }
      break;
    }
    case STATE_PREFILL:
      break;
    case STATE_ABORT: {
      setState((int)STATE_VENT_PRECHECK, false);
      break;
    }
    case STATE_TEST_PULSE:
      break;
    case STATE_MANUAL:
      break;
    case STATE_LOAD:
      break;
    case STATE_SEQUENCE_START:
      break;
    case STATE_SEQUENCE_PRECHECKS:
      break;
    case STATE_SEQUENCE_HOLD:
      break;
    case STATE_PRESSURIZE_FUEL:
      break;
    case STATE_ARMED:
      break;
    case STATE_FIRE:
      break;
    case STATE_MISFIRE:
      break;
    case STATE_VENT_PRECHECK: {
      if (true) { // add the pass fail the check logic here
        setState((int)STATE_CONFIRM_VENT, false);
      }
      else if (false) {
        setState((int)STATE_VENT_PRECHECK_FAILURE, false);
      }
      break;
    }
    case STATE_VENT_PRECHECK_FAILURE:
      break;
    case STATE_CONFIRM_VENT:
      break;
    case STATE_DEPRESSURIZE_FUEL:
      break;
    case STATE_VENT:
      break;
    case STATE_PAD_SHUTDOWN:
      break;
    case STATE_SAFE_UNLOADED:
      break;
  }
}
