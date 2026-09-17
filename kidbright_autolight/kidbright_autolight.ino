// ไฟอัตโนมัติ KidBright v1.3
// เซนเซอร์แสง (LDR, GPIO36) + จอ LED 16x8 (HT16K33, I2C 0x70) บนบอร์ดเท่านั้น ไม่ต่อสายเพิ่ม
// ปุ่ม S1 (GPIO16) = บังคับเปิด, S2 (GPIO14) = บังคับปิด, กดค้างพร้อมกัน = กลับโหมดอัตโนมัติ

#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>

// ลองต่อเน็ตที่รู้จักอยู่แล้วก่อน (เร็ว ไม่ต้องตั้งค่าอะไร)
#define KNOWN_WIFI_SSID "TNW-WIFI2"
#define KNOWN_WIFI_PASS ""  // เน็ตเปิด ไม่มีรหัส
#define KNOWN_WIFI_TIMEOUT_MS 8000
// ถ้าต่อเน็ตที่รู้จักไม่ได้ (ย้ายที่/เปลี่ยนเน็ต) บอร์ดจะเปิดเป็น WiFi ของตัวเอง
// ชื่อนี้ ให้เอามือถือ/คอมไปต่อ แล้วหน้าเว็บตั้งค่าจะเด้งขึ้นมาเองให้เลือกเน็ตใหม่
#define SETUP_AP_NAME "KidBright-Setup"
#define SETUP_PORTAL_TIMEOUT_S 180
// ntfy.sh: บริการ pub/sub ฟรี ไม่ต้องสมัคร ไม่ต้องยืนยันอีเมล
#define NTFY_CMD_URL "https://ntfy.sh/kidbright-cmd-Gs19S5zFML4da5fiUmsi8p"
#define NTFY_CMD_POLL_URL "https://ntfy.sh/kidbright-cmd-Gs19S5zFML4da5fiUmsi8p/json?poll=1&since=60s"
#define NTFY_STATUS_URL "https://ntfy.sh/kidbright-status-Gs19S5zFML4da5fiUmsi8p"
// ntfy.sh (ฟรี ไม่ล็อกอิน) จำกัด ~1 คำขอ/5 วิ และ 250 ข้อความ/วัน ต่อ IP
// โดนจำกัดซ้ำหลายรอบจากการใช้งานสะสมทั้งวัน (เปิดเว็บทิ้งไว้ + อัปโหลดโค้ดหลายรอบ)
// ยืดรอบให้ห่างขึ้นอีกเพื่อความเสถียรตอนพรีเซนต์ แลกกับความเร็วที่ลดลงบ้าง
#define POLL_INTERVAL_MS 20000
#define WIFI_RETRY_MS 5000

#define LDR_PIN 36
#define S1_PIN 16
#define S2_PIN 14
// วัดจริงจากบอร์ดนี้: ห้องปกติ raw ~430-510, บังมือมืดสนิท raw ~590-745
// (ค่า raw "ยิ่งมาก = ยิ่งมืด" สำหรับ LDR ตัวนี้)
#define LDR_BRIGHT_RAW 400
#define LDR_DARK_RAW 760
// ต้อง ON < OFF เสมอ (ค่าน้อย=มืด) ไม่งั้นไฟจะกระพริบตอนแสงแกว่งอยู่กลางๆ
#define DARK_ON_THRESHOLD 70   // มืดกว่านี้ -> เปิดไฟ
#define DARK_OFF_THRESHOLD 75  // สว่างกว่านี้ -> ปิดไฟ (เว้นช่วงกันไฟกระพริบ)
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
unsigned long lastPollAt = 0;
unsigned long lastWifiAttempt = 0;
uint8_t lastLight = 0;
bool wasWifiConnected = false;

Preferences prefs;
unsigned long totalOnSeconds = 0;  // สะสมตลอดอายุบอร์ด เก็บถาวรใน flash (NVS)
unsigned long lampOnSinceMs = 0;   // millis() ตอนไฟติดครั้งล่าสุด (0 = ตอนนี้ไฟดับ)

