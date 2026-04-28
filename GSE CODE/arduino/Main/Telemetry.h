#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <Arduino.h>
#include <math.h>

extern void sendMessage(const char *msg);
extern bool g_outputState[8];

static const int OUTPUT_STATE_COUNT = 8;
static const int MAX_CHANNELS = 32;

struct TelemetryChannel {
  String name;
  String owner;
  String unit;
  float lowRed;
  float highRed;
  float lowYellow;
  float highYellow;
  String partnumber;
  float scale;
  float offset;
  int pin;
  bool isOutput;
};

static TelemetryChannel channels[MAX_CHANNELS];
static int channelCount = 0;
static bool telemetryConfigured = false;
static String telemetryCsvText;

// ---------------- Helpers ----------------

static int parsePin(const String &pinStr) {
  String s = pinStr;
  s.trim();
  s.toUpperCase();

  if (s.startsWith("A")) {
    int n = s.substring(1).toInt();
    return A0 + n;
  }

  return s.toInt();
}

static float toFloatSafe(const String &s, float fallback = 0.0f) {
  String t = s;
  t.trim();

  if (!t.length()) {
    return fallback;
  }

  return t.toFloat();
}

static bool toBoolSafe(const String &s, bool fallback = false) {
  String t = s;
  t.trim();
  t.toUpperCase();

  if (!t.length()) {
    return fallback;
  }

  if (t == "1" || t == "TRUE" || t == "YES" || t == "Y" || t == "OUTPUT") {
    return true;
  }

  if (t == "0" || t == "FALSE" || t == "NO" || t == "N" || t == "INPUT") {
    return false;
  }

  return fallback;
}

static bool isHx711Channel(const TelemetryChannel &ch) {
  String p = ch.partnumber;
  p.trim();
  p.toUpperCase();

  return p == "HX711";
}

static bool readChannelValue(const TelemetryChannel &ch, float &val) {
  // HX711 lives on the Pi side, not Arduino side.
  // Skip these so Arduino telemetry does not block or slow down.
  if (isHx711Channel(ch)) {
    return false;
  }

  // Output telemetry reads from g_outputState[index]
  if (ch.isOutput) {
    if (ch.pin >= 0 && ch.pin < OUTPUT_STATE_COUNT) {
      int raw = g_outputState[ch.pin] ? 1 : 0;
      val = raw * ch.scale + ch.offset;
      return true;
    }

    return false;
  }

  // Normal Arduino analog telemetry
  int raw = analogRead(ch.pin);
  val = raw * ch.scale + ch.offset;
  return true;
}

// ---------------- Config Upload ----------------

void telemetry_beginConfig() {
  telemetryCsvText = "";
  telemetryConfigured = false;
  channelCount = 0;

  sendMessage("Telemetry config begin");
}

void telemetry_appendConfigLine(const String &line) {
  telemetryCsvText += line;
  telemetryCsvText += "\n";
}

void telemetry_endConfig() {
  sendMessage("Telemetry config end, parsing...");

  channelCount = 0;
  telemetryConfigured = false;

  int lineStart = 0;

  int headerEnd = telemetryCsvText.indexOf('\n', lineStart);
  if (headerEnd < 0) {
    sendMessage("ERROR: Telemetry CSV missing header");
    return;
  }

  lineStart = headerEnd + 1;
  int rowIndex = 0;

  while (lineStart < telemetryCsvText.length() && channelCount < MAX_CHANNELS) {
    int lineEnd = telemetryCsvText.indexOf('\n', lineStart);

    if (lineEnd < 0) {
      lineEnd = telemetryCsvText.length();
    }

    String line = telemetryCsvText.substring(lineStart, lineEnd);
    line.trim();
    lineStart = lineEnd + 1;

    if (!line.length()) {
      continue;
    }

    rowIndex++;

    String fields[12];
    int idx = 0;
    int start = 0;

    for (int i = 0; i < line.length() && idx < 11; i++) {
      if (line.charAt(i) == ',') {
        fields[idx] = line.substring(start, i);
        fields[idx].trim();
        idx++;
        start = i + 1;
      }
    }

    fields[idx] = line.substring(start);
    fields[idx].trim();

    int fieldCount = idx + 1;

    if (fieldCount < 12) {
      continue;
    }

    TelemetryChannel &ch = channels[channelCount];

    ch.name = fields[0];
    ch.owner = fields[1];
    ch.unit = fields[2];

    ch.lowRed = toFloatSafe(fields[3], 0.0f);
    ch.highRed = toFloatSafe(fields[4], 0.0f);
    ch.lowYellow = toFloatSafe(fields[5], 0.0f);
    ch.highYellow = toFloatSafe(fields[6], 0.0f);

    ch.partnumber = fields[7];

    ch.scale = toFloatSafe(fields[8], 1.0f);
    ch.offset = toFloatSafe(fields[9], 0.0f);
    ch.pin = parsePin(fields[10]);
    ch.isOutput = toBoolSafe(fields[11], false);

    channelCount++;
  }

  if (channelCount > 0) {
    telemetryConfigured = true;

    char msg[64];
    snprintf(msg, sizeof(msg), "Telemetry channels configured: %d", channelCount);
    sendMessage(msg);
  } else {
    sendMessage("ERROR: No telemetry channels configured");
  }
}

// ---------------- Send Telemetry ----------------

void sendTelemetry() {
  if (!telemetryConfigured || channelCount == 0) {
    Serial.println("END");
    return;
  }

  for (int i = 0; i < channelCount; i++) {
    float val;

    if (!readChannelValue(channels[i], val)) {
      continue;
    }

    Serial.print(channels[i].name);
    Serial.print(" ");
    Serial.println(val, 3);
  }

  Serial.println("END");
}

// ---------------- Optional Read By Name ----------------

float getTelemetry(const String &name) {
  if (!telemetryConfigured || channelCount == 0) {
    return NAN;
  }

  String target = name;
  target.trim();
  target.toUpperCase();

  for (int i = 0; i < channelCount; i++) {
    String chName = channels[i].name;
    chName.trim();
    chName.toUpperCase();

    if (chName == target) {
      float val;

      if (readChannelValue(channels[i], val)) {
        return val;
      }

      return NAN;
    }
  }

  return NAN;
}

#endif