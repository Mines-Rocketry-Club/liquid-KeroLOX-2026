// Prompts.ino
#include <Arduino.h>

extern bool   g_answerReady;
extern String g_answerText;
extern const char* g_password;

static bool   g_promptActive = false;
static String g_promptText;
static const String* g_options = nullptr;
static int    g_optionCount = 0;

static bool   g_pwActive = false;
static String g_pwPrompt = "Enter password:";

static bool equalsIgnoreCase(String a, String b) {
  a.trim(); b.trim();
  a.toUpperCase(); b.toUpperCase();
  return a == b;
}

void promptCancel() {
  g_promptActive = false;
  g_promptText = "";
  g_options = nullptr;
  g_optionCount = 0;
  g_pwActive = false;
}

void promptResendIfActive() {
  if (g_promptActive) {
    Serial.print("PROMPT ");
    Serial.println(g_promptText);
  } else if (g_pwActive) {
    Serial.print("PROMPT ");
    Serial.println(g_pwPrompt);
  }
}

int promptUserSelect(const String& prompt,
                     const String optionsIn[],
                     int optionCountIn)
{
  if (g_pwActive) promptCancel();

  if (!g_promptActive || prompt != g_promptText) {
    g_promptActive = true;
    g_promptText = prompt;
    g_options = optionsIn;
    g_optionCount = optionCountIn;
    Serial.print("PROMPT ");
    Serial.println(g_promptText);
  }

  if (!g_answerReady) return -1;

  String ans = g_answerText;
  g_answerReady = false;
  g_answerText = "";
  ans.trim();

  if (!ans.length()) {
    promptCancel();
    return 0;
  }

  bool digits = true;
  for (int i = 0; i < ans.length(); i++) {
    if (!isDigit(ans[i])) { digits = false; break; }
  }

  if (digits) {
    int n = ans.toInt();
    int max = g_optionCount;
    promptCancel();
    if (n >= 1 && n <= max) return n;
    return 0;
  }

  for (int i = 0; i < g_optionCount; i++) {
    if (equalsIgnoreCase(ans, g_options[i])) {
      promptCancel();
      return i + 1;
    }
  }

  promptCancel();
  return 0;
}

int askPassword() {
  if (g_promptActive) promptCancel();

  if (!g_pwActive) {
    g_pwActive = true;
    Serial.print("PROMPT ");
    Serial.println(g_pwPrompt);
  }

  if (!g_answerReady) return -1;

  String ans = g_answerText;
  g_answerReady = false;
  g_answerText = "";
  ans.trim();

  if (!ans.length()) {
    Serial.println("MSG Incorrect password");
    g_pwActive = false;
    return 0;
  }

  if (ans == String(g_password)) {
    g_pwActive = false;
    return 1;
  }

  Serial.println("MSG Incorrect password");
  g_pwActive = false;
  return 0;
}

