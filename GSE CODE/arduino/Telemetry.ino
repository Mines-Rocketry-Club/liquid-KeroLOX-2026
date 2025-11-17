// Telemetry.ino
// Telemetry configuration is pushed from the Pi over serial.
//
// Protocol (handled in SerialHandler.ino):
//   CFGBEGIN          -> start config mode, clear previous CSV
//   <any CSV line>    -> append to telemetry config text
//   CFGEND            -> parse CSV & build channels
//
// NEW CSV FORMAT (11 columns):
//   name,owner,unit,low_red,high_red,low_yellow,high_yellow,partnumber,scale,offset,pin
//
// Arduino uses:
//   - name  (telemetry label)
//   - owner (stored but not used in logic yet)
//   - scale, offset, pin (last three fields) for value:
//       value = raw_adc * scale + offset
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
  String  owner;      // NEW: second column in CSV
  String  unit;
  float   lowRed;
  float   highRed;
  float   lowYellow;
  float   highYellow;
  String  partnumber;   // not used in control logic yet
  float   scale;
  float   offset;
  uint8_t pin;
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

// Safe float conversion with fallback
static float toFloatSafe(const String &s, float fallback = 0.0f) {
  String t = s;
  t.trim();
  if (!t.length()) return fallback;
  return t.toFloat();
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
      // completely blank line, ignore
      continue;
    }

    rowIndex++;

    // Split into up to 11 fields
    String fields[11];
    int idx   = 0;
    int start = 0;

    for (int i = 0; i < line.length() && idx < 10; i++) {
      if (line.charAt(i) == ',') {
        fields[idx++] = line.substring(start, i);
        start = i + 1;
      }
    }
    fields[idx] = line.substring(start);

    int fieldCount = idx + 1;

    // Expect at least 11 columns in the new format
    if (fieldCount < 11) {
      char buf[96];
      snprintf(buf, sizeof(buf),
               "WARN: Telemetry CSV row %d has %d columns (expected 11).",
               rowIndex, fieldCount);
      sendMessage(buf);

      String rowMsg = "Row content: \"" + line + "\"";
      sendMessage(rowMsg.c_str());

      // Skip this row (don't create a channel from it)
      continue;
    }

    TelemetryChannel &ch = channels[channelCount++];

    // NEW column order:
    //  0: name
    //  1: owner
    //  2: unit
    //  3: low_red
    //  4: high_red
    //  5: low_yellow
    //  6: high_yellow
    //  7: partnumber
    //  8: scale
    //  9: offset
    // 10: pin

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

    int   raw = analogRead(ch.pin);
    float val = raw * ch.scale + ch.offset;

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
      int raw = analogRead(channels[i].pin);
      return raw * channels[i].scale + channels[i].offset;
    }
  }

  // No matching name
  return NAN;
}
