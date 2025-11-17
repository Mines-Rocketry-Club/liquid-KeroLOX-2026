// SerialHandler.ino
//
// Parses lines from the Pi/GUI and routes them to:
//   - Telemetry config (CFGBEGIN/CFGEND + CSV lines)
//   - State machine (normal commands + ELEVATE <STATE>)
//   - Telemetry snapshot (GET)
//   - ASK/ANS prompt/response
//
// Messages back to Pi:
//   STATE <STATE_NAME>
//   MSG <text...>
//   ASK <id> <question...>

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

// From StateMachine.ino
void initStateMachine();
void setStateFromCommand(const char *cmd);
void setStateOverride(const char *cmd);
void broadcastState();

// From Telemetry.ino
void sendTelemetry();
void telemetry_beginConfig();
void telemetry_appendConfigLine(const String &line);
void telemetry_endConfig();

// Exposed from here
void sendMessage(const char *msg);
void askUser(uint8_t id, const char *question);

// ----------------- INTERNAL STATE -----------------

static char  serialBuf[128];
static int   serialLen = 0;

static bool    waitingForAnswer = false;
static uint8_t lastPromptId     = 0;

static bool telemetryConfigMode = false;

// Forward
static void handleLine(const char *line);
static void handleUserAnswer(uint8_t id, const char *answer);

// ----------------- PUBLIC API -----------------

void processSerial() {
  while (Serial.available() > 0) {
    serialLen = Serial.readBytesUntil('\n', serialBuf, sizeof(serialBuf) - 1);
    if (serialLen <= 0) return;

    serialBuf[serialLen] = '\0';
    handleLine(serialBuf);
  }
}

// ----------------- CORE PARSER -----------------

static void handleLine(const char *line) {
  // 1) Telemetry config mode
  if (strcmp(line, "CFGBEGIN") == 0) {
    telemetryConfigMode = true;
    telemetry_beginConfig();
    return;
  }

  if (strcmp(line, "CFGEND") == 0) {
    telemetryConfigMode = false;
    telemetry_endConfig();
    // Let GUI know our current state after config
    initStateMachine();
    return;
  }

  if (telemetryConfigMode) {
    // Treat any non-control line as a CSV row
    telemetry_appendConfigLine(String(line));
    return;
  }

  // 2) Forced override: ELEVATE <STATE>
  if (strncmp(line, "ELEVATED ", 8) == 0) {
    const char *forced = line + 8;  // text after "ELEVATE "

    // Skip extra spaces if any
    while (*forced == ' ') {
      ++forced;
    }

    sendMessage("ELEVATED state override received.");
    setStateOverride(forced);
    return;
  }

  // 3) Answer to a question: ANS <id> <answer...>
  if (strncmp(line, "ANS ", 4) == 0) {
    const char *p = line + 4;

    // Parse id
    uint8_t id = (uint8_t)atoi(p);

    // Skip digits
    while (*p && *p != ' ') {
      ++p;
    }
    while (*p == ' ') {
      ++p;
    }

    if (waitingForAnswer && id == lastPromptId) {
      handleUserAnswer(id, p);
      waitingForAnswer = false;
    }
    return;
  }

  // 4) Telemetry snapshot request
  if (strcmp(line, "GET") == 0) {
    sendTelemetry();
    return;
  }

  // 5) Anything else is treated as a state command
  setStateFromCommand(line);
}

// ----------------- MESSAGING HELPERS -----------------

void sendMessage(const char *msg) {
  Serial.print("MSG ");
  Serial.println(msg);
}

void askUser(uint8_t id, const char *question) {
  lastPromptId     = id;
  waitingForAnswer = true;

  Serial.print("ASK ");
  Serial.print(id);
  Serial.print(' ');
  Serial.println(question);
}

static void handleUserAnswer(uint8_t id, const char *answer) {
  // Uppercase copy
  char buf[64];
  size_t i = 0;
  for (; i < sizeof(buf) - 1 && answer[i] != '\0'; ++i) {
    buf[i] = toupper((unsigned char)answer[i]);
  }
  buf[i] = '\0';

  if (strcmp(buf, "YES") == 0) {
    sendMessage("User answered YES.");
  } else if (strcmp(buf, "NO") == 0) {
    sendMessage("User answered NO.");
  } else {
    char msg[96];
    snprintf(msg, sizeof(msg),
             "User answered (id=%u): %s",
             (unsigned)id, answer);
    sendMessage(msg);
  }
}
