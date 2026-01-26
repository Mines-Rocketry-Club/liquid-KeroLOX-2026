// StateMachine.ino
#include <Arduino.h>
#include <string.h>

extern void sendMessage(const char *msg);
extern int  promptUserSelect(const String& prompt, const String options[], int optionCount);
extern int  askPassword();
extern void promptCancel();

enum SystemState {
  LEVEL_CHECK = 0,
  PREFILL,
  LOAD,
  MANUAL,
  ARMED,
  FIRE,
  ABORT,
  SAFE_UNLOADED,
  STATE_COUNT
};

static int currentState = (int)LEVEL_CHECK;

static const char* STATE_NAMES[STATE_COUNT] = {
  "LEVEL_CHECK",
  "PREFILL",
  "LOAD",
  "MANUAL",
  "ARMED",
  "FIRE",
  "ABORT",
  "SAFE_UNLOADED"
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

// -------------------------
// ELEVATED password gate
// -------------------------
static bool g_elevatedPending = false;
static int  g_elevatedTarget  = -1;

void initStateMachine() {
  currentState = LEVEL_CHECK;
  g_elevatedPending = false;
  g_elevatedTarget  = -1;
  sendMessage("State machine initialized");
  broadcastState();
}

void setStateOverride(const char *cmd) {
  int next;
  if (!parseStateString_int(cmd, next)) {
    sendMessage("Unknown ELEVATED state");
    return;
  }

  // Require password before applying any elevated override (including MANUAL)
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
    // pw == 0 (wrong): askPassword() already printed MSG; keep pending so they can retry
    return;
  }

  switch ((SystemState)currentState) {

    case LEVEL_CHECK: {
      static const String opts[] = { "PREFILL" };
      int r = promptUserSelect("LEVEL_CHECK: Next? (PREFILL)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = PREFILL;
      broadcastState();
      break;
    }

    case PREFILL: {
      static const String opts[] = { "LOAD" };
      int r = promptUserSelect("PREFILL: Next? (LOAD)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = LOAD;
      broadcastState();
      break;
    }

    case LOAD: {
      // MANUAL removed from here; only reachable via ELEVATED
      static const String opts[] = { "ARMED" };
      static bool gateArmed = false;

      if (!gateArmed) {
        int r = promptUserSelect("LOAD: Next? (ARMED)", opts, 1);
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

    case MANUAL: {
      // MANUAL is now only ENTERED via ELEVATED, but you can exit to other states here.
      static const String opts[] = { "LEVEL_CHECK", "PREFILL", "LOAD", "ARMED" };
      static bool gateArmed = false;

      if (!gateArmed) {
        int r = promptUserSelect("MANUAL: Next? (LEVEL_CHECK / PREFILL / LOAD / ARMED)", opts, 4);
        if (r == -1) break;

        if (r == 1) { currentState = LEVEL_CHECK; broadcastState(); break; }
        if (r == 2) { currentState = PREFILL;     broadcastState(); break; }
        if (r == 3) { currentState = LOAD;        broadcastState(); break; }
        if (r == 4) {
          gateArmed = true;   // keep ARMED password gate
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
      // MANUAL removed from here; only reachable via ELEVATED
      static const String opts[] = { "FIRE" };
      int r = promptUserSelect("ARMED: Next? (FIRE)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = FIRE;
      broadcastState();
      break;
    }

    case FIRE: {
      // MANUAL removed from here; only reachable via ELEVATED
      static const String opts[] = { "SAFE_UNLOADED" };
      int r = promptUserSelect("FIRE: Next? (SAFE_UNLOADED)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = SAFE_UNLOADED;
      broadcastState();
      break;
    }

    case ABORT: {
      // MANUAL removed from here; only reachable via ELEVATED
      static const String opts[] = { "SAFE_UNLOADED" };
      int r = promptUserSelect("ABORT: Next? (SAFE_UNLOADED)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = SAFE_UNLOADED;
      broadcastState();
      break;
    }

    case SAFE_UNLOADED: {
      // MANUAL removed from here; only reachable via ELEVATED
      static const String opts[] = { "LEVEL_CHECK" };
      int r = promptUserSelect("SAFE_UNLOADED: Next? (LEVEL_CHECK)", opts, 1);
      if (r == -1) break;
      if (r == 1) currentState = LEVEL_CHECK;
      broadcastState();
      break;
    }
  }
}
