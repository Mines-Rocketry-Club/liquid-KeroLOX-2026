// StateMachine.ino
#include <Arduino.h>
#include <string.h>

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
// Pi GET watchdog timeout
// Easy to change later: 15 min, 20 min, 10 min, etc.
// --------------------------------------------------
const unsigned long PI_GET_TIMEOUT_MS = 1UL * 60UL * 1000UL;

// Tracks last time a GET was received from the Raspberry Pi
static unsigned long g_lastPiGetMs = 0;

// Called by serial handler whenever a GET command is received
void markPiGetReceived() {
  g_lastPiGetMs = millis();
}

// True if too long has passed since last GET
bool isPiGetTimedOut() {
  unsigned long now = millis();
  return (now - g_lastPiGetMs) >= PI_GET_TIMEOUT_MS;
}

// Only these states should auto-abort on Pi timeout
static bool stateRequiresPiHeartbeat(int state) {
  return (state == PERSONNEL_DEPARTING ||
          state == ARMED ||
          state == FIRE);
}

// -------------------------
// ELEVATED password gate
// -------------------------
static bool g_elevatedPending = false;
static int  g_elevatedTarget  = -1;

void initStateMachine() {
  currentState = STARTUP;
  g_elevatedPending = false;
  g_elevatedTarget  = -1;

  // Initialize watchdog reference so timeout starts counting from boot
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
  currentState = ABORT;
  sendMessage("ABORT triggered");
  broadcastState();
}

void stateMachineLoop() {
  // --------------------------------------------------
  // Auto-abort if Pi has not sent GET in time
  // Only active in PERSONNEL_DEPARTING / ARMED / FIRE
  // --------------------------------------------------
  if (stateRequiresPiHeartbeat(currentState) && isPiGetTimedOut()) {
    promptCancel();
    g_elevatedPending = false;
    g_elevatedTarget  = -1;
    currentState = ABORT;
    sendMessage("ABORT triggered: no GET received from Raspberry Pi within timeout");
    broadcastState();
    return;
  }

  // If an ELEVATED override is pending, handle it first and block everything else
  if (g_elevatedPending) {
    int pw = askPassword();
    if (pw == -1) return;   // waiting
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
      // TODO, CLOSE VALVES
      int r = promptUserSelect("STARTUP: Next? (PURGE)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PURGE;
      broadcastState();
      break;
    }

    case PURGE: {
      static const String opts[] = { "WAIT_FOR_FILL" };
      sendMessage("PURGE INITIALIZED");
      // TODO, purge sequence
      sendMessage("PURGE COMPLETE");
      currentState = WAIT_FOR_FILL;
      broadcastState();
      break;
    }

    case WAIT_FOR_FILL: {
      static const String opts[] = { "FILL" };
      // TODO, CLOSE VALVES
      int r = promptUserSelect("WAIT_FOR_FILL: Next? (FILL)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = FILL;
      broadcastState();
      break;
    }

    case FILL: {
      static const String opts[] = { "PERSONNEL_DEPARTING" };
      // TODO OPEN VENT VALVE
      int r = promptUserSelect("FILL: Next? (PERSONNEL_DEPARTING)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PERSONNEL_DEPARTING;
      broadcastState();
      break;
    }

    case PERSONNEL_DEPARTING: {
      static const String opts[] = { "ARMED" };
      static bool gateArmed = false;
      // TODO, CLOSE VALVES

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
      // TODO, OPEN PRESSURIZING VALVES
      int r = promptUserSelect("ARMED: Next? (FIRE)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = FIRE;
      broadcastState();
      break;
    }

    case FIRE: {
      static const String opts[] = { "PP1" };
      // TODO, FIRE SEQUENCE
      int r = promptUserSelect("FIRE: Next? (PP1)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PP1;
      broadcastState();
      break;
    }

    case ABORT: {
      static const String opts[] = { "PP1" };
      sendMessage("ABORT INITIALIZED");
      // TODO, ABORT SEQUENCE
      sendMessage("ABORT COMPLETE");
      currentState = PP1;
      broadcastState();
      break;
    }

    case PP1: {
      static const String opts[] = { "PP2" };
      // TODO, CLOSE VALVES
      int r = promptUserSelect("PP1: Next? (PP2)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PP2;
      broadcastState();
      break;
    }

    case PP2: {
      static const String opts[] = { "TEST_COMPLETE" };
      // TODO, Post test purge sequence
      int r = promptUserSelect("PP2: Next? (TEST_COMPLETE)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = TEST_COMPLETE;
      broadcastState();
      break;
    }

    case TEST_COMPLETE: {
      static const String opts[] = { "STARTUP" };
      int r = promptUserSelect("TEST_COMPLETE: Next? (STARTUP)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = STARTUP;
      broadcastState();
      break;
    }
  }
}