const char *modeStr() {
  return mode == MODE_AUTO ? "auto" : (mode == MODE_ON ? "on" : "off");
}

// เน็ตเปิดไม่มีรหัส ต้องเรียก WiFi.begin(ssid) เฉยๆ ห้ามส่ง "" เป็นรหัสผ่าน
// (บาง core ของ ESP32 จะพยายามยืนยันตัวตนแบบ WPA ด้วยรหัสว่างแล้วค้าง/รีเซ็ต)
void beginKnownWiFi() {
  if (strlen(KNOWN_WIFI_PASS) == 0) {
    WiFi.begin(KNOWN_WIFI_SSID);
  } else {
    WiFi.begin(KNOWN_WIFI_SSID, KNOWN_WIFI_PASS);
  }
}

// เรียกครั้งเดียวใน setup(): ลองเน็ตที่รู้จักก่อน ถ้าไม่เจอค่อยเปิดฮอตสปอตตั้งค่า
// ให้เอามือถือ/คอมไปต่อแล้วเลือกเน็ตใหม่ผ่านหน้าเว็บที่เด้งขึ้นเอง (ไม่ต้องแก้โค้ด/อัปโหลดใหม่)
void connectWiFiEasy() {
  WiFi.mode(WIFI_STA);

  // กด S1+S2 ค้างไว้ตอนเปิด/รีเซ็ตบอร์ด เพื่อข้ามเน็ตเดิมแล้วตั้งค่าเน็ตใหม่ทันที
  // (เช่นตอนพรีเซนต์อยากสลับไปใช้ฮอตสปอตมือถือแทนเน็ตโรงเรียน)
  bool skipKnown = (digitalRead(S1_PIN) == LOW && digitalRead(S2_PIN) == LOW);
  if (skipKnown) {
    Serial.println("WiFi: S1+S2 held at boot -> skipping known network, forcing setup hotspot");
  } else {
    Serial.println("WiFi: trying known network " KNOWN_WIFI_SSID "...");
    beginKnownWiFi();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < KNOWN_WIFI_TIMEOUT_MS) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      wasWifiConnected = true;
      Serial.print("WiFi: connected to known network, IP=");
      Serial.println(WiFi.localIP());
      return;
    }
  }

  Serial.println("WiFi: opening setup hotspot " SETUP_AP_NAME "...");
  WiFiManager wm;
  wm.setConfigPortalTimeout(SETUP_PORTAL_TIMEOUT_S);
  bool ok = wm.autoConnect(SETUP_AP_NAME);
  if (ok) {
    wasWifiConnected = true;
    Serial.print("WiFi: connected via setup hotspot, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi: not configured, continuing offline (auto light + S1/S2 still work)");
  }
}

// เรียกทุกลูป: ถ้าหลุดกลางคัน ลองต่อเน็ตที่รู้จักซ้ำเป็นระยะ (ไม่เปิดฮอตสปอตซ้ำระหว่างทำงาน)
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wasWifiConnected) {
      wasWifiConnected = true;
      Serial.print("WiFi: connected, IP=");
      Serial.println(WiFi.localIP());
    }
    return;
  }
  if (wasWifiConnected) {
    wasWifiConnected = false;
    Serial.println("WiFi: disconnected");
  }
  unsigned long now = millis();
  if (now - lastWifiAttempt < WIFI_RETRY_MS) return;
  lastWifiAttempt = now;
  Serial.println("WiFi: retrying known network...");
  beginKnownWiFi();
}

// ดึง "message" ตัวล่าสุดจาก NDJSON ที่ ntfy.sh ส่งกลับมา (เอาบรรทัดสุดท้าย)
String extractLastMessage(const String &body) {
  int idx = body.lastIndexOf("\"message\":\"");
  if (idx < 0) return "";
  int start = idx + 11;  // ความยาวของ "message":"
  int end = body.indexOf('"', start);
  if (end < 0) return "";
  return body.substring(start, end);
}

