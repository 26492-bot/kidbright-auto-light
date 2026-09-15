// ไฟอัตโนมัติ KidBright v1.3
// เซนเซอร์แสง (LDR, GPIO36) + จอ LED 16x8 (HT16K33, I2C 0x70) บนบอร์ดเท่านั้น ไม่ต่อสายเพิ่ม
// ปุ่ม S1 (GPIO16) = บังคับเปิด, S2 (GPIO14) = บังคับปิด, กดค้างพร้อมกัน = กลับโหมดอัตโนมัติ

#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"

#define LDR_PIN 36
#define S1_PIN 16
#define S2_PIN 14
// วัดจริงจากบอร์ดนี้: ห้องปกติ raw ~430-510, บังมือมืดสนิท raw ~590-745
// (ค่า raw "ยิ่งมาก = ยิ่งมืด" สำหรับ LDR ตัวนี้)
#define LDR_BRIGHT_RAW 400
#define LDR_DARK_RAW 760
#define DARK_ON_THRESHOLD 45   // มืดกว่านี้ -> เปิดไฟ (ไม่ต้องรอมืดสนิท)
#define DARK_OFF_THRESHOLD 60  // สว่างกว่านี้ -> ปิดไฟ (เว้นช่วงกันไฟกระพริบ)
#define MIN_TOGGLE_INTERVAL_MS 2000
#define SMOOTH_ALPHA 0.15
#define BOTH_HOLD_MS 800
#define DEBOUNCE_MS 30

Adafruit_8x16minimatrix matrix;

enum Mode { MODE_AUTO, MODE_ON, MODE_OFF };
Mode mode = MODE_AUTO;

bool lastS1 = HIGH, lastS2 = HIGH;
unsigned long s1DebounceAt = 0, s2DebounceAt = 0;
unsigned long bothHeldSince = 0;
bool lampOn = false;
unsigned long lastToggleAt = 0;
float smoothedRaw = -1;

uint8_t readLight() {
  int raw = analogRead(LDR_PIN);
  smoothedRaw = (smoothedRaw < 0) ? raw : (smoothedRaw + SMOOTH_ALPHA * (raw - smoothedRaw));
  int rawC = constrain((int)smoothedRaw, LDR_BRIGHT_RAW, LDR_DARK_RAW);
  int value = map(rawC, LDR_BRIGHT_RAW, LDR_DARK_RAW, 100, 0);
  return (uint8_t)value;
}

void setLamp(bool on) {
  lampOn = on;
  if (on) {
    matrix.fillScreen(LED_ON);
  } else {
    matrix.clear();
  }
  matrix.writeDisplay();
}

void handleButtons() {
  bool s1 = digitalRead(S1_PIN);
  bool s2 = digitalRead(S2_PIN);
  unsigned long now = millis();

  if (s1 == LOW && s2 == LOW) {
    if (bothHeldSince == 0) bothHeldSince = now;
    if (now - bothHeldSince > BOTH_HOLD_MS && mode != MODE_AUTO) {
      mode = MODE_AUTO;
      Serial.println("MODE -> AUTO (both buttons)");
    }
  } else {
    bothHeldSince = 0;

    if (s1 != lastS1 && now - s1DebounceAt > DEBOUNCE_MS) {
      s1DebounceAt = now;
      if (s1 == LOW) {
        mode = MODE_ON;
        Serial.println("MODE -> ON (S1)");
      }
    }
    if (s2 != lastS2 && now - s2DebounceAt > DEBOUNCE_MS) {
      s2DebounceAt = now;
      if (s2 == LOW) {
        mode = MODE_OFF;
        Serial.println("MODE -> OFF (S2)");
      }
    }
  }
  lastS1 = s1;
  lastS2 = s2;
}

void setup() {
  Serial.begin(115200);
  pinMode(LDR_PIN, INPUT);
  pinMode(S1_PIN, INPUT_PULLUP);
  pinMode(S2_PIN, INPUT_PULLUP);
  matrix.begin(0x70);
  matrix.setRotation(1);
  matrix.clear();
  matrix.writeDisplay();
  Serial.println("KidBright auto light ready.");
}

void loop() {
  handleButtons();

  uint8_t light = readLight();
  unsigned long now = millis();
  bool wantOn;
  if (mode == MODE_ON) wantOn = true;
  else if (mode == MODE_OFF) wantOn = false;
  else wantOn = lampOn ? (light < DARK_OFF_THRESHOLD) : (light < DARK_ON_THRESHOLD);

  bool modeForcesChange = (mode != MODE_AUTO);
  bool cooldownOver = (now - lastToggleAt) > MIN_TOGGLE_INTERVAL_MS;

  if (wantOn != lampOn && (modeForcesChange || cooldownOver)) {
    setLamp(wantOn);
    lastToggleAt = now;
    Serial.print("Light=");
    Serial.print(light);
    Serial.print(" mode=");
    Serial.print(mode == MODE_AUTO ? "auto" : (mode == MODE_ON ? "on" : "off"));
    Serial.print(" -> lamp=");
    Serial.println(wantOn ? "ON" : "OFF");
  }

  delay(50);
}
