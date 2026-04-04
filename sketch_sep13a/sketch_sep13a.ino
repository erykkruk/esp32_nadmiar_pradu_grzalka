/*
  ESP32 Heater Control v2 - "Dumb Executor" with Server-Side Logic
  (System Kontroli Grzalki ESP32 v2 - "Glupi Executor" z Logika po Stronie Serwera)

  Architecture (Architektura):
    ESP32 reads power from meter and sends to Next.js server every 1s.
    Server runs control algorithm (gross export, proportional targeting, EMA).
    Server responds with duty% for SSR.
    ESP32 just sets the duty on SSR - no control logic here.

  Safety System (System Bezpieczenstwa):
    NORMAL  --(no response 10s)--> RAMP_DOWN --(duty=0%)--> SAFE_MODE
    ESP does NOT depend on server to safely shut down.

  Hardware:
    RS485/Modbus: Serial2 RX=GPIO16, TX=GPIO17, DE/RE=GPIO4, 9600 O-8-1
    SSR Control:  GPIO13 (burst-fire, 50Hz half-cycles)
    Energy Meter: DTSU666, slave ID=1, register 0x2012

  Network:
    WiFi: configurable below
    Server: POST /api/esp/report every 1s
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ModbusMaster.h>

//////////////////// Wi-Fi Configuration ////////////////////
const char* WIFI_SSID = "TP-Link_E6D1";
const char* WIFI_PASS = "80246459";
IPAddress local_IP(192,168,255,50), gateway(192,168,255,1),
         subnet(255,255,255,0), dns1(192,168,255,1), dns2(8,8,8,8);

//////////////////// Server Configuration ////////////////////
const char* SERVER_URL = "https://TWOJ-ADRES.coolify.app/api/esp/report";  // <- ZMIEN
const char* API_KEY    = "change-me-to-a-random-secret";                    // <- ZMIEN

//////////////////// RS485 / Modbus Configuration ////////////////////
#define RS485_DE_RE 4
#define UART_BAUD   9600
#define SERIAL_MODE SERIAL_8O1
const uint8_t METER_ID = 1;

HardwareSerial& RS485 = Serial2;
ModbusMaster node;

//////////////////// SSR Configuration ////////////////////
const int SSR_PIN = 13;
const float P_MAX = 2000.0f;
const int HALF_CYCLES_PER_SECOND = 100;  // 50Hz = 100 half-cycles/s
const int N_HALF = HALF_CYCLES_PER_SECOND;  // 1 second window

//////////////////// Safety Configuration ////////////////////
const unsigned long GRACE_PERIOD_MS     = 10000;  // 10s normal tolerance
const unsigned long RAMP_DOWN_START_MS  = 10000;  // Start ramping after 10s
const float         RAMP_STEP_PCT       = 5.0f;   // -5% per second
const float         SAFE_MODE_DUTY      = 0.0f;

//////////////////// State Variables ////////////////////
// Meter reading
float powerW = 0.0f;           // Current power reading [W] (+ import, - export)
unsigned long lastMeterOkMs = 0;

// SSR control
float currentDutyPct = 0.0f;   // Current duty cycle [0-100]
int   onCycles = 0;            // Half-cycles ON per window
int   phaseIndex = 0;
unsigned long lastHalfMs = 0;

// Safety
unsigned long lastResponseMs = 0;  // Last successful server response
bool safeMode = false;

// Timing
unsigned long lastPollMs = 0;
unsigned long lastSendMs = 0;
unsigned long lastSafetyCheckMs = 0;

//////////////////// RS485 Direction Control ////////////////////
void preTransmission()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE, LOW);  }

//////////////////// Modbus: Read Power ////////////////////
bool readFloat32(uint16_t reg, float scale, float& outVal) {
  for (int i = 0; i < 2; i++) {
    uint8_t r = node.readHoldingRegisters(reg, 2);
    if (r == node.ku8MBSuccess) {
      uint16_t hi = node.getResponseBuffer(0);
      uint16_t lo = node.getResponseBuffer(1);
      uint32_t raw = ((uint32_t)hi << 16) | lo;
      float v;
      memcpy(&v, &raw, sizeof(float));
      outVal = v / scale;
      return true;
    }
    delay(5);
  }
  return false;
}

void pollMeter() {
  float v;
  if (readFloat32(0x2012, 10.0f, v)) {
    powerW = v;
    lastMeterOkMs = millis();
  }
}

//////////////////// SSR Burst-Fire Control ////////////////////
void setDuty(float pct) {
  currentDutyPct = constrain(pct, 0.0f, 100.0f);
  onCycles = (int)round((currentDutyPct / 100.0f) * N_HALF);
  onCycles = constrain(onCycles, 0, N_HALF);
}

void halfCycleTick() {
  const unsigned long HALF_MS = 10;  // 10ms per half-cycle at 50Hz
  unsigned long now = millis();

  if (now - lastHalfMs >= HALF_MS) {
    lastHalfMs += HALF_MS;

    if (phaseIndex < onCycles) {
      digitalWrite(SSR_PIN, HIGH);
    } else {
      digitalWrite(SSR_PIN, LOW);
    }

    phaseIndex++;
    if (phaseIndex >= N_HALF) phaseIndex = 0;
  }
}

//////////////////// Safety System ////////////////////
void checkSafety() {
  unsigned long elapsed = millis() - lastResponseMs;

  if (elapsed < GRACE_PERIOD_MS) {
    // NORMAL - server responding, keep current duty
    safeMode = false;
    return;
  }

  if (currentDutyPct > SAFE_MODE_DUTY) {
    // RAMP_DOWN - gradually decrease
    float newDuty = currentDutyPct - RAMP_STEP_PCT;
    if (newDuty < 0) newDuty = 0;
    setDuty(newDuty);
    Serial.printf("RAMP_DOWN: duty=%.1f%% (no response for %lums)\n",
                  currentDutyPct, elapsed);
  } else {
    // SAFE_MODE - SSR OFF, waiting for server
    safeMode = true;
    setDuty(0);
  }
}

//////////////////// Server Communication ////////////////////
void sendReport() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_KEY);
  http.setTimeout(5000);

  unsigned long secsSinceResponse = (millis() - lastResponseMs) / 1000;

  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"power_w\":%.1f,\"uptime_s\":%lu,\"wifi_rssi\":%d,"
    "\"free_heap\":%u,\"current_duty_pct\":%.1f,"
    "\"safe_mode\":%s,\"seconds_since_last_response\":%lu}",
    powerW,
    millis() / 1000,
    WiFi.RSSI(),
    ESP.getFreeHeap(),
    currentDutyPct,
    safeMode ? "true" : "false",
    secsSinceResponse
  );

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    String response = http.getString();
    lastResponseMs = millis();
    safeMode = false;

    // Parse duty_pct from response: {"duty_pct":35.8,"mode":"auto","ack":true}
    int dutyIdx = response.indexOf("\"duty_pct\":");
    if (dutyIdx >= 0) {
      int valueStart = dutyIdx + 11;  // length of "duty_pct":
      float newDuty = response.substring(valueStart).toFloat();
      setDuty(newDuty);
      Serial.printf("Server OK: duty=%.1f%% power=%.1fW\n", newDuty, powerW);
    }
  } else {
    Serial.printf("Server error: HTTP %d\n", httpCode);
  }

  http.end();
}

//////////////////// WiFi Reconnect ////////////////////
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(200);
    halfCycleTick();  // Keep SSR timing alive during reconnect
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi reconnected. IP: ");
    Serial.println(WiFi.localIP());
  }
}

//////////////////// Setup ////////////////////
void setup() {
  Serial.begin(115200);
  delay(200);

  // SSR pin
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);

  // RS485
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);
  RS485.begin(UART_BAUD, SERIAL_MODE, 16, 17);
  RS485.setTimeout(500);
  node.begin(METER_ID, RS485);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns1, dns2);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  Serial.println("\n=== ESP32 HEATER v2 (Dumb Executor) ===");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Server: ");
  Serial.println(SERVER_URL);

  lastResponseMs = millis();  // Start grace period from boot
  lastHalfMs = millis();
  pollMeter();
}

//////////////////// Main Loop ////////////////////
void loop() {
  // SSR timing - must run every 10ms
  halfCycleTick();

  // Read meter every 1s
  if (millis() - lastPollMs >= 1000) {
    lastPollMs = millis();
    pollMeter();
  }

  // Send report to server every 1s (offset from meter read)
  if (millis() - lastSendMs >= 1000) {
    lastSendMs = millis();
    ensureWiFi();
    sendReport();
  }

  // Safety check every 1s
  if (millis() - lastSafetyCheckMs >= 1000) {
    lastSafetyCheckMs = millis();
    checkSafety();
  }
}