// ดึงคำสั่งจากเว็บ (auto/on/off) แล้วส่งสถานะปัจจุบันกลับไปให้เว็บอ่าน ผ่าน ntfy.sh
void syncWithWeb() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastPollAt < POLL_INTERVAL_MS) return;
  lastPollAt = now;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);
  HTTPClient https;
  https.setConnectTimeout(5000);
  https.setTimeout(5000);

  Serial.print("sync: GET cmd code=");
  if (https.begin(client, NTFY_CMD_POLL_URL)) {
    int code = https.GET();
    Serial.print(code);
    if (code == 200) {
      String body = https.getString();
      String cmd = extractLastMessage(body);
      cmd.trim();
      Serial.print(" cmd=\"");
      Serial.print(cmd);
      Serial.print("\"");
      if (cmd == "on") mode = MODE_ON;
      else if (cmd == "off") mode = MODE_OFF;
      else if (cmd == "auto") mode = MODE_AUTO;
    }
    https.end();
  } else {
    Serial.print("begin-failed");
  }
  Serial.println();

  // เว้นช่วงก่อนยิงคำขอที่สอง — ถ้ายิงติดกันทันทีจะไม่มีโควตาเหลือให้ POST
  // เพราะ ntfy.sh เติมโควตาให้แค่ ~1 คำขอ/5 วินาที
  delay(5500);

  String status = String("{\"light\":") + lastLight +
                  ",\"lamp\":" + (lampOn ? "true" : "false") +
                  ",\"mode\":\"" + modeStr() +
                  "\",\"onSec\":" + currentTotalOnSeconds() + "}";
  Serial.print("sync: POST status code=");
  if (https.begin(client, NTFY_STATUS_URL)) {
    https.addHeader("Content-Type", "text/plain");
    int code = https.POST(status);
    Serial.println(code);
    if (code != 200) {
      Serial.print("ntfy POST status failed, code=");
      Serial.println(code);
    }
    https.end();
  }
}

uint8_t readLight() {
  int raw = analogRead(LDR_PIN);
  smoothedRaw = (smoothedRaw < 0) ? raw : (smoothedRaw + SMOOTH_ALPHA * (raw - smoothedRaw));
  int rawC = constrain((int)smoothedRaw, LDR_BRIGHT_RAW, LDR_DARK_RAW);
  int value = map(rawC, LDR_BRIGHT_RAW, LDR_DARK_RAW, 100, 0);
  return (uint8_t)value;
}

void setLamp(bool on) {
  if (on && !lampOn) {
    lampOnSinceMs = millis();
  } else if (!on && lampOn) {
    totalOnSeconds += (millis() - lampOnSinceMs) / 1000;
    prefs.putULong("onSec", totalOnSeconds);
    lampOnSinceMs = 0;
  }
  lampOn = on;
  if (on) {
    matrix.fillScreen(LED_ON);
  } else {
    matrix.clear();
  }
  matrix.writeDisplay();
}

// ใช้ตอนรายงานสถานะ: รวมเวลาที่ติดค้างอยู่ตอนนี้เข้ากับยอดสะสมด้วย (ยังไม่ได้ persist จนกว่าจะปิด)
unsigned long currentTotalOnSeconds() {
  if (lampOn) return totalOnSeconds + (millis() - lampOnSinceMs) / 1000;
  return totalOnSeconds;
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
  prefs.begin("kidbright", false);
  totalOnSeconds = prefs.getULong("onSec", 0);
  Serial.print("Loaded total on-time: ");
  Serial.print(totalOnSeconds);
  Serial.println("s");
  pinMode(LDR_PIN, INPUT);
  pinMode(S1_PIN, INPUT_PULLUP);
  pinMode(S2_PIN, INPUT_PULLUP);
  matrix.begin(0x70);
  matrix.setRotation(1);
  matrix.clear();
  matrix.writeDisplay();
  connectWiFiEasy();
  Serial.println("KidBright auto light ready.");
}

void loop() {
  handleButtons();
  maintainWiFi();

  uint8_t light = readLight();
  lastLight = light;
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

  syncWithWeb();

  delay(50);
}
