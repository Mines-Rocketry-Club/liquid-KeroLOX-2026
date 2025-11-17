// Main.ino

// ---------- Forward declarations for cross-tab functions ----------
void processSerial();
void updateTelemetry();
void stateMachineLoop();

void setup() {
  Serial.begin(115200);
}

void loop() {
  processSerial();    // incoming commands & user answers
  stateMachineLoop(); // hook for continuous per-state logic (currently empty)
}
