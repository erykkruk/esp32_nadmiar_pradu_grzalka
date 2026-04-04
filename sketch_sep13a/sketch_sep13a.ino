/*
  ESP32 Heater Control v2 - Server-Controlled with Offline Fallback
  (System Kontroli Grzalki ESP32 v2 - Sterowanie Serwerowe z Trybem Offline)

  Architecture:
    ONLINE:  ESP reads meter -> POST to server -> gets duty% -> sets SSR
    OFFLINE: ESP reads meter -> runs local algorithm (same as server) -> sets SSR

  Local algorithm (identical to server):
    - Gross export = measured export + heater power (no feedback loop)
    - Target = avg gross - 100W reserve (proportional)
    - EMA filter (TAU_UP=8s, TAU_DOWN=5s)

  Safety: after 10s without server -> switches to LOCAL mode (not ramp down!)
          Local mode runs the full algorithm autonomously.

  Hardware:
    RS485/Modbus: Serial2 RX=GPIO16, TX=GPIO17, DE/RE=GPIO4, 9600 O-8-1
    SSR Control:  GPIO13 (burst-fire, 50Hz half-cycles)
    Energy Meter: DTSU666, slave ID=1, register 0x2012
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ModbusMaster.h>
#include "config.h"  // <- Skopiuj config.example.h jako config.h i uzupelnij

//////////////////// Configuration from config.h ////////////////////
const char* WIFI_SSID  = CFG_WIFI_SSID;
const char* WIFI_PASS  = CFG_WIFI_PASS;
IPAddress local_IP(CFG_IP_ADDR), gateway(CFG_IP_GATEWAY),
         subnet(CFG_IP_SUBNET), dns1(CFG_IP_DNS1), dns2(CFG_IP_DNS2);
const char* SERVER_URL = CFG_SERVER_URL;
const char* API_KEY    = CFG_API_KEY;

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
const int HALF_CYCLES_PER_SECOND = 100;
const int N_HALF = HALF_CYCLES_PER_SECOND;

//////////////////// Control Algorithm Constants ////////////////////
// Same values as server (constants.ts)
const float EXPORT_RESERVE_W = 100.0f;
const int   GROSS_BUFFER_SIZE = 10;

// 3-priority algorithm constants (same as server)
const float ALPHA_DOWN       = 0.7f;    // Fast filter for decrease
const float ALPHA_UP         = 0.4f;    // Filter for increase
const unsigned long INCREASE_INTERVAL_MS = 2000; // Increase every 2s
const float MAX_STEP_W       = 200.0f;  // Max change per tick [W]

//////////////////// Offline Timeout ////////////////////
const unsigned long OFFLINE_THRESHOLD_MS = 10000;  // 10s without server -> go LOCAL

//////////////////// State Variables ////////////////////

// Operating mode
enum ControlSource { SOURCE_SERVER, SOURCE_LOCAL };
ControlSource controlSource = SOURCE_SERVER;

// Meter reading
float powerW = 0.0f;
unsigned long lastMeterOkMs = 0;

// SSR control
float currentDutyPct = 0.0f;
int   onCycles = 0;
int   phaseIndex = 0;
unsigned long lastHalfMs = 0;

// Server timing
unsigned long lastResponseMs = 0;

// Local algorithm state (mirrors server's store)
float localGrossBuffer[GROSS_BUFFER_SIZE];
int   localBufferIndex = 0;
bool  localBufferFull = false;
float localPApplied = 0.0f;       // Power after filter [W]
unsigned long localLastIncreaseMs = 0; // For increase interval

// Timing
unsigned long lastPollMs = 0;
unsigned long lastSendMs = 0;
unsigned long lastLocalTickMs = 0;

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
  const unsigned long HALF_MS = 10;
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

//////////////////// Local Algorithm (same as server) ////////////////////

float getLocalAverageGross() {
  int count = localBufferFull ? GROSS_BUFFER_SIZE : localBufferIndex;
  if (count == 0) return 0.0f;

  float sum = 0.0f;
  for (int i = 0; i < count; i++) {
    sum += localGrossBuffer[i];
  }
  return sum / count;
}

void runLocalAlgorithm() {
  unsigned long now = millis();
  float prevApplied = localPApplied;

  // Measured export and import
  float measuredExport = (powerW < 0.0f) ? -powerW : 0.0f;
  float gridImport     = (powerW > 0.0f) ?  powerW : 0.0f;

  // Gross export = measured + heater (removes feedback loop)
  float grossExport = measuredExport + localPApplied;

  // Add to circular buffer
  localGrossBuffer[localBufferIndex] = grossExport;
  localBufferIndex = (localBufferIndex + 1) % GROSS_BUFFER_SIZE;
  if (localBufferIndex == 0) localBufferFull = true;

  // === 3-PRIORITY ALGORITHM (same as server & simulation.html) ===

  // PRIORITY 1: Import → immediate cut, no filter
  if (gridImport > 0.0f) {
    float cut = gridImport + EXPORT_RESERVE_W;
    localPApplied = max(0.0f, localPApplied - cut);
    Serial.printf("LOCAL P1-IMPORT: cut=%.0fW applied=%.0fW\n", cut, localPApplied);
  }
  // PRIORITY 2: Export below reserve → fast proportional reduction
  else if (measuredExport < EXPORT_RESERVE_W) {
    float deficit = EXPORT_RESERVE_W - measuredExport;
    float target = max(0.0f, localPApplied - deficit);
    localPApplied = ALPHA_DOWN * target + (1.0f - ALPHA_DOWN) * localPApplied;
    Serial.printf("LOCAL P2-LOW: deficit=%.0fW applied=%.0fW\n", deficit, localPApplied);
  }
  // PRIORITY 3: Surplus → slow increase (only every 5s)
  else {
    if (now - localLastIncreaseMs >= INCREASE_INTERVAL_MS) {
      localLastIncreaseMs = now;
      float avgGross = getLocalAverageGross();
      float target = constrain(avgGross - EXPORT_RESERVE_W, 0.0f, P_MAX);
      if (target > localPApplied) {
        localPApplied = ALPHA_UP * target + (1.0f - ALPHA_UP) * localPApplied;
        Serial.printf("LOCAL P3-UP: avg=%.0fW target=%.0fW applied=%.0fW\n", avgGross, target, localPApplied);
      }
    }
  }

  // Rate limiter: max ±200W per tick
  float delta = localPApplied - prevApplied;
  if (delta >  MAX_STEP_W) localPApplied = prevApplied + MAX_STEP_W;
  if (delta < -MAX_STEP_W) localPApplied = prevApplied - MAX_STEP_W;

  // Clamp
  localPApplied = constrain(localPApplied, 0.0f, P_MAX);

  // Set duty
  float dutyPct = (localPApplied / P_MAX) * 100.0f;
  setDuty(dutyPct);
}

void resetLocalState() {
  // Sync local state with current SSR output so there's no jump
  localPApplied = (currentDutyPct / 100.0f) * P_MAX;
  localLastIncreaseMs = millis();
  // Reset buffer - will fill up with fresh data
  localBufferIndex = 0;
  localBufferFull = false;
  for (int i = 0; i < GROSS_BUFFER_SIZE; i++) {
    localGrossBuffer[i] = 0.0f;
  }
}

//////////////////// Mode Management ////////////////////
void updateControlSource() {
  unsigned long elapsed = millis() - lastResponseMs;
  ControlSource newSource = (elapsed < OFFLINE_THRESHOLD_MS) ? SOURCE_SERVER : SOURCE_LOCAL;

  if (newSource != controlSource) {
    if (newSource == SOURCE_LOCAL) {
      Serial.println(">>> SWITCHING TO LOCAL MODE (server unreachable)");
      resetLocalState();
    } else {
      Serial.println(">>> SWITCHING TO SERVER MODE (server back online)");
    }
    controlSource = newSource;
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
  bool isLocal = (controlSource == SOURCE_LOCAL);

  char payload[300];
  snprintf(payload, sizeof(payload),
    "{\"power_w\":%.1f,\"uptime_s\":%lu,\"wifi_rssi\":%d,"
    "\"free_heap\":%u,\"current_duty_pct\":%.1f,"
    "\"safe_mode\":false,\"offline_mode\":%s,"
    "\"seconds_since_last_response\":%lu}",
    powerW,
    millis() / 1000,
    WiFi.RSSI(),
    ESP.getFreeHeap(),
    currentDutyPct,
    isLocal ? "true" : "false",
    secsSinceResponse
  );

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    String response = http.getString();
    lastResponseMs = millis();

    // Only apply server duty if we're in SERVER mode
    // (updateControlSource will switch us back on next tick)
    int dutyIdx = response.indexOf("\"duty_pct\":");
    if (dutyIdx >= 0) {
      int valueStart = dutyIdx + 11;
      float newDuty = response.substring(valueStart).toFloat();

      if (controlSource == SOURCE_SERVER) {
        setDuty(newDuty);
      }
      // If still LOCAL, we'll switch to SERVER on next updateControlSource()
      // and the next server response will be applied

      Serial.printf("Server OK: duty=%.1f%% power=%.1fW (source=%s)\n",
                    newDuty, powerW, isLocal ? "LOCAL" : "SERVER");
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
    halfCycleTick();
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

  // SSR
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

  // Init local algorithm buffer
  for (int i = 0; i < GROSS_BUFFER_SIZE; i++) {
    localGrossBuffer[i] = 0.0f;
  }

  Serial.println("\n=== ESP32 HEATER v2 (Server + Local Fallback) ===");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Server: ");
  Serial.println(SERVER_URL);
  Serial.println("Offline fallback: ENABLED (same algorithm as server)");

  lastResponseMs = millis();
  lastHalfMs = millis();
  localLastTickMs = millis();
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

  // Check if we should be in SERVER or LOCAL mode
  updateControlSource();

  // Try sending to server every 1s (even in LOCAL mode - to detect when server comes back)
  if (millis() - lastSendMs >= 1000) {
    lastSendMs = millis();
    ensureWiFi();
    sendReport();
  }

  // Run local algorithm every 1s if in LOCAL mode
  if (controlSource == SOURCE_LOCAL) {
    if (millis() - lastLocalTickMs >= 1000) {
      lastLocalTickMs = millis();
      runLocalAlgorithm();
    }
  }
}
