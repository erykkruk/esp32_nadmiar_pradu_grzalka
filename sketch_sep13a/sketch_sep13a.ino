/*
  ESP32 + DTSU666 (RS485/Modbus RTU) -> WWW: import/eksport + SSR burst-fire + AUTO/MANUAL + wykres 60 s
  LOGIKA SCHODKOWA (z uspokojeniem):
    - Rezerwa eksportu: min 150 W (EXPORT_RESERVE_W)
    - Deadband: ±150 W wokół rezerwy (DEADBAND_W)
    - Krok: 250 W (STEP_W)
    - AUTO:
        * eksport < (rezerwa - deadband)  -> -250 W (nie częściej niż co DEC_COOLDOWN_MS)
        * eksport > (rezerwa + deadband)  -> +250 W (nie częściej niż co INC_COOLDOWN_MS)
        * w strefie martwej               -> bez zmian (pamięć stanu)
    - EMA na eksporcie (tylko do decyzji schodka); P_applied: EMA tylko w górę, w dół natychmiast
    - Minimalny czas podtrzymania poziomu po każdej zmianie (MIN_HOLD_MS)

  Sieć:    TP-Link_E6D1 / 80246459
  IP:      192.168.255.50 (statyczne)
  RS485:   Serial2  RX=16, TX=17, DE/RE=GPIO4
  Modbus:  9600 bps, O-8-1, slave id = 1
  Moc:     Total active power @0x2012 = float32 w 0.1 W  => /10 => W
           (+) pobór z sieci, (−) eksport do sieci

  SSR:     SSR IN+ = GPIO13, IN− = GND (zalecany driver tranzystorowy)
  Ster.:   okno 1 s, burst-fire 50 Hz (10 ms)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>

//////////////////// Wi-Fi ////////////////////
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

//////////////////// Pomocnicze //////////////////
bool readFloat32_raw(uint16_t reg, float& outVal) {
  for (int i=0;i<2;i++){
    uint8_t r = node.readHoldingRegisters(reg, 2);
    if (r == node.ku8MBSuccess){
      uint16_t hi = node.getResponseBuffer(0);
      uint16_t lo = node.getResponseBuffer(1);
      uint32_t raw = ((uint32_t)hi << 16) | lo; // przy dziwnych wynikach spróbuj (lo<<16)|hi
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

//////////////////// Dane licznika //////////////////
struct {
  float Pt = 0.0f;          // W: + import (pobór), − eksport (nadwyżka)
  unsigned long lastOkMs=0;
} md;

unsigned long lastPoll = 0;

//////////////////// SSR / Sterowanie //////////////////
// Sprzęt
const int SSR_PIN = 13;                 // D13 -> SSR IN+; SSR IN- -> GND

// Parametry główne
const float P_MAX = 2000.0f;            // maks. moc grzałki (W)
const int   WINDOW_SECONDS = 1;         // okno 1 s
const int   HALF_CYCLES_PER_SECOND = 100; // 50 Hz -> 100 półokresów
const int   N_HALF = WINDOW_SECONDS * HALF_CYCLES_PER_SECOND;

// Logika AUTO (uspokojona)
const float EXPORT_RESERVE_W        = 150.0f;   // rezerwa eksportu
const float STEP_W                  = 250.0f;   // wielkość schodka
const float DEADBAND_W              = 150.0f;   // strefa martwa ±150 W
const unsigned long INC_COOLDOWN_MS = 3000;     // min odstęp między kolejnymi +250 W
const unsigned long DEC_COOLDOWN_MS = 1500;     // min odstęp między kolejnymi -250 W
const unsigned long MIN_HOLD_MS     = 3000;     // minimalny czas podtrzymania poziomu po każdej zmianie

// Filtr na decyzji (EMA na eksporcie)
const float TAU_EXPORT_DECISIONS = 2.0f;        // sek

// Filtr wzrostu na P_applied; spadek natychmiast
const float TAU_UP = 5.0f; // sek

// Tryb i wartości
bool  autoMode = true;     // AUTO: z exportu; MANUAL: z suwaka
float manualDuty = 0.0f;   // 0..1 (tylko MANUAL)

// Stan sterowania
float P_step    = 0.0f;    // docelowa moc schodkowa (0..P_MAX), pamięć stanu
float P_applied = 0.0f;    // faktycznie przykładana (po filtrze wzrostowym)
int   on_cycles = 0;
int   phaseIndex = 0;
unsigned long lastHalfMillis = 0;
unsigned long lastIncMs = 0;
unsigned long lastDecMs = 0;
unsigned long lastChangeMs = 0;

// Dodatkowe: przefiltrowany eksport do decyzji
float exportW_filt = 0.0f;
bool  exportFiltInit = false;

void setupSSR(){
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);
  phaseIndex = 0;
  lastHalfMillis = millis();
  P_applied = 0.0f;
  P_step = 0.0f;
}

void pollMeter(){
  float v;
  if (readFloat32_scaled(0x2012, 10.0f, v)) { // Total active power /10 => W
    md.Pt = v;
    md.lastOkMs = millis();

    // surowy eksport (≥0)
    float exportW_raw = (md.Pt < 0.0f) ? -md.Pt : 0.0f;

    // EMA do decyzji (dwukierunkowa, krótka)
    if(!exportFiltInit){ exportW_filt = exportW_raw; exportFiltInit = true; }
    float dt = 1.0f; // bo wołamy co ~1 s
    float k  = dt / (TAU_EXPORT_DECISIONS + dt);
    if (k < 0) k = 0; if (k > 1) k = 1;
    exportW_filt = exportW_filt + k * (exportW_raw - exportW_filt);
  }
}

void updateStepFromExport(){
  if (!autoMode) return;

  const float exportW = exportW_filt; // używamy przefiltrowanego eksportu
  const unsigned long now = millis();

  const float lowThresh  = EXPORT_RESERVE_W - DEADBAND_W;
  const float highThresh = EXPORT_RESERVE_W + DEADBAND_W;

  // jeśli nie minęło MIN_HOLD_MS od ostatniej zmiany, nie ruszamy
  if (now - lastChangeMs < MIN_HOLD_MS) return;

  // Za mały eksport -> zmniejszamy krok (z cooldownem w dół)
  if (exportW < lowThresh) {
    if (now - lastDecMs >= DEC_COOLDOWN_MS) {
      P_step -= STEP_W;
      if (P_step < 0) P_step = 0;
      lastDecMs = now;
      lastChangeMs = now;
    }
    return;
  }

  // Duży eksport -> zwiększamy krok (z cooldownem w górę)
  if (exportW > highThresh) {
    if (now - lastIncMs >= INC_COOLDOWN_MS) {
      P_step += STEP_W;
      if (P_step > P_MAX) P_step = P_MAX;
      lastIncMs = now;
      lastChangeMs = now;
    }
    return;
  }

  // W strefie martwej: bez zmian
}

float computeTargetPower(){
  if (autoMode) {
    if (P_step < 0) P_step = 0;
    if (P_step > P_MAX) P_step = P_MAX;
    return P_step;
  } else {
    float P_target = constrain(manualDuty, 0.0f, 1.0f) * P_MAX;
    return P_target;
  }
}

void updateControlWindow(){
  float P_target = computeTargetPower();

  // Spadek -> natychmiastowe zejście
  if (P_target < P_applied) {
    P_applied = P_target;
  } else {
    // Wzrost -> łagodne dochodzenie (EMA)
    float k = (float)WINDOW_SECONDS / (TAU_UP + (float)WINDOW_SECONDS);
    if (k < 0.0f) k = 0.0f; if (k > 1.0f) k = 1.0f;
    P_applied = P_applied + k * (P_target - P_applied);
  }

  // Ograniczenia
  if (P_applied < 0.0f) P_applied = 0.0f;
  if (P_applied > P_MAX) P_applied = P_MAX;

  // Duty -> ilość półokresów w oknie
  float duty = P_applied / P_MAX;              // 0..1
  on_cycles = (int)round(duty * N_HALF);       // 0..N_HALF
  if (on_cycles < 0) on_cycles = 0;
  if (on_cycles > N_HALF) on_cycles = N_HALF;
}

void halfCycleTick(){
  const unsigned long HALF_MS = 10; // ~10 ms
  unsigned long now = millis();
  if (now - lastHalfMillis >= HALF_MS){
    lastHalfMillis += HALF_MS;

    if (phaseIndex == 0) updateControlWindow();

    if (phaseIndex < on_cycles) digitalWrite(SSR_PIN, HIGH);
    else                        digitalWrite(SSR_PIN, LOW);

    phaseIndex++;
    if (phaseIndex >= N_HALF) phaseIndex = 0;
  }
}

//////////////////// WWW //////////////////
String htmlPage(){
  String s = R"HTML(
<!doctype html><html lang="pl"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bilans, nadwyżka i grzałka (60 s)</title>
<style>
:root{ --ok:#0ea37a; --bad:#d64545; --b:#e5e7eb; }
*{box-sizing:border-box}
body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:16px;background:#fafafa}
h1{margin:0 0 10px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
.card{background:#fff;border:1px solid var(--b);border-radius:14px;padding:12px}
.kv{display:flex;justify-content:space-between;align-items:baseline}
.mono{font-variant-numeric:tabular-nums}
.ctrl{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
.badge{padding:4px 8px;border:1px solid var(--b);border-radius:999px}
#pct{width:220px}
.canvasWrap{height:220px}
canvas{width:100%;height:100%}
small{opacity:.6}
</style>

<h1>Bilans, nadwyżka i grzałka</h1>

<div class="card ctrl">
  <label><input type="checkbox" id="auto"> <b>Automatycznie</b></label>
  <input type="range" id="pct" min="0" max="100" step="1">
  <b id="pctVal" class="mono">0%</b>
  <span class="badge">Tryb: <span id="modeLabel">MANUAL</span></span>
  <span class="badge">Rezerwa: <span class="mono">150 W</span></span>
  <span class="badge">Deadband: <span class="mono">±150 W</span></span>
  <span class="badge">Krok AUTO: <span class="mono">250 W</span></span>
  <span class="badge">Zadana: <span class="mono" id="targetW">0 W</span></span>
  <span class="badge">Przyłożona: <span class="mono" id="appliedW">0 W</span></span>
</div>

<div class="grid">
  <div class="card"><div class="kv"><b>Pobór z sieci</b><span class="mono" id="imp">– W</span></div></div>
  <div class="card"><div class="kv"><b>Nadwyżka (eksport)</b><span class="mono" id="exp">– W</span></div></div>
  <div class="card"><div class="kv"><b>Moc grzałki</b><span class="mono" id="heater">– W</span></div></div>
</div>

<div class="grid" style="margin-top:12px">
  <div class="card">
    <div class="kv"><b>Wykres: moc grzałki (60 s)</b><small>odświeżanie co 1 s</small></div>
    <div class="canvasWrap"><canvas id="cHeater" width="600" height="220"></canvas></div>
  </div>
  <div class="card">
    <div class="kv"><b>Wykres: pobór z sieci (60 s)</b><small>odświeżanie co 1 s</small></div>
    <div class="canvasWrap"><canvas id="cImport" width="600" height="220"></canvas></div>
  </div>
</div>

<script>
function fmt(v,d=1){return Number(v).toFixed(d)}

// wykres bez bibliotek
function drawSeries(canvas, data, maxY, color){
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0,0,W,H);
  ctx.strokeStyle='#e5e7eb'; ctx.lineWidth=1;
  for(let i=0;i<=4;i++){ const y=i*(H/4); ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(W,y); ctx.stroke(); }
  ctx.strokeStyle='#ccc'; ctx.beginPath(); ctx.moveTo(0,H-0.5); ctx.lineTo(W,H-0.5); ctx.stroke();
  if(data.length<2) return;
  ctx.strokeStyle=color; ctx.lineWidth=2; ctx.beginPath();
  const n=data.length;
  for(let i=0;i<n;i++){
    const x = (i/(60-1))*W;
    const v = Math.max(0, data[i]);
    const y = H - (maxY>0 ? (v/maxY)*H : 0);
    if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  }
  ctx.stroke();
}

let state = {
  auto:true, manual_pct:0, P_target:0, P_applied:0,
  grid_import_W:0, export_W:0, heater_W:0
};
let histHeater = new Array(60).fill(0);
let histImport = new Array(60).fill(0);

function byId(id){ return document.getElementById(id); }
function applyUI(){
  byId('auto').checked = state.auto;
  byId('pct').disabled = state.auto;
  const pctVal = state.auto ? Math.round((state.P_applied/2000)*100) : Math.round(state.manual_pct);
  byId('pct').value = pctVal;
  byId('pctVal').textContent = pctVal + '%';
  byId('modeLabel').textContent = state.auto ? 'AUTO' : 'MANUAL';
  byId('targetW').textContent = Math.round(state.P_target)+' W';
  byId('appliedW').textContent = Math.round(state.P_applied)+' W';
  byId('imp').textContent     = fmt(state.grid_import_W,1)+' W';
  byId('exp').textContent     = fmt(state.export_W,1)+' W';
  byId('heater').textContent  = fmt(state.heater_W,1)+' W';
}

function drawAll(){
  drawSeries(byId('cHeater'), histHeater, 2000, '#0ea37a');
  drawSeries(byId('cImport'), histImport, 3000, '#d64545');
}

async function fetchAll(){
  const [d, c] = await Promise.all([
    fetch('/data.json').then(r=>r.json()),
    fetch('/ctrl.json').then(r=>r.json()),
  ]);
  state.grid_import_W = d.grid_import_W;
  state.export_W      = d.export_W;
  state.heater_W      = d.heater_W;
  state.auto        = c.auto;
  state.manual_pct  = c.manual_pct;
  state.P_target    = c.P_target;
  state.P_applied   = c.P_applied;

  histHeater.push(state.heater_W); if(histHeater.length>60) histHeater.shift();
  histImport.push(state.grid_import_W); if(histImport.length>60) histImport.shift();

  applyUI();
  drawAll();
}

async function setCtrl(params){
  const u = new URLSearchParams(params);
  const r = await fetch('/set?'+u.toString());
  return r.json();
}

document.addEventListener('DOMContentLoaded', ()=>{
  byId('auto').addEventListener('change', async (e)=>{
    const mode = e.target.checked ? 'auto' : 'manual';
    const pct  = byId('pct').value;
    await setCtrl({mode,duty:pct});
    fetchAll().catch(()=>{});
  });
  byId('pct').addEventListener('input', ()=>{
    byId('pctVal').textContent = byId('pct').value + '%';
  });
  byId('pct').addEventListener('change', async ()=>{
    const pct = byId('pct').value;
    await setCtrl({duty:pct});
    fetchAll().catch(()=>{});
  });

  fetchAll().catch(()=>{});
  setInterval(()=>fetchAll().catch(()=>{}), 1000);
});
</script>
</html>
)HTML";
  return s;
}

void handleRoot(){ server.send(200,"text/html; charset=utf-8", htmlPage()); }

// Zwięzłe dane do wykresów/UI
void handleData(){
  float grid_import_W = md.Pt > 0 ? md.Pt : 0.0f;
  float export_W      = md.Pt < 0 ? -md.Pt : 0.0f;
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"grid_import_W\":%.1f,\"export_W\":%.1f,\"heater_W\":%.1f}",
    grid_import_W, export_W, P_applied
  );
  server.send(200,"application/json; charset=utf-8", buf);
}

// Stan kontrolera (dla UI)
void handleCtrlJSON(){
  float P_target = computeTargetPower();
  float manual_pct = manualDuty * 100.0f;
  char buf[400];
  snprintf(buf, sizeof(buf),
    "{\"auto\":%s,\"manual_pct\":%.1f,"
    "\"P_target\":%.1f,\"P_applied\":%.1f,"
    "\"on_cycles\":%d,\"window_halfcycles\":%d}",
    autoMode?"true":"false", manual_pct,
    P_target, P_applied,
    on_cycles, N_HALF
  );
  server.send(200, "application/json; charset=utf-8", buf);
}

// Ustawienia z UI
void handleSet(){
  if (server.hasArg("mode")){
    String m = server.arg("mode");
    if (m == "auto")   autoMode = true;
    if (m == "manual") autoMode = false;
  }
  if (server.hasArg("duty")){
    int pct = server.arg("duty").toInt();
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    manualDuty = pct / 100.0f;
  }
  handleCtrlJSON();
}

//////////////////// SETUP / LOOP //////////////////
void setup(){
  Serial.begin(115200);
  delay(200);

  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);

  RS485.begin(UART_BAUD, SERIAL_MODE, 16, 17);
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

  // HTTP
  server.on("/", handleRoot);
  server.on("/data.json", handleData);
  server.on("/ctrl.json", handleCtrlJSON);
  server.on("/set", handleSet);
  server.begin();

  // SSR
  setupSSR();

  // Start
  pollMeter();              // wstępny odczyt + inicjalizacja filtru eksportu
  updateStepFromExport();   // inicjalizuj stan schodka na podstawie pierwszego pomiaru
  updateControlWindow();
}

void loop(){
  server.handleClient();

  // Odczyt i aktualizacja stanu co ~1 s
  if (millis() - lastPoll > 1000) {
    lastPoll = millis();
    pollMeter();
    updateStepFromExport(); // decyzja: +250 / -250 / bez zmiany (z histerezą i cooldownami)
  }

  // Sterowanie burst-fire (10 ms)
  halfCycleTick();
}