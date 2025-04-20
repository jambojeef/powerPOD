#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include "Audio.h"
#include "driver/ledc.h"
#include "esp_sleep.h"

// ——— Wi‑Fi Credentials ———
const char* ssid     = "VM2907217";
const char* password = "b8cyFsdNkkst";

// ——— OTA Settings ———
// Replace with your GitHub Releases raw URL to powerPOD.bin
const char* firmwareURL = "https://github.com/jambojeef/powerPOD/commit/cbf4b92872b77e644c985fd420a76e381cab8797#diff-0f93f244bf850355adb654094989c76806c61af6bcc38c2082161e0ff3aec65f";

// Root CA PEM for github.com (Let’s Encrypt ISRG Root X1)
const char* rootCACert = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgISA6z...
...rest of your CA’s PEM...
-----END CERTIFICATE-----
)EOF";

// ——— Audio URLs ———
const char* track1URL  = "https://dl.dropboxusercontent.com/.../0001.mp3";
const char* track2URL  = "https://dl.dropboxusercontent.com/.../0002.mp3";
const char* track3URL  = "https://dl.dropboxusercontent.com/.../0003.mp3";

// ——— GPIO Definitions ———
const int lockNOPin    = 32;  // wakes on LOW
const int lockNCPin    = 13;
const int buttonPin    = 33;
const int ledStripPin  = 14;  // PWM ch0
const int buttonLEDPin = 27;  // PWM ch1
const int relayPin     = 12;

// ——— I2S Pins ———
const int I2S_BCLK = 26, I2S_LRC = 25, I2S_DOUT = 22;

// ——— State Machine ———
enum SystemState { IDLE, WELCOME, CHARGING, OCCUPIED, GOODBYE };
SystemState state = IDLE;

// ——— Timing & Flags ———
unsigned long chargeStartTime     = 0;
const unsigned long chargeDuration = 3600000UL;  // 1 hour
bool relayToggledOff              = false;

// ——— Fade Control ———
unsigned long fadeStart = 0, fadeDur = 0;
int          fadeFrom   = 0, fadeTo = 0;
bool         fadeActive = false;

// ——— SPST LED Pulse ———
int ledPulseValue  = 0, pulseDirection = 1;

// ——— Edge Detect ———
bool prevLockOpen = false;

// ——— Audio Object ———
Audio audio;

// ——— Function Prototypes ———
void startFade(int from, int to, unsigned long duration);
void updateFade();
void fadeLED(int from, int to, unsigned long dur);
void pulseRelay();
void checkForUpdates();

void setup() {
  Serial.begin(115200);

  // I/O
  pinMode(lockNOPin, INPUT_PULLUP);
  pinMode(lockNCPin, INPUT_PULLUP);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);

  // PWM channels
  ledcSetup(0, 1000, 8);
  ledcAttachPin(ledStripPin, 0);
  ledcSetup(1,  500, 8);
  ledcAttachPin(buttonLEDPin, 1);

  // Wi‑Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n✅ WiFi up, IP=" + WiFi.localIP().toString());

  // Audio init
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(20);

  // Wake on door‑open
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, 0);
  auto reason = esp_sleep_get_wakeup_cause();
  if (reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke by door‑open → WELCOME");
    startFade(0, 255, 8000);
    audio.connecttohost(track1URL);
    state = WELCOME;
  } else {
    Serial.println("Normal boot → IDLE");
    state = IDLE;
  }

  prevLockOpen = (digitalRead(lockNOPin)==LOW && digitalRead(lockNCPin)==HIGH);
}

