// Main.ino
const char* g_password = "Groundstation";

void processSerial();
void initStateMachine();
void stateMachineLoop();

void markPiGetReceived();
bool isPiGetTimedOut();

void setup() {
  Serial.begin(115200);
  initStateMachine();
}

void loop() {
  processSerial();
  stateMachineLoop();
}