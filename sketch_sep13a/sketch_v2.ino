/*
  ESP32 Heater Control System v2.0 (System Kontroli Grzałki ESP32 v2.0)

  NOWY ALGORYTM:
    - Regulator proporcjonalny zamiast schodkowego
    - Maszyna stanów: STARTUP → NORMAL ⇄ CAUTION → EMERGENCY
    - Analiza trendu z predykcją
    - Szybka reakcja na import (1-3s zamiast 15-25s)
    - Pełna konfiguracja przez WWW
    - 3 preset'y: Oszczędny / Zbalansowany / Agresywny

  Hardware:
    - ESP32 + DTSU666 (RS485/Modbus RTU)
    - SSR burst-fire 50Hz
    - Web Interface z konfiguracją
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>
#include <EEPROM.h>

//==================== CONFIGURATION STRUCTURE (Struktura konfiguracji) ====================
struct Config {
  uint32_t magic;              // Magic number to verify valid config (Liczba magiczna do weryfikacji)

  // Power settings (Ustawienia mocy)
  float P_MAX;                 // Maximum heater power [W] (Maksymalna moc grzałki)

  // Reserve settings (Ustawienia rezerwy)
  float RESERVE_NORMAL;        // Export reserve in NORMAL mode [W] (Rezerwa eksportu w trybie NORMAL)
  float RESERVE_CAUTION;       // Export reserve in CAUTION mode [W] (Rezerwa w trybie CAUTION)

  // State thresholds (Progi stanów)
  float IMPORT_CAUTION;        // Import threshold for CAUTION [W] (Próg importu dla CAUTION)
  float IMPORT_EMERGENCY;      // Import threshold for EMERGENCY [W] (Próg importu dla EMERGENCY)
  float EXPORT_TO_NORMAL;      // Export needed to return to NORMAL [W] (Eksport potrzebny do powrotu do NORMAL)

  // Controller gains (Wzmocnienia regulatora)
  float Kp_STARTUP;            // Gain in STARTUP mode (Wzmocnienie w trybie STARTUP)
  float Kp_NORMAL;             // Gain in NORMAL mode (Wzmocnienie w trybie NORMAL)
  float Kp_CAUTION;            // Gain in CAUTION mode (Wzmocnienie w trybie CAUTION)

  // Filter settings (Ustawienia filtrów)
  float TAU_UP;                // Rise time constant [s] (Stała czasowa narastania)
  float TAU_DOWN;              // Fall time constant [s] (Stała czasowa opadania)
  float MAX_INCREASE_RATE;     // Max power increase rate [W/s] (Max tempo wzrostu mocy)
  float MAX_DECREASE_RATE;     // Max power decrease rate [W/s] (Max tempo spadku mocy)

  // Timing (Czasowe)
  int DECISION_INTERVAL_MS;    // Decision interval [ms] (Interwał decyzji)
  int TREND_WINDOW_S;          // Trend analysis window [s] (Okno analizy trendu)

  uint16_t crc;                // CRC checksum (Suma kontrolna CRC)
};

const uint32_t CONFIG_MAGIC = 0xDEADBEEF;

// Default configuration (Domyślna konfiguracja)
Config config = {
  .magic = CONFIG_MAGIC,
  .P_MAX = 2000.0f,
  .RESERVE_NORMAL = 100.0f,
  .RESERVE_CAUTION = 200.0f,
  .IMPORT_CAUTION = 0.0f,
  .IMPORT_EMERGENCY = 100.0f,
  .EXPORT_TO_NORMAL = 300.0f,
  .Kp_STARTUP = 2.0f,
  .Kp_NORMAL = 0.5f,
  .Kp_CAUTION = 1.0f,
  .TAU_UP = 3.0f,
  .TAU_DOWN = 1.5f,
  .MAX_INCREASE_RATE = 500.0f,
  .MAX_DECREASE_RATE = 1000.0f,
  .DECISION_INTERVAL_MS = 1000,
  .TREND_WINDOW_S = 5,
  .crc = 0
};

//==================== Wi-Fi Configuration (Konfiguracja Wi-Fi) ====================
const char* WIFI_SSID = "TP-Link_E6D1";
const char* WIFI_PASS = "80246459";
IPAddress local_IP(192,168,255,50), gateway(192,168,255,1),
         subnet(255,255,255,0), dns1(192,168,255,1), dns2(8,8,8,8);

//==================== RS485 / Modbus Configuration (Konfiguracja RS485/Modbus) ====================
#define RS485_DE_RE 4
#define UART_BAUD   9600
#define SERIAL_MODE SERIAL_8O1
const uint8_t METER_ID = 1;

HardwareSerial& RS485 = Serial2;
ModbusMaster node;
WebServer server(80);

//==================== State Machine (Maszyna stanów) ====================
enum SystemState {
  STATE_STARTUP,    // Fast ramp-up after boot (Szybki rozruch po starcie)
  STATE_NORMAL,     // Normal operation (Normalna praca)
  STATE_CAUTION,    // Low export warning (Ostrzeżenie o niskim eksporcie)
  STATE_EMERGENCY   // Import detected - shutdown (Wykryto import - wyłączenie)
};

const char* stateNames[] = {"STARTUP", "NORMAL", "CAUTION", "EMERGENCY"};
SystemState currentState = STATE_STARTUP;

//==================== Control Variables (Zmienne sterowania) ====================
const int SSR_PIN = 13;
const int HALF_CYCLES_PER_SECOND = 100;
const int N_HALF = HALF_CYCLES_PER_SECOND;  // 1 second window (Okno 1-sekundowe)

// Power values (Wartości mocy)
float P_target = 0.0f;      // Target power from controller (Moc docelowa z regulatora)
float P_limited = 0.0f;     // After rate limiter (Po limitowniku szybkości)
float P_applied = 0.0f;     // After EMA filter, actually applied (Po filtrze EMA, faktycznie aplikowana)

// Measurements (Pomiary)
float currentPower = 0.0f;  // Current grid power [W]: + import, - export (Aktualna moc sieci: + pobór, - eksport)
float currentExport = 0.0f; // Current export [W] (Aktualny eksport)
float currentImport = 0.0f; // Current import [W] (Aktualny import)

// Trend analysis (Analiza trendu)
const int TREND_BUFFER_SIZE = 10;
float trendBuffer[TREND_BUFFER_SIZE];
int trendIndex = 0;
bool trendBufferFull = false;
float exportTrend = 0.0f;   // W/s change rate (Tempo zmiany W/s)

// Timing (Czasowanie)
unsigned long lastPoll = 0;
unsigned long lastDecision = 0;
unsigned long lastModbusOk = 0;
unsigned long stateEnterTime = 0;
unsigned long lastHalfMillis = 0;

// SSR control (Sterowanie SSR)
int on_cycles = 0;
int phaseIndex = 0;

// Mode (Tryb)
bool autoMode = true;
float manualDuty = 0.0f;

// Error flags (Flagi błędów)
bool modbusError = false;
bool wifiError = false;

// Statistics (Statystyki)
unsigned long totalImportTime = 0;
unsigned long totalExportTime = 0;
float maxImportW = 0.0f;
unsigned long lastStatsReset = 0;

//==================== RS485 Direction Control (Sterowanie kierunkiem RS485) ====================
void preTransmission(){ digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission(){ digitalWrite(RS485_DE_RE, LOW); }

//==================== Modbus Reading (Odczyt Modbus) ====================
bool readFloat32_raw(uint16_t reg, float& outVal) {
  for (int i=0; i<2; i++){
    uint8_t r = node.readHoldingRegisters(reg, 2);
    if (r == node.ku8MBSuccess){
      uint16_t hi = node.getResponseBuffer(0);
      uint16_t lo = node.getResponseBuffer(1);
      uint32_t raw = ((uint32_t)hi << 16) | lo;
      memcpy(&outVal, &raw, sizeof(float));
      return true;
    }
    delay(5);
  }
  return false;
}

bool readFloat32_scaled(uint16_t reg, float scale, float& outVal){
  float v;
  if (readFloat32_raw(reg, v)){
    outVal = v / scale;
    return true;
  }
  return false;
}

//==================== Spike Filter (Filtr skoków) ====================
float lastValidPower = 0.0f;
const float SPIKE_THRESHOLD = 1000.0f;

float filterSpikes(float newValue) {
  if (abs(newValue - lastValidPower) > SPIKE_THRESHOLD && lastValidPower != 0.0f) {
    // Spike detected - use previous value (Wykryto skok - użyj poprzedniej wartości)
    return lastValidPower;
  }
  lastValidPower = newValue;
  return newValue;
}

//==================== Trend Calculation (Obliczanie trendu) ====================
void updateTrendBuffer(float exportValue) {
  trendBuffer[trendIndex] = exportValue;
  trendIndex = (trendIndex + 1) % TREND_BUFFER_SIZE;
  if (trendIndex == 0) trendBufferFull = true;

  // Calculate trend (linear regression approximation) (Oblicz trend - aproksymacja regresji liniowej)
  if (trendBufferFull || trendIndex >= 3) {
    int count = trendBufferFull ? TREND_BUFFER_SIZE : trendIndex;
    int oldest = trendBufferFull ? trendIndex : 0;
    int newest = (trendIndex - 1 + TREND_BUFFER_SIZE) % TREND_BUFFER_SIZE;

    float oldVal = trendBuffer[oldest];
    float newVal = trendBuffer[newest];
    float dt = count * 1.0f;  // seconds (sekundy)

    exportTrend = (newVal - oldVal) / dt;  // W/s
  }
}

//==================== State Machine (Maszyna stanów) ====================
void updateStateMachine() {
  SystemState prevState = currentState;
  unsigned long now = millis();
  unsigned long timeInState = now - stateEnterTime;

  switch (currentState) {
    case STATE_STARTUP:
      // Exit when power stabilizes (Wyjdź gdy moc się ustabilizuje)
      if (abs(P_target - P_applied) < 100.0f && timeInState > 5000) {
        currentState = STATE_NORMAL;
      }
      break;

    case STATE_NORMAL:
      // Check for import or falling trend (Sprawdź import lub spadający trend)
      if (currentImport > config.IMPORT_CAUTION) {
        currentState = STATE_CAUTION;
      } else if (currentExport < config.RESERVE_NORMAL && exportTrend < -50.0f) {
        currentState = STATE_CAUTION;
      }
      break;

    case STATE_CAUTION:
      // Emergency if significant import (Awaryjnie jeśli znaczny import)
      if (currentImport > config.IMPORT_EMERGENCY) {
        currentState = STATE_EMERGENCY;
      }
      // Back to normal if export recovers (Powrót do normalnego jeśli eksport się odbuduje)
      else if (currentExport > config.EXPORT_TO_NORMAL && timeInState > 10000) {
        currentState = STATE_NORMAL;
      }
      break;

    case STATE_EMERGENCY:
      // Exit when export recovers (Wyjdź gdy eksport się odbuduje)
      if (currentExport > config.RESERVE_NORMAL && timeInState > 5000) {
        currentState = STATE_STARTUP;  // Restart through STARTUP (Restart przez STARTUP)
      }
      break;
  }

  if (currentState != prevState) {
    stateEnterTime = now;
    Serial.printf("State change: %s -> %s (Zmiana stanu)\n", stateNames[prevState], stateNames[currentState]);
  }
}

//==================== Controller (Regulator) ====================
float getKp() {
  switch (currentState) {
    case STATE_STARTUP: return config.Kp_STARTUP;
    case STATE_NORMAL: return config.Kp_NORMAL;
    case STATE_CAUTION: return config.Kp_CAUTION;
    case STATE_EMERGENCY: return 10.0f;  // Very fast response (Bardzo szybka reakcja)
    default: return config.Kp_NORMAL;
  }
}

float getReserve() {
  switch (currentState) {
    case STATE_NORMAL: return config.RESERVE_NORMAL;
    case STATE_CAUTION: return config.RESERVE_CAUTION;
    default: return config.RESERVE_NORMAL;
  }
}

void updateController() {
  if (!autoMode) {
    P_target = manualDuty * config.P_MAX;
    return;
  }

  // EMERGENCY - immediate shutdown (AWARYJNE - natychmiastowe wyłączenie)
  if (currentState == STATE_EMERGENCY) {
    P_target = 0.0f;
    return;
  }

  float reserve = getReserve();
  float Kp = getKp();

  // Proportional control (Sterowanie proporcjonalne)
  float error = currentExport - reserve;

  // Trend prediction (Predykcja trendu)
  if (exportTrend < -50.0f && currentExport < reserve * 2) {
    // Falling fast - preemptive reduction (Szybki spadek - prewencyjna redukcja)
    error -= abs(exportTrend) * config.TREND_WINDOW_S;
  }

  // Adaptive Kp based on error magnitude (Adaptacyjne Kp na podstawie wielkości błędu)
  if (abs(error) > 500) Kp *= 1.5f;
  else if (abs(error) > 200) Kp *= 1.2f;

  P_target = P_applied + Kp * error;
  P_target = constrain(P_target, 0.0f, config.P_MAX);
}

//==================== Rate Limiter (Limiter szybkości) ====================
void applyRateLimiter(float dt) {
  float deltaP = P_target - P_limited;

  if (currentState == STATE_EMERGENCY) {
    // No rate limiting in emergency (Brak ograniczeń w trybie awaryjnym)
    P_limited = P_target;
    return;
  }

  float maxIncrease = config.MAX_INCREASE_RATE * dt;
  float maxDecrease = config.MAX_DECREASE_RATE * dt;

  if (deltaP > 0) {
    deltaP = min(deltaP, maxIncrease);
  } else {
    deltaP = max(deltaP, -maxDecrease);
  }

  P_limited += deltaP;
  P_limited = constrain(P_limited, 0.0f, config.P_MAX);
}

//==================== EMA Filter (Filtr EMA) ====================
void applyEMAFilter(float dt) {
  if (currentState == STATE_EMERGENCY) {
    // No filtering in emergency (Brak filtrowania w trybie awaryjnym)
    P_applied = P_limited;
    return;
  }

  float tau = (P_limited > P_applied) ? config.TAU_UP : config.TAU_DOWN;
  float alpha = dt / (tau + dt);

  P_applied += alpha * (P_limited - P_applied);
  P_applied = constrain(P_applied, 0.0f, config.P_MAX);
}

//==================== SSR Control (Sterowanie SSR) ====================
void updateSSRControl() {
  float duty = P_applied / config.P_MAX;
  on_cycles = (int)round(duty * N_HALF);
  on_cycles = constrain(on_cycles, 0, N_HALF);
}

void halfCycleTick() {
  const unsigned long HALF_MS = 10;
  unsigned long now = millis();

  if (now - lastHalfMillis >= HALF_MS) {
    lastHalfMillis += HALF_MS;

    if (phaseIndex < on_cycles) {
      digitalWrite(SSR_PIN, HIGH);
    } else {
      digitalWrite(SSR_PIN, LOW);
    }

    phaseIndex++;
    if (phaseIndex >= N_HALF) {
      phaseIndex = 0;
      updateSSRControl();
    }
  }
}

//==================== Poll Meter (Odpytywanie licznika) ====================
void pollMeter() {
  float v;
  if (readFloat32_scaled(0x2012, 10.0f, v)) {
    v = filterSpikes(v);
    currentPower = v;
    currentExport = (v < 0.0f) ? -v : 0.0f;
    currentImport = (v > 0.0f) ? v : 0.0f;

    lastModbusOk = millis();
    modbusError = false;

    updateTrendBuffer(currentExport);

    // Statistics (Statystyki)
    if (currentImport > 0) {
      totalImportTime++;
      if (currentImport > maxImportW) maxImportW = currentImport;
    } else {
      totalExportTime++;
    }
  } else {
    if (millis() - lastModbusOk > 30000) {
      modbusError = true;
      currentState = STATE_EMERGENCY;
    }
  }
}

//==================== EEPROM Config (Konfiguracja EEPROM) ====================
uint16_t calculateCRC(const Config& cfg) {
  uint16_t crc = 0xFFFF;
  const uint8_t* data = (const uint8_t*)&cfg;
  size_t len = sizeof(Config) - sizeof(uint16_t);  // Exclude CRC field (Wyklucz pole CRC)

  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

void saveConfig() {
  config.crc = calculateCRC(config);
  EEPROM.put(0, config);
  EEPROM.commit();
  Serial.println("Config saved to EEPROM (Konfiguracja zapisana do EEPROM)");
}

void loadConfig() {
  Config loaded;
  EEPROM.get(0, loaded);

  if (loaded.magic == CONFIG_MAGIC && calculateCRC(loaded) == loaded.crc) {
    config = loaded;
    Serial.println("Config loaded from EEPROM (Konfiguracja wczytana z EEPROM)");
  } else {
    Serial.println("Invalid config, using defaults (Nieprawidłowa konfiguracja, używam domyślnych)");
    saveConfig();
  }
}

//==================== Presets (Preset'y) ====================
void applyPreset(int preset) {
  switch (preset) {
    case 0:  // Savings (Oszczędny)
      config.RESERVE_NORMAL = 150.0f;
      config.RESERVE_CAUTION = 300.0f;
      config.IMPORT_EMERGENCY = 50.0f;
      config.Kp_NORMAL = 0.3f;
      config.MAX_DECREASE_RATE = 2000.0f;
      break;
    case 1:  // Balanced (Zbalansowany)
      config.RESERVE_NORMAL = 100.0f;
      config.RESERVE_CAUTION = 200.0f;
      config.IMPORT_EMERGENCY = 100.0f;
      config.Kp_NORMAL = 0.5f;
      config.MAX_DECREASE_RATE = 1000.0f;
      break;
    case 2:  // Greedy (Agresywny)
      config.RESERVE_NORMAL = 50.0f;
      config.RESERVE_CAUTION = 100.0f;
      config.IMPORT_EMERGENCY = 200.0f;
      config.Kp_NORMAL = 0.8f;
      config.MAX_INCREASE_RATE = 1000.0f;
      break;
  }
  saveConfig();
}

//==================== Web Interface (Interfejs WWW) ====================
String htmlPage() {
  String s = R"HTML(
<!doctype html><html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sterowanie Grzałką v2.0</title>
<style>
:root{--ok:#10b981;--warn:#f59e0b;--bad:#ef4444;--bg:#f8fafc;--card:#fff;--border:#e2e8f0;--text:#1e293b}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);padding:16px;max-width:1200px;margin:0 auto}
h1{font-size:1.5em;margin-bottom:16px;display:flex;align-items:center;gap:8px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:12px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px}
.card h2{font-size:1em;color:#64748b;margin-bottom:12px;border-bottom:1px solid var(--border);padding-bottom:8px}
.stat{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #f1f5f9}
.stat:last-child{border:none}
.stat-value{font-size:1.2em;font-weight:600;font-variant-numeric:tabular-nums}
.state{display:inline-block;padding:4px 12px;border-radius:20px;font-size:0.85em;font-weight:600}
.state-STARTUP{background:#dbeafe;color:#1d4ed8}
.state-NORMAL{background:#d1fae5;color:#059669}
.state-CAUTION{background:#fef3c7;color:#d97706}
.state-EMERGENCY{background:#fee2e2;color:#dc2626}
.control-row{display:flex;gap:12px;align-items:center;margin-bottom:12px;flex-wrap:wrap}
.btn{padding:8px 16px;border:none;border-radius:8px;cursor:pointer;font-weight:500;transition:all 0.2s}
.btn-primary{background:#3b82f6;color:white}
.btn-primary:hover{background:#2563eb}
.btn-secondary{background:#e2e8f0;color:#475569}
.btn-secondary:hover{background:#cbd5e1}
.btn-danger{background:#ef4444;color:white}
.btn-danger:hover{background:#dc2626}
.btn-preset{background:#f1f5f9;border:2px solid transparent}
.btn-preset.active{border-color:#3b82f6;background:#eff6ff}
input[type="range"]{flex:1;min-width:150px}
input[type="number"]{width:100px;padding:6px;border:1px solid var(--border);border-radius:6px}
.config-grid{display:grid;grid-template-columns:1fr 120px;gap:8px;align-items:center}
.config-grid label{font-size:0.9em;color:#64748b}
.chart-container{height:180px;position:relative;margin-top:12px}
canvas{width:100%;height:100%}
.status-bar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px}
.status-badge{padding:4px 10px;border-radius:16px;font-size:0.8em;display:flex;align-items:center;gap:4px}
.status-ok{background:#d1fae5;color:#059669}
.status-warn{background:#fef3c7;color:#d97706}
.status-error{background:#fee2e2;color:#dc2626}
.tabs{display:flex;gap:4px;margin-bottom:16px;border-bottom:2px solid var(--border);padding-bottom:4px}
.tab{padding:8px 16px;cursor:pointer;border-radius:8px 8px 0 0;transition:all 0.2s}
.tab:hover{background:#f1f5f9}
.tab.active{background:#3b82f6;color:white}
.tab-content{display:none}
.tab-content.active{display:block}
.trend-up{color:#10b981}
.trend-down{color:#ef4444}
.trend-stable{color:#64748b}
</style>
</head><body>

<h1>
  <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="#3b82f6" stroke-width="2">
    <path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83"/>
    <circle cx="12" cy="12" r="5"/>
  </svg>
  Sterowanie Grzałką v2.0
</h1>

<div class="status-bar">
  <span class="status-badge" id="statusWifi">WiFi: --</span>
  <span class="status-badge" id="statusModbus">Modbus: --</span>
  <span class="status-badge" id="statusState">Stan: --</span>
</div>

<div class="tabs">
  <div class="tab active" onclick="showTab('monitor')">Monitor</div>
  <div class="tab" onclick="showTab('control')">Sterowanie</div>
  <div class="tab" onclick="showTab('config')">Konfiguracja</div>
  <div class="tab" onclick="showTab('stats')">Statystyki</div>
</div>

<!-- MONITOR TAB -->
<div id="tab-monitor" class="tab-content active">
  <div class="grid">
    <div class="card">
      <h2>Pomiary w czasie rzeczywistym</h2>
      <div class="stat"><span>Moc sieci</span><span class="stat-value" id="gridPower">-- W</span></div>
      <div class="stat"><span>Eksport</span><span class="stat-value" id="exportW">-- W</span></div>
      <div class="stat"><span>Import</span><span class="stat-value" id="importW">-- W</span></div>
      <div class="stat"><span>Trend eksportu</span><span class="stat-value" id="trend">-- W/s</span></div>
    </div>
    <div class="card">
      <h2>Sterowanie grzałką</h2>
      <div class="stat"><span>Moc docelowa</span><span class="stat-value" id="targetW">-- W</span></div>
      <div class="stat"><span>Moc aplikowana</span><span class="stat-value" id="appliedW">-- W</span></div>
      <div class="stat"><span>Wypełnienie SSR</span><span class="stat-value" id="duty">-- %</span></div>
      <div class="stat"><span>Aktualne Kp</span><span class="stat-value" id="kp">--</span></div>
    </div>
  </div>

  <div class="grid">
    <div class="card">
      <h2>Wykres: Moc grzałki (60s)</h2>
      <div class="chart-container"><canvas id="chartHeater" width="600" height="180"></canvas></div>
    </div>
    <div class="card">
      <h2>Wykres: Eksport/Import (60s)</h2>
      <div class="chart-container"><canvas id="chartGrid" width="600" height="180"></canvas></div>
    </div>
  </div>
</div>

<!-- CONTROL TAB -->
<div id="tab-control" class="tab-content">
  <div class="card">
    <h2>Tryb pracy</h2>
    <div class="control-row">
      <button class="btn btn-primary" id="btnAuto" onclick="setMode('auto')">AUTO</button>
      <button class="btn btn-secondary" id="btnManual" onclick="setMode('manual')">MANUAL</button>
      <button class="btn btn-danger" onclick="setMode('emergency')">STOP AWARYJNY</button>
    </div>
    <div class="control-row" id="manualControl" style="display:none">
      <span>Moc ręczna:</span>
      <input type="range" id="manualSlider" min="0" max="100" value="0" oninput="updateManualLabel()">
      <span id="manualLabel">0%</span>
      <button class="btn btn-secondary" onclick="applyManual()">Zastosuj</button>
    </div>
  </div>

  <div class="card">
    <h2>Preset'y</h2>
    <div class="control-row">
      <button class="btn btn-preset" onclick="applyPreset(0)">
        <b>Oszczędny</b><br>
        <small>Priorytet: zero importu</small>
      </button>
      <button class="btn btn-preset active" onclick="applyPreset(1)">
        <b>Zbalansowany</b><br>
        <small>Kompromis</small>
      </button>
      <button class="btn btn-preset" onclick="applyPreset(2)">
        <b>Agresywny</b><br>
        <small>Max zużycie nadwyżki</small>
      </button>
    </div>
  </div>
</div>

<!-- CONFIG TAB -->
<div id="tab-config" class="tab-content">
  <div class="grid">
    <div class="card">
      <h2>Moc</h2>
      <div class="config-grid">
        <label>Max moc grzałki [W]</label>
        <input type="number" id="cfg_P_MAX" value="2000">
      </div>
    </div>
    <div class="card">
      <h2>Rezerwy eksportu</h2>
      <div class="config-grid">
        <label>Rezerwa NORMAL [W]</label>
        <input type="number" id="cfg_RESERVE_NORMAL" value="100">
        <label>Rezerwa CAUTION [W]</label>
        <input type="number" id="cfg_RESERVE_CAUTION" value="200">
      </div>
    </div>
    <div class="card">
      <h2>Progi stanów</h2>
      <div class="config-grid">
        <label>Import → CAUTION [W]</label>
        <input type="number" id="cfg_IMPORT_CAUTION" value="0">
        <label>Import → EMERGENCY [W]</label>
        <input type="number" id="cfg_IMPORT_EMERGENCY" value="100">
        <label>Eksport → NORMAL [W]</label>
        <input type="number" id="cfg_EXPORT_TO_NORMAL" value="300">
      </div>
    </div>
    <div class="card">
      <h2>Regulator</h2>
      <div class="config-grid">
        <label>Kp STARTUP</label>
        <input type="number" id="cfg_Kp_STARTUP" value="2.0" step="0.1">
        <label>Kp NORMAL</label>
        <input type="number" id="cfg_Kp_NORMAL" value="0.5" step="0.1">
        <label>Kp CAUTION</label>
        <input type="number" id="cfg_Kp_CAUTION" value="1.0" step="0.1">
      </div>
    </div>
    <div class="card">
      <h2>Filtry</h2>
      <div class="config-grid">
        <label>TAU narastania [s]</label>
        <input type="number" id="cfg_TAU_UP" value="3.0" step="0.5">
        <label>TAU opadania [s]</label>
        <input type="number" id="cfg_TAU_DOWN" value="1.5" step="0.5">
        <label>Max wzrost [W/s]</label>
        <input type="number" id="cfg_MAX_INCREASE" value="500">
        <label>Max spadek [W/s]</label>
        <input type="number" id="cfg_MAX_DECREASE" value="1000">
      </div>
    </div>
  </div>
  <div class="control-row" style="margin-top:16px">
    <button class="btn btn-primary" onclick="saveConfig()">Zapisz konfigurację</button>
    <button class="btn btn-secondary" onclick="loadConfig()">Wczytaj z urządzenia</button>
    <button class="btn btn-danger" onclick="resetConfig()">Przywróć domyślne</button>
  </div>
</div>

<!-- STATS TAB -->
<div id="tab-stats" class="tab-content">
  <div class="grid">
    <div class="card">
      <h2>Statystyki sesji</h2>
      <div class="stat"><span>Czas importu</span><span class="stat-value" id="statImportTime">-- s</span></div>
      <div class="stat"><span>Czas eksportu</span><span class="stat-value" id="statExportTime">-- s</span></div>
      <div class="stat"><span>Max import</span><span class="stat-value" id="statMaxImport">-- W</span></div>
      <div class="stat"><span>Czas od resetu</span><span class="stat-value" id="statUptime">--</span></div>
    </div>
    <div class="card">
      <h2>Efektywność</h2>
      <div class="stat"><span>% czasu bez importu</span><span class="stat-value" id="statEfficiency">-- %</span></div>
    </div>
  </div>
  <button class="btn btn-secondary" onclick="resetStats()">Resetuj statystyki</button>
</div>

<script>
let state = {auto:true, state:'STARTUP', P_target:0, P_applied:0, export_W:0, import_W:0, trend:0};
let histHeater = new Array(60).fill(0);
let histExport = new Array(60).fill(0);
let histImport = new Array(60).fill(0);

function showTab(name) {
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
  event.target.classList.add('active');
  document.getElementById('tab-'+name).classList.add('active');
}

function drawChart(canvas, datasets, maxY) {
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0,0,W,H);

  // Grid
  ctx.strokeStyle='#e2e8f0'; ctx.lineWidth=1;
  for(let i=0;i<=4;i++){
    const y=i*(H/4);
    ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(W,y); ctx.stroke();
  }

  // Zero line if needed
  if (maxY > 0) {
    const zeroY = H/2;
    ctx.strokeStyle='#94a3b8';
    ctx.setLineDash([5,5]);
    ctx.beginPath(); ctx.moveTo(0,zeroY); ctx.lineTo(W,zeroY); ctx.stroke();
    ctx.setLineDash([]);
  }

  // Data
  datasets.forEach(({data, color}) => {
    if(data.length<2) return;
    ctx.strokeStyle=color; ctx.lineWidth=2; ctx.beginPath();
    const n=data.length;
    for(let i=0;i<n;i++){
      const x = (i/(60-1))*W;
      const y = H/2 - (data[i]/maxY)*(H/2);
      if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
    }
    ctx.stroke();
  });
}

async function fetchData() {
  try {
    const [d, c, s] = await Promise.all([
      fetch('/data.json').then(r=>r.json()),
      fetch('/ctrl.json').then(r=>r.json()),
      fetch('/stats.json').then(r=>r.json())
    ]);

    state = {...state, ...d, ...c};

    // Update UI
    document.getElementById('gridPower').textContent = d.grid_power.toFixed(0) + ' W';
    document.getElementById('exportW').textContent = d.export_W.toFixed(0) + ' W';
    document.getElementById('importW').textContent = d.import_W.toFixed(0) + ' W';

    const trendClass = d.trend > 10 ? 'trend-up' : (d.trend < -10 ? 'trend-down' : 'trend-stable');
    document.getElementById('trend').className = 'stat-value ' + trendClass;
    document.getElementById('trend').textContent = (d.trend > 0 ? '+' : '') + d.trend.toFixed(1) + ' W/s';

    document.getElementById('targetW').textContent = c.P_target.toFixed(0) + ' W';
    document.getElementById('appliedW').textContent = c.P_applied.toFixed(0) + ' W';
    document.getElementById('duty').textContent = ((c.P_applied / c.P_MAX) * 100).toFixed(0) + ' %';
    document.getElementById('kp').textContent = c.Kp.toFixed(2);

    // Status badges
    document.getElementById('statusWifi').className = 'status-badge ' + (c.wifi_ok ? 'status-ok' : 'status-error');
    document.getElementById('statusWifi').textContent = 'WiFi: ' + (c.wifi_ok ? 'OK' : 'Błąd');

    document.getElementById('statusModbus').className = 'status-badge ' + (c.modbus_ok ? 'status-ok' : 'status-error');
    document.getElementById('statusModbus').textContent = 'Modbus: ' + (c.modbus_ok ? 'OK' : 'Błąd');

    const stateColors = {STARTUP:'status-ok', NORMAL:'status-ok', CAUTION:'status-warn', EMERGENCY:'status-error'};
    document.getElementById('statusState').className = 'status-badge ' + stateColors[c.state];
    document.getElementById('statusState').textContent = 'Stan: ' + c.state;

    // Mode buttons
    document.getElementById('btnAuto').classList.toggle('btn-primary', c.auto);
    document.getElementById('btnAuto').classList.toggle('btn-secondary', !c.auto);
    document.getElementById('btnManual').classList.toggle('btn-primary', !c.auto);
    document.getElementById('btnManual').classList.toggle('btn-secondary', c.auto);
    document.getElementById('manualControl').style.display = c.auto ? 'none' : 'flex';

    // Stats
    document.getElementById('statImportTime').textContent = s.import_time + ' s';
    document.getElementById('statExportTime').textContent = s.export_time + ' s';
    document.getElementById('statMaxImport').textContent = s.max_import.toFixed(0) + ' W';
    document.getElementById('statUptime').textContent = formatTime(s.uptime);
    const total = s.import_time + s.export_time;
    const eff = total > 0 ? ((s.export_time / total) * 100).toFixed(1) : '100.0';
    document.getElementById('statEfficiency').textContent = eff + ' %';

    // Charts
    histHeater.push(c.P_applied); if(histHeater.length>60) histHeater.shift();
    histExport.push(d.export_W); if(histExport.length>60) histExport.shift();
    histImport.push(-d.import_W); if(histImport.length>60) histImport.shift();

    drawChart(document.getElementById('chartHeater'), [{data:histHeater, color:'#10b981'}], 2000);
    drawChart(document.getElementById('chartGrid'), [
      {data:histExport, color:'#3b82f6'},
      {data:histImport, color:'#ef4444'}
    ], 500);

  } catch(e) { console.error(e); }
}

function formatTime(seconds) {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  return `${h}h ${m}m ${s}s`;
}

async function setMode(mode) {
  await fetch('/set?mode=' + mode);
  fetchData();
}

function updateManualLabel() {
  document.getElementById('manualLabel').textContent = document.getElementById('manualSlider').value + '%';
}

async function applyManual() {
  const duty = document.getElementById('manualSlider').value;
  await fetch('/set?duty=' + duty);
  fetchData();
}

async function applyPreset(n) {
  await fetch('/set?preset=' + n);
  loadConfig();
  document.querySelectorAll('.btn-preset').forEach((b,i) => b.classList.toggle('active', i===n));
}

async function saveConfig() {
  const params = new URLSearchParams();
  params.set('P_MAX', document.getElementById('cfg_P_MAX').value);
  params.set('RESERVE_NORMAL', document.getElementById('cfg_RESERVE_NORMAL').value);
  params.set('RESERVE_CAUTION', document.getElementById('cfg_RESERVE_CAUTION').value);
  params.set('IMPORT_CAUTION', document.getElementById('cfg_IMPORT_CAUTION').value);
  params.set('IMPORT_EMERGENCY', document.getElementById('cfg_IMPORT_EMERGENCY').value);
  params.set('EXPORT_TO_NORMAL', document.getElementById('cfg_EXPORT_TO_NORMAL').value);
  params.set('Kp_STARTUP', document.getElementById('cfg_Kp_STARTUP').value);
  params.set('Kp_NORMAL', document.getElementById('cfg_Kp_NORMAL').value);
  params.set('Kp_CAUTION', document.getElementById('cfg_Kp_CAUTION').value);
  params.set('TAU_UP', document.getElementById('cfg_TAU_UP').value);
  params.set('TAU_DOWN', document.getElementById('cfg_TAU_DOWN').value);
  params.set('MAX_INCREASE', document.getElementById('cfg_MAX_INCREASE').value);
  params.set('MAX_DECREASE', document.getElementById('cfg_MAX_DECREASE').value);

  await fetch('/config?' + params.toString());
  alert('Konfiguracja zapisana!');
}

async function loadConfig() {
  const r = await fetch('/config.json');
  const c = await r.json();

  document.getElementById('cfg_P_MAX').value = c.P_MAX;
  document.getElementById('cfg_RESERVE_NORMAL').value = c.RESERVE_NORMAL;
  document.getElementById('cfg_RESERVE_CAUTION').value = c.RESERVE_CAUTION;
  document.getElementById('cfg_IMPORT_CAUTION').value = c.IMPORT_CAUTION;
  document.getElementById('cfg_IMPORT_EMERGENCY').value = c.IMPORT_EMERGENCY;
  document.getElementById('cfg_EXPORT_TO_NORMAL').value = c.EXPORT_TO_NORMAL;
  document.getElementById('cfg_Kp_STARTUP').value = c.Kp_STARTUP;
  document.getElementById('cfg_Kp_NORMAL').value = c.Kp_NORMAL;
  document.getElementById('cfg_Kp_CAUTION').value = c.Kp_CAUTION;
  document.getElementById('cfg_TAU_UP').value = c.TAU_UP;
  document.getElementById('cfg_TAU_DOWN').value = c.TAU_DOWN;
  document.getElementById('cfg_MAX_INCREASE').value = c.MAX_INCREASE_RATE;
  document.getElementById('cfg_MAX_DECREASE').value = c.MAX_DECREASE_RATE;
}

async function resetConfig() {
  if(confirm('Przywrócić domyślne ustawienia?')) {
    await fetch('/reset_config');
    loadConfig();
    alert('Przywrócono domyślne!');
  }
}

async function resetStats() {
  await fetch('/reset_stats');
  fetchData();
}

// Init
fetchData();
loadConfig();
setInterval(fetchData, 1000);
</script>
</body></html>
)HTML";
  return s;
}

//==================== HTTP Handlers (Obsługa HTTP) ====================
void handleRoot() { server.send(200, "text/html; charset=utf-8", htmlPage()); }

void handleData() {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"grid_power\":%.1f,\"export_W\":%.1f,\"import_W\":%.1f,\"trend\":%.2f}",
    currentPower, currentExport, currentImport, exportTrend
  );
  server.send(200, "application/json", buf);
}

void handleCtrl() {
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"auto\":%s,\"state\":\"%s\",\"P_target\":%.1f,\"P_applied\":%.1f,"
    "\"P_MAX\":%.1f,\"Kp\":%.2f,\"modbus_ok\":%s,\"wifi_ok\":%s}",
    autoMode ? "true" : "false",
    stateNames[currentState],
    P_target, P_applied,
    config.P_MAX,
    getKp(),
    modbusError ? "false" : "true",
    WiFi.status() == WL_CONNECTED ? "true" : "false"
  );
  server.send(200, "application/json", buf);
}

void handleStats() {
  unsigned long uptime = (millis() - lastStatsReset) / 1000;
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"import_time\":%lu,\"export_time\":%lu,\"max_import\":%.1f,\"uptime\":%lu}",
    totalImportTime, totalExportTime, maxImportW, uptime
  );
  server.send(200, "application/json", buf);
}

void handleConfigJSON() {
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"P_MAX\":%.1f,\"RESERVE_NORMAL\":%.1f,\"RESERVE_CAUTION\":%.1f,"
    "\"IMPORT_CAUTION\":%.1f,\"IMPORT_EMERGENCY\":%.1f,\"EXPORT_TO_NORMAL\":%.1f,"
    "\"Kp_STARTUP\":%.2f,\"Kp_NORMAL\":%.2f,\"Kp_CAUTION\":%.2f,"
    "\"TAU_UP\":%.1f,\"TAU_DOWN\":%.1f,\"MAX_INCREASE_RATE\":%.1f,\"MAX_DECREASE_RATE\":%.1f}",
    config.P_MAX, config.RESERVE_NORMAL, config.RESERVE_CAUTION,
    config.IMPORT_CAUTION, config.IMPORT_EMERGENCY, config.EXPORT_TO_NORMAL,
    config.Kp_STARTUP, config.Kp_NORMAL, config.Kp_CAUTION,
    config.TAU_UP, config.TAU_DOWN, config.MAX_INCREASE_RATE, config.MAX_DECREASE_RATE
  );
  server.send(200, "application/json", buf);
}

void handleSet() {
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    if (m == "auto") { autoMode = true; currentState = STATE_STARTUP; }
    else if (m == "manual") { autoMode = false; }
    else if (m == "emergency") { currentState = STATE_EMERGENCY; P_target = 0; }
  }
  if (server.hasArg("duty")) {
    int pct = server.arg("duty").toInt();
    manualDuty = constrain(pct, 0, 100) / 100.0f;
  }
  if (server.hasArg("preset")) {
    applyPreset(server.arg("preset").toInt());
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleConfig() {
  if (server.hasArg("P_MAX")) config.P_MAX = server.arg("P_MAX").toFloat();
  if (server.hasArg("RESERVE_NORMAL")) config.RESERVE_NORMAL = server.arg("RESERVE_NORMAL").toFloat();
  if (server.hasArg("RESERVE_CAUTION")) config.RESERVE_CAUTION = server.arg("RESERVE_CAUTION").toFloat();
  if (server.hasArg("IMPORT_CAUTION")) config.IMPORT_CAUTION = server.arg("IMPORT_CAUTION").toFloat();
  if (server.hasArg("IMPORT_EMERGENCY")) config.IMPORT_EMERGENCY = server.arg("IMPORT_EMERGENCY").toFloat();
  if (server.hasArg("EXPORT_TO_NORMAL")) config.EXPORT_TO_NORMAL = server.arg("EXPORT_TO_NORMAL").toFloat();
  if (server.hasArg("Kp_STARTUP")) config.Kp_STARTUP = server.arg("Kp_STARTUP").toFloat();
  if (server.hasArg("Kp_NORMAL")) config.Kp_NORMAL = server.arg("Kp_NORMAL").toFloat();
  if (server.hasArg("Kp_CAUTION")) config.Kp_CAUTION = server.arg("Kp_CAUTION").toFloat();
  if (server.hasArg("TAU_UP")) config.TAU_UP = server.arg("TAU_UP").toFloat();
  if (server.hasArg("TAU_DOWN")) config.TAU_DOWN = server.arg("TAU_DOWN").toFloat();
  if (server.hasArg("MAX_INCREASE")) config.MAX_INCREASE_RATE = server.arg("MAX_INCREASE").toFloat();
  if (server.hasArg("MAX_DECREASE")) config.MAX_DECREASE_RATE = server.arg("MAX_DECREASE").toFloat();

  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleResetConfig() {
  config = {
    .magic = CONFIG_MAGIC,
    .P_MAX = 2000.0f,
    .RESERVE_NORMAL = 100.0f,
    .RESERVE_CAUTION = 200.0f,
    .IMPORT_CAUTION = 0.0f,
    .IMPORT_EMERGENCY = 100.0f,
    .EXPORT_TO_NORMAL = 300.0f,
    .Kp_STARTUP = 2.0f,
    .Kp_NORMAL = 0.5f,
    .Kp_CAUTION = 1.0f,
    .TAU_UP = 3.0f,
    .TAU_DOWN = 1.5f,
    .MAX_INCREASE_RATE = 500.0f,
    .MAX_DECREASE_RATE = 1000.0f,
    .DECISION_INTERVAL_MS = 1000,
    .TREND_WINDOW_S = 5,
    .crc = 0
  };
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleResetStats() {
  totalImportTime = 0;
  totalExportTime = 0;
  maxImportW = 0;
  lastStatsReset = millis();
  server.send(200, "application/json", "{\"ok\":true}");
}

//==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(200);

  // EEPROM
  EEPROM.begin(512);
  loadConfig();

  // RS485
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);
  RS485.begin(UART_BAUD, SERIAL_MODE, 16, 17);
  RS485.setTimeout(500);
  node.begin(METER_ID, RS485);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  // SSR
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns1, dns2);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  // Web server
  server.on("/", handleRoot);
  server.on("/data.json", handleData);
  server.on("/ctrl.json", handleCtrl);
  server.on("/stats.json", handleStats);
  server.on("/config.json", handleConfigJSON);
  server.on("/set", handleSet);
  server.on("/config", handleConfig);
  server.on("/reset_config", handleResetConfig);
  server.on("/reset_stats", handleResetStats);
  server.begin();

  // Initialize
  lastModbusOk = millis();
  stateEnterTime = millis();
  lastStatsReset = millis();
  lastHalfMillis = millis();

  // Initialize trend buffer
  for (int i = 0; i < TREND_BUFFER_SIZE; i++) trendBuffer[i] = 0;

  Serial.println("\n=== ESP32 HEATER CONTROL v2.0 ===");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.println("New algorithm: Proportional + State Machine + Trend Prediction");
}

//==================== Main Loop ====================
void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Poll meter every 1 second (Odpytuj licznik co 1 sekundę)
  if (now - lastPoll >= 1000) {
    lastPoll = now;
    pollMeter();
  }

  // Decision every DECISION_INTERVAL_MS (Decyzja co DECISION_INTERVAL_MS)
  if (now - lastDecision >= config.DECISION_INTERVAL_MS) {
    float dt = (now - lastDecision) / 1000.0f;
    lastDecision = now;

    updateStateMachine();
    updateController();
    applyRateLimiter(dt);
    applyEMAFilter(dt);
  }

  // SSR burst-fire control (Sterowanie SSR burst-fire)
  halfCycleTick();
}
