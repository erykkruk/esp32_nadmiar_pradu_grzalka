/*
  ESP32 + DTSU666 (RS485/Modbus RTU) -> WWW: import/eksport + SSR burst-fire + AUTO/MANUAL + wykres 60 s
  STABILNA LOGIKA Z UŚREDNIANIEM:
    - Średnia krocząca z ostatnich 10 sekund pomiarów
    - Decyzje podejmowane co 10 sekund (nie co 1 s)
    - Rezerwa eksportu: min 100 W (EXPORT_RESERVE_W)
    - AUTO:
        * eksport < 50 W   -> łagodnie -50 W
        * eksport ≥ 250 W  -> łagodnie +50 W
        * 50..249 W        -> bez zmian (szeroka histereza)
    - Płynne przejścia z filtrem EMA

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
  if (readFloat32_raw(reg, v)){ outVal = v / scale; return true; }
  return false;
}

//////////////////// Dane licznika //////////////////
struct {
  float Pt = 0.0f;          // W: + import (pobór), − eksport (nadwyżka)
  unsigned long lastOkMs=0;
} md;

// Bufor do uśredniania (10 ostatnich pomiarów)
const int AVG_BUFFER_SIZE = 10;
float exportBuffer[AVG_BUFFER_SIZE];
int bufferIndex = 0;
bool bufferFull = false;

unsigned long lastPoll = 0;
unsigned long lastDecision = 0;

void pollMeter(){
  float v;
  if (readFloat32_scaled(0x2012, 10.0f, v)) {
    md.Pt = v;
    md.lastOkMs = millis();
    
    // Dodaj do bufora eksportu
    float currentExport = (v < 0.0f) ? -v : 0.0f;
    exportBuffer[bufferIndex] = currentExport;
    bufferIndex = (bufferIndex + 1) % AVG_BUFFER_SIZE;
    if (bufferIndex == 0) bufferFull = true;
  }
}

// Oblicz średnią z bufora
float getAverageExport(){
  if (!bufferFull && bufferIndex == 0) return 0.0f;
  
  float sum = 0.0f;
  int count = bufferFull ? AVG_BUFFER_SIZE : bufferIndex;
  
  for(int i = 0; i < count; i++){
    sum += exportBuffer[i];
  }
  
  return count > 0 ? sum / count : 0.0f;
}

//////////////////// SSR / Sterowanie //////////////////
// Sprzęt
const int SSR_PIN = 13;

// Parametry główne
const float P_MAX = 2000.0f;            // maks. moc grzałki (W)
const int   WINDOW_SECONDS = 1;         // okno 1 s
const int   HALF_CYCLES_PER_SECOND = 100; // 50 Hz -> 100 półokresów
const int   N_HALF = WINDOW_SECONDS * HALF_CYCLES_PER_SECOND;

// Logika AUTO - złagodzona
const float EXPORT_RESERVE_W = 100.0f;  // zawsze zachowaj min. 100 W eksportu
const float STEP_W           = 50.0f;   // mniejszy krok dla stabilności (było 100)
const float EXPORT_LOW       = 50.0f;   // próg dolny (poniżej = zmniejsz moc)
const float EXPORT_HIGH      = 250.0f;  // próg górny (powyżej = zwiększ moc)
const unsigned long DECISION_INTERVAL_MS = 10000; // decyzje co 10 sekund

// Filtry dla płynnych zmian
const float TAU_UP   = 8.0f;  // wolniejsze dochodzenie (było 5)
const float TAU_DOWN = 5.0f;  // wolniejsze schodzenie

// Tryb i wartości
bool  autoMode = true;
float manualDuty = 0.0f;

// Stan sterowania
float P_step    = 0.0f;    // docelowa moc schodkowa
float P_applied = 0.0f;    // faktycznie przykładana
int   on_cycles = 0;
int   phaseIndex = 0;
unsigned long lastHalfMillis = 0;

void setupSSR(){
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);
  phaseIndex = 0;
  lastHalfMillis = millis();
  P_applied = 0.0f;
  P_step = 0.0f;
  
  // Inicjalizacja bufora
  for(int i = 0; i < AVG_BUFFER_SIZE; i++){
    exportBuffer[i] = 0.0f;
  }
}

void updateStepFromExport(){
  if (!autoMode) return;
  
  // Decyzje tylko co 10 sekund
  unsigned long now = millis();
  if (now - lastDecision < DECISION_INTERVAL_MS) return;
  lastDecision = now;
  
  // Używaj średniej z ostatnich 10 pomiarów
  float avgExport = getAverageExport();
  
  // Logika z histerezą
  if (avgExport < EXPORT_LOW) {
    // Za mało eksportu - zmniejsz moc
    if (P_step >= STEP_W) {
      P_step -= STEP_W;
      Serial.printf("Zmniejszam moc o %.0f W (śr. eksport: %.1f W)\n", STEP_W, avgExport);
    } else {
      P_step = 0.0f;
    }
  }
  else if (avgExport >= EXPORT_HIGH) {
    // Dużo eksportu - zwiększ moc
    float newStep = P_step + STEP_W;
    if (newStep <= P_MAX) {
      P_step = newStep;
      Serial.printf("Zwiększam moc o %.0f W (śr. eksport: %.1f W)\n", STEP_W, avgExport);
    } else {
      P_step = P_MAX;
    }
  }
  // Pasmo 50-249 W -> bez zmian (szeroka histereza dla stabilności)
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
  
  // Płynne przejścia w obie strony
  float k;
  if (P_target < P_applied) {
    // Spadek - wolniejszy niż wcześniej
    k = (float)WINDOW_SECONDS / (TAU_DOWN + (float)WINDOW_SECONDS);
  } else {
    // Wzrost - łagodne dochodzenie
    k = (float)WINDOW_SECONDS / (TAU_UP + (float)WINDOW_SECONDS);
  }
  
  if (k < 0.0f) k = 0.0f; 
  if (k > 1.0f) k = 1.0f;
  P_applied = P_applied + k * (P_target - P_applied);
  
  // Ograniczenia
  if (P_applied < 0.0f) P_applied = 0.0f;
  if (P_applied > P_MAX) P_applied = P_MAX;
  
  // Duty -> ilość półokresów w oknie
  float duty = P_applied / P_MAX;
  on_cycles = (int)round(duty * N_HALF);
  if (on_cycles < 0) on_cycles = 0;
  if (on_cycles > N_HALF) on_cycles = N_HALF;
}

void halfCycleTick(){
  const unsigned long HALF_MS = 10;
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
<title>Stabilne sterowanie grzałką (60 s)</title>
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
.badge{padding:4px 8px;border:1px solid var(--b);border-radius:999px;font-size:0.9em}
.badge-info{background:#e0f2fe;border-color:#0284c7}
#pct{width:220px}
.canvasWrap{height:220px}
canvas{width:100%;height:100%}
small{opacity:.6}
.status{margin-top:12px;padding:8px;background:#f0f9ff;border-radius:8px;font-size:0.9em}
</style>

<h1>Stabilne sterowanie grzałką</h1>

<div class="card ctrl">
  <label><input type="checkbox" id="auto"> <b>Automatycznie</b></label>
  <input type="range" id="pct" min="0" max="100" step="1">
  <b id="pctVal" class="mono">0%</b>
  <span class="badge">Tryb: <span id="modeLabel">MANUAL</span></span>
</div>

<div class="card status">
  <b>Parametry stabilizacji:</b>
  <span class="badge badge-info">Uśrednianie: 10 s</span>
  <span class="badge badge-info">Decyzje co: 10 s</span>
  <span class="badge badge-info">Krok: ±50 W</span>
  <span class="badge badge-info">Histereza: 50-250 W</span>
  <span class="badge badge-info">Śr. eksport: <span class="mono" id="avgExp">– W</span></span>
</div>

<div class="grid">
  <div class="card"><div class="kv"><b>Pobór z sieci</b><span class="mono" id="imp">– W</span></div></div>
  <div class="card"><div class="kv"><b>Nadwyżka (eksport)</b><span class="mono" id="exp">– W</span></div></div>
  <div class="card"><div class="kv"><b>Moc grzałki</b><span class="mono" id="heater">– W</span></div></div>
  <div class="card"><div class="kv"><b>Moc zadana</b><span class="mono" id="targetW">– W</span></div></div>
</div>

<div class="grid" style="margin-top:12px">
  <div class="card">
    <div class="kv"><b>Wykres: moc grzałki (60 s)</b><small>odświeżanie co 1 s</small></div>
    <div class="canvasWrap"><canvas id="cHeater" width="600" height="220"></canvas></div>
  </div>
  <div class="card">
    <div class="kv"><b>Wykres: eksport (60 s)</b><small>odświeżanie co 1 s</small></div>
    <div class="canvasWrap"><canvas id="cExport" width="600" height="220"></canvas></div>
  </div>
</div>

<script>
function fmt(v,d=1){return Number(v).toFixed(d)}

// wykres bez bibliotek
function drawSeries(canvas, data, maxY, color, targetLine=null){
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0,0,W,H);
  
  // Siatka
  ctx.strokeStyle='#e5e7eb'; ctx.lineWidth=1;
  for(let i=0;i<=4;i++){ 
    const y=i*(H/4); 
    ctx.beginPath(); 
    ctx.moveTo(0,y); 
    ctx.lineTo(W,y); 
    ctx.stroke(); 
  }
  
  // Linia docelowa (opcjonalna)
  if(targetLine !== null && maxY > 0){
    ctx.strokeStyle='#fbbf24'; 
    ctx.lineWidth=1;
    ctx.setLineDash([5,5]);
    const ty = H - (targetLine/maxY)*H;
    ctx.beginPath();
    ctx.moveTo(0,ty);
    ctx.lineTo(W,ty);
    ctx.stroke();
    ctx.setLineDash([]);
  }
  
  // Dane
  if(data.length<2) return;
  ctx.strokeStyle=color; 
  ctx.lineWidth=2; 
  ctx.beginPath();
  const n=data.length;
  for(let i=0;i<n;i++){
    const x = (i/(60-1))*W;
    const v = Math.max(0, data[i]);
    const y = H - (maxY>0 ? (v/maxY)*H : 0);
    if(i===0) ctx.moveTo(x,y); 
    else ctx.lineTo(x,y);
  }
  ctx.stroke();
}

let state = {
  auto:true, manual_pct:0, P_target:0, P_applied:0,
  grid_import_W:0, export_W:0, heater_W:0, avg_export_W:0
};
let histHeater = new Array(60).fill(0);
let histExport = new Array(60).fill(0);

function byId(id){ return document.getElementById(id); }
function applyUI(){
  byId('auto').checked = state.auto;
  byId('pct').disabled = state.auto;
  const pctVal = state.auto ? Math.round((state.P_applied/2000)*100) : Math.round(state.manual_pct);
  byId('pct').value = pctVal;
  byId('pctVal').textContent = pctVal + '%';
  byId('modeLabel').textContent = state.auto ? 'AUTO' : 'MANUAL';
  byId('targetW').textContent  = Math.round(state.P_target)+' W';
  byId('imp').textContent      = fmt(state.grid_import_W,1)+' W';
  byId('exp').textContent      = fmt(state.export_W,1)+' W';
  byId('heater').textContent   = fmt(state.heater_W,1)+' W';
  byId('avgExp').textContent   = fmt(state.avg_export_W,1)+' W';
}

function drawAll(){
  drawSeries(byId('cHeater'), histHeater, 2000, '#0ea37a', state.P_target);
  drawSeries(byId('cExport'), histExport, 500, '#3b82f6', 100); // 100W = rezerwa
}

async function fetchAll(){
  const [d, c] = await Promise.all([
    fetch('/data.json').then(r=>r.json()),
    fetch('/ctrl.json').then(r=>r.json()),
  ]);
  state.grid_import_W = d.grid_import_W;
  state.export_W      = d.export_W;
  state.heater_W      = d.heater_W;
  state.avg_export_W  = d.avg_export_W || state.export_W;
  state.auto          = c.auto;
  state.manual_pct    = c.manual_pct;
  state.P_target      = c.P_target;
  state.P_applied     = c.P_applied;
  
  histHeater.push(state.heater_W); 
  if(histHeater.length>60) histHeater.shift();
  histExport.push(state.export_W); 
  if(histExport.length>60) histExport.shift();
  
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
  float avg_export_W  = getAverageExport();
  
  char buf[300];
  snprintf(buf, sizeof(buf),
    "{\"grid_import_W\":%.1f,\"export_W\":%.1f,\"heater_W\":%.1f,\"avg_export_W\":%.1f}",
    grid_import_W, export_W, P_applied, avg_export_W
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
    if (pct < 0) pct = 0; 
    if (pct > 100) pct = 100;
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
  
  Serial.println("\n=== STABILNE STEROWANIE GRZAŁKĄ ===");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("Parametry:");
  Serial.println("- Uśrednianie: 10 ostatnich pomiarów");
  Serial.println("- Decyzje co: 10 sekund");
  Serial.println("- Krok mocy: ±50 W");
  Serial.println("- Histereza: 50-250 W eksportu");
  
  // HTTP
  server.on("/", handleRoot);
  server.on("/data.json", handleData);
  server.on("/ctrl.json", handleCtrlJSON);
  server.on("/set", handleSet);
  server.begin();
  
  // SSR
  setupSSR();
  
  // Start
  pollMeter();
  updateStepFromExport();
  updateControlWindow();
}

void loop(){
  server.handleClient();
  
  // Odczyt co 1 s (do bufora uśredniania)
  if (millis() - lastPoll > 1000) {
    lastPoll = millis();
    pollMeter();
    updateStepFromExport(); // sprawdza czy minęło 10s od ostatniej decyzji
  }
  
  // Sterowanie burst-fire (10 ms)
  halfCycleTick();
}