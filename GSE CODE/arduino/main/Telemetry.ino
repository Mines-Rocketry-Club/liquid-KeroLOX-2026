// Telemetry.ino
// Telemetry configuration is pushed from the Pi over serial.
//
// Protocol (handled in SerialHandler.ino):
//   CFGBEGIN          -> start config mode, clear previous CSV
//   <any CSV line>    -> append to telemetry config text
//   CFGEND            -> parse CSV & build channels
//
// NEW CSV FORMAT (12 columns):
//   name,owner,unit,low_red,high_red,low_yellow,high_yellow,partnumber,scale,offset,pin,is_output
//
// Arduino uses:
//   - name
//   - owner
//   - scale, offset, pin
//   - is_output
//
// Reading behavior:
//   if is_output == false:
//       value = analogRead(pin) * scale + offset
//
//   if is_output == true:
//       value = digitalRead(pin) * scale + offset
//
// Public API:
//   void telemetry_beginConfig();
//   void telemetry_appendConfigLine(const String& line);
//   void telemetry_endConfig();
//   void sendTelemetry();
//   float getTelemetry(const String& name)

#include <Arduino.h>
#include <math.h>

// Implemented in SerialHandler.ino
extern void sendMessage(const char *msg);

struct TelemetryChannel {
  String  name;
  String  owner;
  String  unit;
  float   lowRed;
  float   highRed;
  float   lowYellow;
  float   highYellow;
  String  partnumber;
  float   scale;
  float   offset;
  uint8_t pin;
  bool    isOutput;
};

static const int MAX_CHANNELS = 32;

static TelemetryChannel channels[MAX_CHANNELS];
static int   channelCount        = 0;
static bool  telemetryConfigured = false;

// Raw CSV text received from the Pi during config mode
static String telemetryCsvText;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
static uint8_t parsePin(const String &pinStr) {
  String s = pinStr;
  s.trim();
  s.toUpperCase();

  if (s.startsWith("A")) {
    int n = s.substring(1).toInt();  // "A2" -> 2
    return A0 + n;
  }

  // digital pin like "5" or "13"
  return (uint8_t)s.toInt();
}

static float toFloatSafe(const String &s, float fallback = 0.0f) {
  String t = s;
  t.trim();
  if (!t.length()) return fallback;
  return t.toFloat();
}

static bool toBoolSafe(const String &s, bool fallback = false) {
  String t = s;
  t.trim();
  t.toUpperCase();

  if (!t.length()) return fallback;

  if (t == "1" || t == "TRUE" || t == "YES" || t == "Y" || t == "OUTPUT") {
    return true;
  }

  if (t == "0" || t == "FALSE" || t == "NO" || t == "N" || t == "INPUT") {
    return false;
  }

  return fallback;
}

// Parse full CSV stored in `telemetryCsvText`
static void parseTelemetryFromString(const String &cfg) {
  channelCount        = 0;
  telemetryConfigured = false;

  int lineStart = 0;

  // Skip header line
  int nl = cfg.indexOf('\n', lineStart);
  if (nl < 0) {
    sendMessage("ERROR: Telemetry CSV missing header");
    return;
  }
  lineStart = nl + 1;

  int rowIndex = 0;  // counts DATA rows (not including header)

  while (lineStart < cfg.length() && channelCount < MAX_CHANNELS) {
    int lineEnd = cfg.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = cfg.length();

    String line = cfg.substring(lineStart, lineEnd);
    line.trim();
    lineStart = lineEnd + 1;

    if (!line.length()) {
      continue;
    }

    rowIndex++;

    // Split into up to 12 fields
    String fields[12];
    int idx   = 0;
    int start = 0;

    for (int i = 0; i < line.length() && idx < 11; i++) {
      if (line.charAt(i) == ',') {
        fields[idx++] = line.substring(start, i);
        start = i + 1;
      }
    }
    fields[idx] = line.substring(start);

    int fieldCount = idx + 1;

    // Expect at least 12 columns in the new format
    if (fieldCount < 12) {
      char buf[96];
      snprintf(buf, sizeof(buf),
               "WARN: Telemetry CSV row %d has %d columns (expected 12).",
               rowIndex, fieldCount);
      sendMessage(buf);

      String rowMsg = "Row content: \"" + line + "\"";
      sendMessage(rowMsg.c_str());

      continue;
    }

    TelemetryChannel &ch = channels[channelCount++];

    // Column order:
    //  0:  name
    //  1:  owner
    //  2:  unit
    //  3:  low_red
    //  4:  high_red
    //  5:  low_yellow
    //  6:  high_yellow
    //  7:  partnumber
    //  8:  scale
    //  9:  offset
    // 10:  pin
    // 11:  is_output

    ch.name       = fields[0];
    ch.name.trim();

    ch.owner      = fields[1];
    ch.owner.trim();

    ch.unit       = fields[2];
    ch.unit.trim();

    ch.lowRed     = toFloatSafe(fields[3], 0.0f);
    ch.highRed    = toFloatSafe(fields[4], 0.0f);
    ch.lowYellow  = toFloatSafe(fields[5], 0.0f);
    ch.highYellow = toFloatSafe(fields[6], 0.0f);

    ch.partnumber = fields[7];
    ch.partnumber.trim();

    ch.scale      = toFloatSafe(fields[8], 1.0f);
    ch.offset     = toFloatSafe(fields[9], 0.0f);
    ch.pin        = parsePin(fields[10]);
    ch.isOutput   = toBoolSafe(fields[11], false);
  }

  if (channelCount > 0) {
    telemetryConfigured = true;

    char msg[64];
    snprintf(msg, sizeof(msg),
             "Telemetry channels configured: %d",
             channelCount);
    sendMessage(msg);

  } else {
    sendMessage("ERROR: No valid telemetry rows parsed");
  }
}

// ------------------------------------------------------------
// Called from SerialHandler.ino during config upload
// ------------------------------------------------------------
void telemetry_beginConfig() {
  telemetryCsvText    = "";
  telemetryConfigured = false;
  channelCount        = 0;
  sendMessage("Telemetry config begin");
}

void telemetry_appendConfigLine(const String &line) {
  telemetryCsvText += line;
  telemetryCsvText += "\n";
}

void telemetry_endConfig() {
  sendMessage("Telemetry config end, parsing...");
  parseTelemetryFromString(telemetryCsvText);
}

// ------------------------------------------------------------
// Normal telemetry snapshot (called when Pi sends GET)
// ------------------------------------------------------------
void sendTelemetry() {
  if (!telemetryConfigured || channelCount == 0) {
    sendMessage("ERROR: Telemetry not configured");
    Serial.println("END");
    return;
  }

  for (int i = 0; i < channelCount; i++) {
    TelemetryChannel &ch = channels[i];
    float val;

    if (ch.isOutput) {
      int raw = digitalRead(ch.pin);
      val = raw * ch.scale + ch.offset;
    } else {
      int raw = analogRead(ch.pin);
      val = raw * ch.scale + ch.offset;
    }

    Serial.print(ch.name);
    Serial.print(" ");
    Serial.println(val, 3);
  }

  Serial.println("END");
}

// ------------------------------------------------------------
// read telemetry by name
// Returns NAN if:
//   - telemetry not configured
//   - channel not found
// ------------------------------------------------------------
float getTelemetry(const String& name) {
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
      if (channels[i].isOutput) {
        int raw = digitalRead(channels[i].pin);
        return raw * channels[i].scale + channels[i].offset;
      } else {
        int raw = analogRead(channels[i].pin);
        return raw * channels[i].scale + channels[i].offset;
      }
    }
  }

  return NAN;
}