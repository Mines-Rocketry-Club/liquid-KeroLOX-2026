#include <Arduino.h>

void telemetry_beginConfig();
void telemetry_appendConfigLine(const String& line);
void telemetry_endConfig();
void sendTelemetry();

void forceAbort();
void setStateOverride(const char *cmd);
void broadcastState();
void promptResendIfActive();
void markPiGetReceived();

bool   g_answerReady = false;
String g_answerText;
bool   g_telemetryPending = false;  // deferred GET flag

static bool   g_inConfigMode = false;
static String g_serialBuf    = "";

void sendMessage(const char *msg) {
  Serial.print("MSG ");
  Serial.println(msg);
}

static void handleLine(const String& lineIn) {
  String line = lineIn;
  line.trim();
  if (!line.length()) return;

  if (g_inConfigMode) {
    if (line.equalsIgnoreCase("CFGEND")) {
      g_inConfigMode = false;
      telemetry_endConfig();
      broadcastState();
      promptResendIfActive();
      return;
    }

    telemetry_appendConfigLine(line);
    return;
  }

  if (line.equalsIgnoreCase("CFGBEGIN")) {
    g_inConfigMode = true;
    telemetry_beginConfig();
    return;
  }

  if (line.equalsIgnoreCase("GET")) {
    markPiGetReceived();
    g_telemetryPending = true;  // defer — service in loop() after steppers tick
    return;
  }

  if (line.equalsIgnoreCase("ABORT")) {
    forceAbort();
    return;
  }

  if (line.startsWith("ELEVATED ")) {
    String s = line.substring(9);
    s.trim();
    s.toUpperCase();
    setStateOverride(s.c_str());
    return;
  }

  if (line.startsWith("ANSWER ") || line.equalsIgnoreCase("ANSWER")) {
    String rest = "";
    int sp = line.indexOf(' ');
    if (sp >= 0) rest = line.substring(sp + 1);
    rest.trim();
    g_answerText = rest;
    g_answerReady = true;
    return;
  }
}

void processSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      g_serialBuf.replace("\r", "");
      handleLine(g_serialBuf);
      g_serialBuf = "";
    } else {
      g_serialBuf += c;
    }
  }
}
