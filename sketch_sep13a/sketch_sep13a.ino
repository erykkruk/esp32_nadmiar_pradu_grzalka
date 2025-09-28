/*
  ESP32 + DTSU666 (RS485/Modbus RTU) -> mini WWW + automatyczne sterowanie grzałką
  UPROSZCZONE: tylko pobór z sieci + moc grzałki

  - WiFi: TP-Link_E6D1 / 80246459
  - Statyczny IP: 192.168.255.50
  - RS485 (DTSU666): Serial2  (RX=16, TX=17), DE/RE=GPIO4
  - Modbus: 9600 bps, O-8-1, slave id = 1
  - Biblioteka: ModbusMaster (Doc Walker)

  Rejestry:
  - 0x2012: Total active power (float32 w 0.1 W) -> /10 => W
    Konwencja: +W = pobór z sieci, -W = eksport do sieci

  Sterowanie:
  - SSR IN+ = GPIO13, IN- = GND (zalecany driver tranzystorowy)
  - Burst-fire: okno 1 s, 100 półokresów (50 Hz)
  - EMA asymetryczne: τ_up=5 s (łagodny wzrost), τ_down=1 s (szybki spadek przy imporcie)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>

//////////////////// KONFIG Wi-Fi ////////////////////
const char* WIFI_SSID = "TP-Link_E6D1";
const char* WIFI_PASS = "80246459";
IPAddress local_IP(192,168,255,50), gateway(192,168,255,1),
         subnet(255,255,255,0), dns1(192,168,255,1), dns2(8,8,8,8);

//////////////////// RS485 / Modbus //////////////////
#define RS485_DE_RE 4
#define UART_BAUD   9600
#define SERIAL_MODE SERIAL_8O1
const uint8_t  METER_ID = 1;

HardwareSerial& RS485 = Serial2;
ModbusMaster node;
WebServer server(80);

void preTransmission(){ digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission(){ digitalWrite(RS485_DE_RE, LOW);  }

//////////////////// POMOCNICZE //////////////////////
bool readFloat32_raw(uint16_t reg, float& outVal) {
  // DTSU666: float32, hi-word first; skala 0.1 W dla mocy
  for (int i=0;i<2;i++){
    uint8_t r = node.readHoldingRegisters(reg, 2);
    if (r == node.ku8MBSuccess){
      uint16_t hi = node.getResponseBuffer(0);
      uint16_t lo = node.getResponseBuffer(1);
      uint32_t raw = ((uint32_t)hi << 16) | lo; // w razie dziwnych wartości spróbuj (lo<<16)|hi
      memcpy(&outVal, &raw, sizeof(float));
      return true;
    }
    delay(5);
  }
  return false;
}
bool readFloat32_scaled(uint16_t reg, float scale, float& outVal){
  float v;
  if (readFloat32_raw(reg, v)){ outVal = v / scale; return true; }
  return false;
}

//////////////////// DANE ////////////////////
struct {
  float Pt = 0.0f;          // W, dodatni=pobór z sieci, ujemny=eksport
  unsigned long lastOkMs=0;
} md;

unsigned long lastPoll = 0;

void pollMeter(){
  float v;
  // Total active power (float32 w 0.1 W) => /10 = W
  if (readFloat32_scaled(0x2012, 10.0f, v)) {
    md.Pt = v;
    md.lastOkMs = millis();
  }
}

//////////////////// SSR / KONTROLA ////////////////////
// Sprzęt
const int SSR_PIN = 13;                 // D13 -> SSR IN+; SSR IN- -> GND
// Parametry sterowania
const float P_MAX = 2000.0f;            // maks. moc grzałki (W)
const int   WINDOW_SECONDS = 1;         // okno 1 s
const int   HALF_CYCLES_PER_SECOND = 100; // 50 Hz => 100 półokresów/s
const int   N_HALF = WINDOW_SECONDS * HALF_CYCLES_PER_SECOND;
const float TAU_UP   = 5.0f;            // EMA dla wzrostu (sek)
const float TAU_DOWN = 1.0f;            // EMA dla spadku (sek, szybciej)
const float IMPORT_DEADBAND_W = 15.0f;  // martwa strefa importu (W)

// Zmienne sterowania
float P_applied = 0.0f;     // bieżąca moc przyłożona do grzałki (W) po filtrze
int   on_cycles = 0;        // ile półokresów w oknie ON
int   phaseIndex = 0;       // indeks półokresu w oknie
unsigned long lastHalfMillis = 0;

void setupSSR(){
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);
  phaseIndex = 0;
  lastHalfMillis = millis();
  P_applied = 0.0f;
}

// Wyznacz P_target tylko z aktualnego bilansu:
// - jeśli eksport (Pt<0): użyj nadwyżki do grzałki
// - jeśli import (Pt>=0): nie grzej
float computeTargetPower(){
  if (md.Pt < 0.0f) {
    float exportW = -md.Pt;
    if (exportW > P_MAX) exportW = P_MAX;
    return exportW;
  } else {
    return 0.0f;
  }
}

// EMA asymetryczne: szybciej w dół (gdy import) niż w górę (gdy eksport)
void updateControlWindow(){
  float P_target = computeTargetPower();

  // import wyraźnie dodatni -> użyj szybkiego opadania (tau_down)
  bool importing = (md.Pt > IMPORT_DEADBAND_W);
  float tau = importing ? TAU_DOWN : TAU_UP;

  float k = (float)WINDOW_SECONDS / (tau + (float)WINDOW_SECONDS);
  if (k < 0.0f) k = 0.0f; if (k > 1.0f) k = 1.0f;

  // EMA
  P_applied = P_applied + k * (P_target - P_applied);
  if (P_applied < 0.0f) P_applied = 0.0f;
  if (P_applied > P_MAX) P_applied = P_MAX;

  // Duty -> ilość półokresów w oknie
  float duty = P_applied / P_MAX;              // 0..1
  on_cycles = (int)round(duty * N_HALF);       // 0..N_HALF
  if (on_cycles < 0) on_cycles = 0;
  if (on_cycles > N_HALF) on_cycles = N_HALF;
}

// Tyka co ok. 10 ms (półokres 50 Hz), bez blokowania
void halfCycleTick(){
  const unsigned long HALF_MS = 10; // ~10 ms
  unsigned long now = millis();
  if (now - lastHalfMillis >= HALF_MS){
    lastHalfMillis += HALF_MS;

    // Początek okna -> przelicz filtr i on_cycles
    if (phaseIndex == 0) updateControlWindow();

    // Ustaw SSR w tym półokresie
    if (phaseIndex < on_cycles) digitalWrite(SSR_PIN, HIGH);
    else                         digitalWrite(SSR_PIN, LOW);

    // Kolejny półokres
    phaseIndex++;
    if (phaseIndex >= N_HALF) phaseIndex = 0;
  }
}

//////////////////// WWW ////////////////////
String htmlPage(){
  // Minimalny, czytelny panel: pobór + moc grzałki
  String s = R"HTML(
<!doctype html><html lang="pl"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bilans i grzałka</title>
<style>
body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:18px;background:#fafafa}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}
.card{background:#fff;border:1px solid #e5e7eb;border-radius:14px;padding:14px}
.kv{display:flex;justify-content:space-between;align-items:baseline}
.mono{font-variant-numeric:tabular-nums}
h1{margin:0 0 10px}
small{opacity:.6}
</style>
<h1>Bilans i grzałka</h1>
<div class="grid">
  <div class="card"><div class="kv"><b>Pobór z sieci</b><span class="mono" id="imp">– W</span></div></div>
  <div class="card"><div class="kv"><b>Moc grzałki</b><span class="mono" id="heater">– W</span></div></div>
  <div class="card"><small>Algorytm: automatyczna dywersja nadwyżki, wzrost wolny / spadek szybki (EMA asymetryczna), okno 1 s.</small></div>
</div>
<script>
function fmt(v,d=1){return Number(v).toFixed(d)}
async function tick(){
  try{
    const r = await fetch('/data.json');
    const j = await r.json();
    document.getElementById('imp').textContent    = fmt(Math.max(0, j.grid_import_W),1)+' W';
    document.getElementById('heater').textContent = fmt(j.heater_W,1)+' W';
  }catch(e){}
}
setInterval(tick, 1000); tick();
</script>
</html>
)HTML";
  return s;
}

void handleRoot(){ server.send(200,"text/html; charset=utf-8", htmlPage()); }

void handleData(){
  float grid_import_W = md.Pt > 0 ? md.Pt : 0.0f; // tylko dodatnie (pobór)
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"grid_import_W\":%.1f,\"heater_W\":%.1f}",
    grid_import_W, P_applied
  );
  server.send(200,"application/json; charset=utf-8", buf);
}

//////////////////// SETUP / LOOP ////////////////////
void setup(){
  Serial.begin(115200);
  delay(200);

  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);

  RS485.begin(UART_BAUD, SERIAL_MODE, 16, 17); // RX=16, TX=17
  RS485.setTimeout(500);
  node.begin(METER_ID, RS485);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns1, dns2);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  // WWW
  server.on("/", handleRoot);
  server.on("/data.json", handleData);
  server.begin();

  // SSR
  setupSSR();

  // Pierwszy odczyt + start
  pollMeter();
  updateControlWindow();
}

void loop(){
  server.handleClient();

  // Odczyt mocy co ~500 ms (szybciej, żeby feedback był żywszy)
  if (millis() - lastPoll > 500) {
    lastPoll = millis();
    pollMeter();
  }

  // Sterowanie burst-fire
  halfCycleTick();
}