// SerialHandler.ino
#include <Arduino.h>

void telemetry_beginConfig();
void telemetry_appendConfigLine(const String& line);
void telemetry_endConfig();
void sendTelemetry();

void forceAbort();
void setStateOverride(const char *cmd);
void broadcastState();
void promptResendIfActive();

bool   g_answerReady = false;
String g_answerText;

static bool g_inConfigMode = false;

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
    sendTelemetry();
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
    String line = Serial.readStringUntil('\n');
    line.replace("\r", "");
    handleLine(line);
  }
}
