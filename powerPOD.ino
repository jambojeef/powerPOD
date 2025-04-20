#include <WiFi.h>
#include "Audio.h"
#include "driver/ledc.h"
#include "esp_sleep.h"

// ——— Wi‑Fi & Audio URLs ———
const char* ssid       = "VM2907217";
const char* password   = "b8cyFsdNkkst";
const char* track1URL  = "https://dl.dropboxusercontent.com/…/0001.mp3";
const char* track2URL  = "https://dl.dropboxusercontent.com/…/0002.mp3";
const char* track3URL  = "https://dl/dropboxusercontent.com/…/0003.mp3";

// ——— GPIOs ———
const int lockNOPin    = 32;  // wakes on LOW
const int lockNCPin    = 13;
const int buttonPin    = 33;
const int ledStripPin  = 14;  // PWM channel 0
const int buttonLEDPin = 27;  // PWM channel 1
const int relayPin     = 12;

// ——— I2S for Audio ———
const int I2S_BCLK = 26, I2S_LRC = 25, I2S_DOUT = 22;

// ——— State Machine ———
enum SystemState { IDLE, WELCOME, CHARGING, OCCUPIED, GOODBYE };
SystemState state = IDLE;

// ——— Charge timing ———
unsigned long chargeStartTime      = 0;
const unsigned long chargeDuration = 3600000UL;  // 1 hour
bool relayToggledOff               = false;

// ——— Non‑blocking fade vars ———
unsigned long fadeStart  = 0, fadeDur = 0;
int          fadeFrom    = 0, fadeTo = 0;
bool         fadeActive  = false;

// ——— SPST‑LED pulse vars ———
int ledPulseValue  = 0, pulseDirection = 1;

// ——— Edge detection ———
bool prevLockOpen = false;

// ——— Audio ———
Audio audio;

// ——— Helper: blocking fade for GOODBYE ———
void fadeLED(int from, int to, unsigned long dur) {
  int steps = abs(to - from), dir = (to > from ? 1 : -1);
  for (int i = 0; i <= steps; i++) {
    ledcWrite(0, from + dir * i);
    delay(dur / steps);
  }
}

// ——— Helper: relay pulse ———
void pulseRelay() {
  digitalWrite(relayPin, LOW);
  delay(200);
  digitalWrite(relayPin, HIGH);
}

// ——— Helpers: non‑blocking fade with “smootherstep” easing ———
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
  // smootherstep: 6t^5 - 15t^4 + 10t^3
  float t = (float)elapsed / fadeDur;
  float ease = t*t*t*(t*(6*t - 15) + 10);
  int b = fadeFrom + (int)((fadeTo - fadeFrom) * ease);
  ledcWrite(0, b);
}

void setup() {
  Serial.begin(115200);

  // I/O setup
  pinMode(lockNOPin,    INPUT_PULLUP);
  pinMode(lockNCPin,    INPUT_PULLUP);
  pinMode(buttonPin,    INPUT_PULLUP);
  pinMode(relayPin,     OUTPUT);
  digitalWrite(relayPin, HIGH);

  // PWM channels
  ledcSetup(0, 1000, 8);
  ledcAttachPin(ledStripPin, 0);
  ledcSetup(1,  500, 8);
  ledcAttachPin(buttonLEDPin, 1);

  // Wi‑Fi connect
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n✅ WiFi up, IP=" + WiFi.localIP().toString());

  // Audio init
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(20);

  // Configure deep‑sleep wake on door‑open
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, 0);
  auto reason = esp_sleep_get_wakeup_cause();
  if (reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke by door‑open → WELCOME");
    // now 8 s fade with smoothstep
    startFade(0, 255, 8000);
    audio.connecttohost(track1URL);
    state = WELCOME;
  } else {
    Serial.println("Normal boot → IDLE");
    state = IDLE;
  }

  prevLockOpen = digitalRead(lockNOPin)==LOW && digitalRead(lockNCPin)==HIGH;
}

void loop() {
  // 0) Immediately shut off strip in IDLE
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

  // 1) Read inputs & detect edges
  bool lockOpen   = digitalRead(lockNOPin)==LOW && digitalRead(lockNCPin)==HIGH;
  bool lockClosed = digitalRead(lockNCPin)==LOW && digitalRead(lockNOPin)==HIGH;
  bool btnPress   = digitalRead(buttonPin)==LOW;
  bool openEdge   = lockOpen  && !prevLockOpen;
  bool closeEdge  = lockClosed && prevLockOpen;
  prevLockOpen    = lockOpen;

  // 2) State machine
  switch (state) {
    case IDLE:
      relayToggledOff = false;
      if (openEdge) {
        Serial.println("→ WELCOME");
        startFade(0, 255, 8000);  // 8 s smooth fade‑in
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
        Serial.println("→ IDLE, sleeping");
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

  // 3) Update any active fade
  if (fadeActive && state != IDLE) {
    updateFade();
  }

  // 4) SPST‑LED behavior
  if (state == WELCOME && !audio.isRunning() && !fadeActive) {
    ledcWrite(1, ledPulseValue);
    ledPulseValue += pulseDirection * 4;
    if (ledPulseValue <= 0) {
      ledPulseValue  = 0;
      pulseDirection = 1;
    } else if (ledPulseValue >= 255) {
      ledPulseValue  = 255;
      pulseDirection = -1;
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