void loop() {
  // 0) Ensure strip off in IDLE
  if (state == IDLE) {
    fadeActive = false;
    ledcWrite(0, 0);
    ledcDetachPin(ledStripPin);
    pinMode(ledStripPin, OUTPUT);
    digitalWrite(ledStripPin, LOW);
  } else {
    pinMode(ledStripPin, INPUT);
    ledcAttachPin(ledStripPin, 0);
  }

  audio.loop();

  // 1) Inputs & edge‑detect
  bool lockOpen   = (digitalRead(lockNOPin)==LOW  && digitalRead(lockNCPin)==HIGH);
  bool lockClosed = (digitalRead(lockNCPin)==LOW  && digitalRead(lockNOPin)==HIGH);
  bool btnPress   = (digitalRead(buttonPin)==LOW);
  bool openEdge   = lockOpen  && !prevLockOpen;
  bool closeEdge  = lockClosed && prevLockOpen;
  prevLockOpen    = lockOpen;

  // 2) State machine
  switch (state) {
    case IDLE:
      relayToggledOff = false;
      if (openEdge) {
        Serial.println("→ WELCOME");
        startFade(0, 255, 8000);
        audio.connecttohost(track1URL);
        state = WELCOME;
      }
      break;

    case WELCOME:
      if (btnPress) {
        Serial.println("→ CHARGING");
        audio.stopSong();
        audio.connecttohost(track2URL);
        pulseRelay();
        chargeStartTime = millis();
        state = CHARGING;
      }
      break;

    case CHARGING:
      if (!fadeActive) {
        ledcWrite(0, ((millis()/500)%2) ? 255 : 0);
      }
      if (closeEdge) {
        Serial.println("→ OCCUPIED");
        fadeActive = false;
        ledcWrite(0, 0);
        state = OCCUPIED;
      }
      if (!relayToggledOff && millis() - chargeStartTime >= chargeDuration) {
        Serial.println("⚡ Timer done, stopping charge");
        pulseRelay();
        relayToggledOff = true;
      }
      break;

    case OCCUPIED:
      if (openEdge) {
        Serial.println("→ GOODBYE");
        audio.connecttohost(track3URL);
        startFade(0, 255, 1000);
        state = GOODBYE;
      }
      break;

    case GOODBYE:
      if (closeEdge) {
        Serial.println("→ Checking for OTA update…");
        checkForUpdates();  // now uses httpUpdate.update(...)
        Serial.println("→ No update applied; entering IDLE");
        fadeLED(255, 0, 500);
        fadeActive = false;
        ledcDetachPin(ledStripPin);
        pinMode(ledStripPin, OUTPUT);
        digitalWrite(ledStripPin, LOW);
        audio.stopSong();
        esp_deep_sleep_start();
      }
      break;
  }

  // 3) Update fade if active
  if (fadeActive && state != IDLE) {
    updateFade();
  }

  // 4) SPST‑LED behavior
  if (state == WELCOME && !audio.isRunning() && !fadeActive) {
    ledcWrite(1, ledPulseValue);
    ledPulseValue += pulseDirection * 4;
    if (ledPulseValue <= 0) {
      ledPulseValue  = 0; pulseDirection = 1;
    } else if (ledPulseValue >= 255) {
      ledPulseValue  = 255; pulseDirection = -1;
    }
  }
  else if (state == CHARGING) {
    ledcWrite(1, 255);
  }
  else {
    ledcWrite(1, 0);
    ledPulseValue  = 0;
    pulseDirection = 1;
  }

  delay(10);
}

// ——— Non‑blocking fade (smootherstep) ———
void startFade(int from, int to, unsigned long duration) {
  fadeFrom   = from;
  fadeTo     = to;
  fadeDur    = duration;
  fadeStart  = millis();
  fadeActive = true;
}
void updateFade() {
  unsigned long elapsed = millis() - fadeStart;
  if (elapsed >= fadeDur) {
    ledcWrite(0, fadeTo);
    fadeActive = false;
    return;
  }
  float t    = float(elapsed) / fadeDur;
  float ease = t*t*t*(t*(6*t - 15) + 10);
  int b      = fadeFrom + int((fadeTo - fadeFrom) * ease);
  ledcWrite(0, b);
}

// ——— Blocking fade for GOODBYE ———
void fadeLED(int from, int to, unsigned long dur) {
  int steps = abs(to - from), dir = (to > from ? 1 : -1);
  for (int i = 0; i <= steps; i++) {
    ledcWrite(0, from + dir * i);
    delay(dur / steps);
  }
}

// ——— Relay pulse ———
void pulseRelay() {
  digitalWrite(relayPin, LOW);
  delay(200);
  digitalWrite(relayPin, HIGH);
}

// ——— Secure OTA update check ———
void checkForUpdates() {
  WiFiClientSecure client;
  client.setCACert(rootCACert);

  // ESP32 uses httpUpdate.update(...)
  t_httpUpdate_return ret = httpUpdate.update(
    client,
    firmwareURL,
    "1.0.0"        // current version string
  );
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA failed (%d): %s\n",
        httpUpdate.getLastError(),
        httpUpdate.getLastErrorString().c_str()
      );
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: no update available");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA OK, rebooting");
      break;
  }
}
