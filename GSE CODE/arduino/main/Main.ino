// Main.ino
#include <Arduino.h>

const char* g_password = "Groundstation";

#include "Prompts.h"
#include "Telemetry.h"
#include "StateMachine.h"
#include "SerialHandler.h"

extern bool g_telemetryPending;

void setup() {
  Serial.begin(57600);
  initStateMachine();
}

void loop() {
  processSerial();

  // Service telemetry after steppers tick — never mid-serial-receive.
  // Ensures END is never delayed by a stepper move, preventing the Pi's
  // _waiting_for_snapshot flag from locking out subsequent GETs.
  if (g_telemetryPending) {
    g_telemetryPending = false;
    sendTelemetry();
  }

  stateMachineLoop();  // tickAllSteppers() is called inside here
}
