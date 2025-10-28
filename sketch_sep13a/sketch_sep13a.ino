/*
  ESP32 Heater Control System (System Kontroli Grzałki ESP32)
  ESP32 + DTSU666 (RS485/Modbus RTU) -> Web Interface: import/export + SSR burst-fire + AUTO/MANUAL + 60s charts
  
  STABLE LOGIC WITH AVERAGING (STABILNA LOGIKA Z UŚREDNIANIEM):
    - Rolling average from last 10 seconds of measurements (Średnia krocząca z ostatnich 10 sekund pomiarów)
    - Decisions made every 10 seconds (not every 1s) (Decyzje podejmowane co 10 sekund, nie co 1s)
    - Export reserve: min 100W (EXPORT_RESERVE_W) (Rezerwa eksportu: min 100W)
    - AUTO mode logic (Logika trybu AUTO):
        * export < 50W   -> gently decrease by 50W (eksport < 50W -> łagodnie -50W)
        * export ≥ 250W  -> gently increase by 50W (eksport ≥ 250W -> łagodnie +50W)
        * 50..249W       -> no change (wide hysteresis) (50..249W -> bez zmian, szeroka histereza)
    - Smooth transitions with EMA filter (Płynne przejścia z filtrem EMA)

  Network (Sieć):    TP-Link_E6D1 / 80246459
  IP Address:        192.168.255.50 (static/statyczne)
  RS485 Interface:   Serial2  RX=16, TX=17, DE/RE=GPIO4
  Modbus Config:     9600 bps, O-8-1, slave id = 1
  Power Reading:     Total active power @0x2012 = float32 in 0.1W units => /10 => W
                     (+) grid import (pobór z sieci), (−) grid export (eksport do sieci)

  SSR Control:       SSR IN+ = GPIO13, IN− = GND (transistor driver recommended/zalecany driver tranzystorowy)
  Control Method:    1 second window, burst-fire 50Hz (10ms half-cycles) (okno 1s, burst-fire 50Hz półokresów 10ms)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>

//////////////////// Wi-Fi Configuration (Konfiguracja Wi-Fi) ////////////////////
const char* WIFI_SSID = "TP-Link_E6D1";
const char* WIFI_PASS = "80246459";
IPAddress local_IP(192,168,255,50), gateway(192,168,255,1),
         subnet(255,255,255,0), dns1(192,168,255,1), dns2(8,8,8,8);

//////////////////// RS485 / Modbus Configuration (Konfiguracja RS485/Modbus) ////////////////////
#define RS485_DE_RE 4
#define UART_BAUD   9600
#define SERIAL_MODE SERIAL_8O1
const uint8_t  METER_ID = 1;

HardwareSerial& RS485 = Serial2;
ModbusMaster node;
WebServer server(80);

// RS485 direction control functions (Funkcje sterowania kierunkiem RS485)
void preTransmission(){ digitalWrite(RS485_DE_RE, HIGH); }  // Enable transmit mode (Włącz tryb nadawania)
void postTransmission(){ digitalWrite(RS485_DE_RE, LOW);  }   // Enable receive mode (Włącz tryb odbioru)

//////////////////// Helper Functions (Funkcje Pomocnicze) ////////////////////
// Read 32-bit float from Modbus registers (raw value) (Odczyt 32-bitowej liczby zmiennoprzecinkowej z rejestrów Modbus - wartość surowa)
bool readFloat32_raw(uint16_t reg, float& outVal) {
  for (int i=0;i<2;i++){  // Try up to 2 times (Próbuj maksymalnie 2 razy)
    uint8_t r = node.readHoldingRegisters(reg, 2);  // Read 2 consecutive registers (Odczytaj 2 kolejne rejestry)
    if (r == node.ku8MBSuccess){
      uint16_t hi = node.getResponseBuffer(0);  // High word (Starsze słowo)
      uint16_t lo = node.getResponseBuffer(1);  // Low word (Młodsze słowo)
      uint32_t raw = ((uint32_t)hi << 16) | lo; // Combine into 32-bit value (Połącz w wartość 32-bitową)
      memcpy(&outVal, &raw, sizeof(float));     // Convert to float (Konwertuj na float)
      return true;
    }
    delay(5);  // Short delay before retry (Krótka pauza przed ponowną próbą)
  }
  return false;  // Communication failed (Komunikacja nieudana)
}
// Read 32-bit float and apply scaling (Odczyt 32-bitowej liczby zmiennoprzecinkowej z przeskalowaniem)
bool readFloat32_scaled(uint16_t reg, float scale, float& outVal){
  float v;
  if (readFloat32_raw(reg, v)){ 
    outVal = v / scale;  // Apply scaling factor (Zastosuj współczynnik skalowania)
    return true; 
  }
  return false;  // Read failed (Odczyt nieudany)
}

//////////////////// Energy Meter Data (Dane Licznika Energii) ////////////////////
// Energy meter data structure (Struktura danych licznika energii)
struct {
  float Pt = 0.0f;          // Power in Watts: + import from grid, − export to grid (Moc w Watach: + pobór z sieci, − eksport do sieci)
  unsigned long lastOkMs=0; // Timestamp of last successful reading (Znacznik czasu ostatniego udanego odczytu)
} md;

// Averaging buffer for last 10 measurements (Bufor uśredniania dla ostatnich 10 pomiarów)
const int AVG_BUFFER_SIZE = 10;
float exportBuffer[AVG_BUFFER_SIZE];  // Circular buffer for export values (Bufor cykliczny dla wartości eksportu)
int bufferIndex = 0;                  // Current buffer position (Aktualna pozycja w buforze)
bool bufferFull = false;              // Flag indicating if buffer is full (Flaga wskazująca czy bufor jest pełny)

unsigned long lastPoll = 0;
unsigned long lastDecision = 0;

// Poll energy meter for current power reading (Odpytaj licznik energii o aktualny odczyt mocy)
void pollMeter(){
  float v;
  if (readFloat32_scaled(0x2012, 10.0f, v)) {  // Read total active power register (Odczytaj rejestr całkowitej mocy czynnej)
    md.Pt = v;                    // Store power value (Zapisz wartość mocy)
    md.lastOkMs = millis();       // Update timestamp (Zaktualizuj znacznik czasu)
    
    // Add to export averaging buffer (Dodaj do bufora uśredniania eksportu)
    float currentExport = (v < 0.0f) ? -v : 0.0f;  // Convert negative power to positive export (Konwertuj ujemną moc na dodatni eksport)
    exportBuffer[bufferIndex] = currentExport;
    bufferIndex = (bufferIndex + 1) % AVG_BUFFER_SIZE;  // Circular buffer increment (Cykliczne zwiększenie indeksu bufora)
    if (bufferIndex == 0) bufferFull = true;            // Mark buffer as full when we wrap around (Oznacz bufor jako pełny gdy wracamy do początku)
  }
}

// Calculate average export power from buffer (Oblicz średnią moc eksportu z bufora)
float getAverageExport(){
  if (!bufferFull && bufferIndex == 0) return 0.0f;  // No data available (Brak dostępnych danych)
  
  float sum = 0.0f;
  int count = bufferFull ? AVG_BUFFER_SIZE : bufferIndex;  // Determine how many values to average (Określ ile wartości uśrednić)
  
  for(int i = 0; i < count; i++){  // Sum all values in buffer (Zsumuj wszystkie wartości w buforze)
    sum += exportBuffer[i];
  }
  
  return count > 0 ? sum / count : 0.0f;  // Return average or 0 if no data (Zwróć średnią lub 0 jeśli brak danych)
}

//////////////////// SSR Control System (System Sterowania SSR) ////////////////////
// Hardware configuration (Konfiguracja sprzętowa)
const int SSR_PIN = 13;  // GPIO pin connected to SSR control input (Pin GPIO podłączony do wejścia sterowania SSR)

// Main control parameters (Główne parametry sterowania)
const float P_MAX = 2000.0f;              // Maximum heater power in Watts (Maksymalna moc grzałki w Watach)
const int   WINDOW_SECONDS = 1;           // Control window duration in seconds (Czas trwania okna sterowania w sekundach)
const int   HALF_CYCLES_PER_SECOND = 100; // 50Hz AC = 100 half-cycles per second (50Hz AC = 100 półokresów na sekundę)
const int   N_HALF = WINDOW_SECONDS * HALF_CYCLES_PER_SECOND;  // Total half-cycles in control window (Całkowita liczba półokresów w oknie sterowania)

// AUTO mode logic - stabilized (Logika trybu AUTO - ustabilizowana)
const float EXPORT_RESERVE_W = 100.0f;   // Always maintain minimum 100W export (Zawsze zachowaj minimum 100W eksportu)
const float STEP_W           = 50.0f;    // Smaller step for stability (was 100W) (Mniejszy krok dla stabilności, było 100W)
const float EXPORT_LOW       = 50.0f;    // Lower threshold (below = decrease power) (Próg dolny, poniżej = zmniejsz moc)
const float EXPORT_HIGH      = 250.0f;   // Upper threshold (above = increase power) (Próg górny, powyżej = zwiększ moc)
const unsigned long DECISION_INTERVAL_MS = 10000; // Make decisions every 10 seconds (Podejmuj decyzje co 10 sekund)

// Filters for smooth transitions (Filtry dla płynnych przejść)
const float TAU_UP   = 8.0f;  // Slower rise time constant (was 5) (Wolniejsza stała czasowa narastania, było 5)
const float TAU_DOWN = 5.0f;  // Slower fall time constant (Wolniejsza stała czasowa opadania)

// Mode and control values (Tryb i wartości sterowania)
bool  autoMode = true;      // Auto/Manual mode flag (Flaga trybu Auto/Ręczny)
float manualDuty = 0.0f;    // Manual mode duty cycle (0.0-1.0) (Współczynnik wypełnienia w trybie ręcznym 0.0-1.0)

// Control state variables (Zmienne stanu sterowania)
float P_step    = 0.0f;    // Target step power from AUTO logic (Docelowa moc schodkowa z logiki AUTO)
float P_applied = 0.0f;    // Actually applied power after filtering (Faktycznie przykładana moc po filtrowaniu)
int   on_cycles = 0;       // Number of half-cycles SSR should be ON (Liczba półokresów gdy SSR powinien być WŁĄCZONY)
int   phaseIndex = 0;      // Current half-cycle index in window (Aktualny indeks półokresu w oknie)
unsigned long lastHalfMillis = 0;  // Timestamp for half-cycle timing (Znacznik czasu dla taktowania półokresów)

// Initialize SSR control system (Inicjalizacja systemu sterowania SSR)
void setupSSR(){
  pinMode(SSR_PIN, OUTPUT);         // Configure SSR pin as output (Skonfiguruj pin SSR jako wyjście)
  digitalWrite(SSR_PIN, LOW);       // Start with SSR OFF (Zacznij z SSR WYŁĄCZONYM)
  phaseIndex = 0;                   // Reset phase index (Zresetuj indeks fazy)
  lastHalfMillis = millis();        // Initialize timing (Zainicjalizuj taktowanie)
  P_applied = 0.0f;                 // Reset applied power (Zresetuj przykładaną moc)
  P_step = 0.0f;                    // Reset step power (Zresetuj moc schodkową)
  
  // Initialize averaging buffer (Inicjalizacja bufora uśredniania)
  for(int i = 0; i < AVG_BUFFER_SIZE; i++){
    exportBuffer[i] = 0.0f;
  }
}

// Update target power based on average export (AUTO mode) (Aktualizuj moc docelową na podstawie średniego eksportu - tryb AUTO)
void updateStepFromExport(){
  if (!autoMode) return;  // Only operate in AUTO mode (Działaj tylko w trybie AUTO)
  
  // Make decisions only every 10 seconds (Podejmuj decyzje tylko co 10 sekund)
  unsigned long now = millis();
  if (now - lastDecision < DECISION_INTERVAL_MS) return;
  lastDecision = now;
  
  // Use average from last 10 measurements (Używaj średniej z ostatnich 10 pomiarów)
  float avgExport = getAverageExport();
  
  // Hysteresis logic for stability (Logika histerezy dla stabilności)
  if (avgExport < EXPORT_LOW) {
    // Too little export - decrease heater power (Za mało eksportu - zmniejsz moc grzałki)
    if (P_step >= STEP_W) {
      P_step -= STEP_W;
      Serial.printf("Decreasing power by %.0f W (avg export: %.1f W) [Zmniejszam moc o %.0f W (śr. eksport: %.1f W)]\n", STEP_W, avgExport, STEP_W, avgExport);
    } else {
      P_step = 0.0f;
    }
  }
  else if (avgExport >= EXPORT_HIGH) {
    // High export - increase heater power (Duży eksport - zwiększ moc grzałki)
    float newStep = P_step + STEP_W;
    if (newStep <= P_MAX) {
      P_step = newStep;
      Serial.printf("Increasing power by %.0f W (avg export: %.1f W) [Zwiększam moc o %.0f W (śr. eksport: %.1f W)]\n", STEP_W, avgExport, STEP_W, avgExport);
    } else {
      P_step = P_MAX;
    }
  }
  // Band 50-249W -> no change (wide hysteresis for stability) (Pasmo 50-249W -> bez zmian, szeroka histereza dla stabilności)
}

// Compute target power based on current mode (Oblicz moc docelową na podstawie aktualnego trybu)
float computeTargetPower(){
  if (autoMode) {
    // AUTO mode: use step power with limits (Tryb AUTO: użyj mocy schodkowej z ograniczeniami)
    if (P_step < 0) P_step = 0;        // Minimum limit (Ograniczenie dolne)
    if (P_step > P_MAX) P_step = P_MAX; // Maximum limit (Ograniczenie górne)
    return P_step;
  } else {
    // MANUAL mode: use manual duty cycle (Tryb RĘCZNY: użyj ręcznego współczynnika wypełnienia)
    float P_target = constrain(manualDuty, 0.0f, 1.0f) * P_MAX;
    return P_target;
  }
}

// Update control window with smooth transitions (Aktualizuj okno sterowania z płynnymi przejściami)
void updateControlWindow(){
  float P_target = computeTargetPower();
  
  // Smooth transitions in both directions (Płynne przejścia w obu kierunkach)
  float k;
  if (P_target < P_applied) {
    // Decreasing - slower transition (Zmniejszanie - wolniejsze przejście)
    k = (float)WINDOW_SECONDS / (TAU_DOWN + (float)WINDOW_SECONDS);
  } else {
    // Increasing - gentle approach (Zwiększanie - łagodne dochodzenie)
    k = (float)WINDOW_SECONDS / (TAU_UP + (float)WINDOW_SECONDS);
  }
  
  // Limit filter coefficient (Ogranicz współczynnik filtra)
  if (k < 0.0f) k = 0.0f; 
  if (k > 1.0f) k = 1.0f;
  
  // Apply exponential moving average filter (Zastosuj filtr wykładniczej średniej ruchomej)
  P_applied = P_applied + k * (P_target - P_applied);
  
  // Apply power limits (Zastosuj ograniczenia mocy)
  if (P_applied < 0.0f) P_applied = 0.0f;
  if (P_applied > P_MAX) P_applied = P_MAX;
  
  // Convert power to duty cycle -> number of half-cycles in window (Konwertuj moc na współczynnik wypełnienia -> liczba półokresów w oknie)
  float duty = P_applied / P_MAX;
  on_cycles = (int)round(duty * N_HALF);
  if (on_cycles < 0) on_cycles = 0;
  if (on_cycles > N_HALF) on_cycles = N_HALF;
}

// Half-cycle timing for burst-fire SSR control (Taktowanie półokresów dla sterowania SSR typu burst-fire)
void halfCycleTick(){
  const unsigned long HALF_MS = 10;  // 10ms = one half-cycle at 50Hz (10ms = jeden półokres przy 50Hz)
  unsigned long now = millis();
  
  if (now - lastHalfMillis >= HALF_MS){  // Time for next half-cycle (Czas na następny półokres)
    lastHalfMillis += HALF_MS;
    
    if (phaseIndex == 0) updateControlWindow();  // Update control at start of each window (Aktualizuj sterowanie na początku każdego okna)
    
    // Turn SSR ON or OFF based on current phase (Włącz lub wyłącz SSR na podstawie aktualnej fazy)
    if (phaseIndex < on_cycles) digitalWrite(SSR_PIN, HIGH);  // SSR ON (SSR WŁĄCZONY)
    else                        digitalWrite(SSR_PIN, LOW);   // SSR OFF (SSR WYŁĄCZONY)
    
    phaseIndex++;  // Advance to next half-cycle (Przejdź do następnego półokresu)
    if (phaseIndex >= N_HALF) phaseIndex = 0;  // Reset at end of window (Zresetuj na końcu okna)
  }
}

//////////////////// Web Interface (Interfejs WWW) ////////////////////
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

// Serve main web page (Obsłuż główną stronę WWW)
void handleRoot(){ server.send(200,"text/html; charset=utf-8", htmlPage()); }

// Handle data API endpoint - provides current measurements for charts/UI (Obsłuż punkt końcowy API danych - dostarcza aktualne pomiary dla wykresów/UI)
void handleData(){
  float grid_import_W = md.Pt > 0 ? md.Pt : 0.0f;   // Positive power = import from grid (Dodatnia moc = pobór z sieci)
  float export_W      = md.Pt < 0 ? -md.Pt : 0.0f;  // Negative power = export to grid (Ujemna moc = eksport do sieci)
  float avg_export_W  = getAverageExport();          // Rolling average export (Średni eksport kroczący)
  
  char buf[300];
  snprintf(buf, sizeof(buf),
    "{\"grid_import_W\":%.1f,\"export_W\":%.1f,\"heater_W\":%.1f,\"avg_export_W\":%.1f}",
    grid_import_W, export_W, P_applied, avg_export_W
  );
  server.send(200,"application/json; charset=utf-8", buf);
}

// Handle control status API endpoint (for UI) (Obsłuż punkt końcowy API stanu sterowania dla UI)
void handleCtrlJSON(){
  float P_target = computeTargetPower();     // Current target power (Aktualna moc docelowa)
  float manual_pct = manualDuty * 100.0f;    // Manual duty cycle as percentage (Ręczny współczynnik wypełnienia jako procent)
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

// Handle settings from UI (Obsłuż ustawienia z UI)
void handleSet(){
  if (server.hasArg("mode")){  // Mode change request (Żądanie zmiany trybu)
    String m = server.arg("mode");
    if (m == "auto")   autoMode = true;   // Switch to AUTO mode (Przełącz na tryb AUTO)
    if (m == "manual") autoMode = false;  // Switch to MANUAL mode (Przełącz na tryb RĘCZNY)
  }
  if (server.hasArg("duty")){  // Manual duty cycle change (Zmiana ręcznego współczynnika wypełnienia)
    int pct = server.arg("duty").toInt();
    if (pct < 0) pct = 0;      // Minimum 0% (Minimum 0%)
    if (pct > 100) pct = 100;  // Maximum 100% (Maksimum 100%)
    manualDuty = pct / 100.0f; // Convert percentage to 0.0-1.0 range (Konwertuj procent na zakres 0.0-1.0)
  }
  handleCtrlJSON();  // Return updated control status (Zwróć zaktualizowany stan sterowania)
}

//////////////////// Main Setup and Loop (Główna Inicjalizacja i Pętla) ////////////////////
// Main system initialization (Główna inicjalizacja systemu)
void setup(){
  Serial.begin(115200);  // Initialize serial communication (Zainicjalizuj komunikację szeregowy)
  delay(200);
  
  // Configure RS485 direction control pin (Skonfiguruj pin sterowania kierunkiem RS485)
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);  // Start in receive mode (Zacznij w trybie odbioru)
  
  // Initialize RS485/Modbus communication (Zainicjalizuj komunikację RS485/Modbus)
  RS485.begin(UART_BAUD, SERIAL_MODE, 16, 17);  // RX=16, TX=17
  RS485.setTimeout(500);
  node.begin(METER_ID, RS485);
  node.preTransmission(preTransmission);   // Set transmit callback (Ustaw callback nadawania)
  node.postTransmission(postTransmission); // Set receive callback (Ustaw callback odbioru)
  
  // Initialize Wi-Fi connection (Zainicjalizuj połączenie Wi-Fi)
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns1, dns2);  // Configure static IP (Skonfiguruj statyczne IP)
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);  // Wait max 15 seconds (Czekaj maksymalnie 15 sekund)
  
  // Print system information (Wyświetl informacje o systemie)
  Serial.println("\n=== ESP32 HEATER CONTROL SYSTEM (STABILNE STEROWANIE GRZAŁKĄ) ===");
  Serial.print("IP Address (Adres IP): ");
  Serial.println(WiFi.localIP());
  Serial.println("System Parameters (Parametry systemu):");
  Serial.println("- Averaging: 10 last measurements (Uśrednianie: 10 ostatnich pomiarów)");
  Serial.println("- Decisions every: 10 seconds (Decyzje co: 10 sekund)");
  Serial.println("- Power step: ±50W (Krok mocy: ±50W)");
  Serial.println("- Hysteresis: 50-250W export (Histereza: 50-250W eksportu)");
  
  // Setup HTTP server endpoints (Skonfiguruj punkty końcowe serwera HTTP)
  server.on("/", handleRoot);           // Main web page (Główna strona WWW)
  server.on("/data.json", handleData);  // Real-time data API (API danych w czasie rzeczywistym)
  server.on("/ctrl.json", handleCtrlJSON); // Control status API (API stanu sterowania)
  server.on("/set", handleSet);         // Settings API (API ustawień)
  server.begin();                       // Start web server (Uruchom serwer WWW)
  
  // Initialize SSR control system (Zainicjalizuj system sterowania SSR)
  setupSSR();
  
  // Initial system startup (Początkowe uruchomienie systemu)
  pollMeter();              // Get first power reading (Wykonaj pierwszy odczyt mocy)
  updateStepFromExport();   // Initialize AUTO logic (Zainicjalizuj logikę AUTO)
  updateControlWindow();    // Initialize control window (Zainicjalizuj okno sterowania)
}

// Main program loop (Główna pętla programu)
void loop(){
  server.handleClient();  // Handle web server requests (Obsłużuj żądania serwera WWW)
  
  // Poll energy meter every 1 second (for averaging buffer) (Odpytuj licznik energii co 1 sekundę dla bufora uśredniania)
  if (millis() - lastPoll > 1000) {
    lastPoll = millis();
    pollMeter();              // Read current power from meter (Odczytaj aktualną moc z licznika)
    updateStepFromExport();   // Check if 10s passed since last decision (Sprawdź czy minęło 10s od ostatniej decyzji)
  }
  
  // Burst-fire SSR control timing (10ms half-cycles) (Taktowanie sterowania SSR burst-fire, półokresy 10ms)
  halfCycleTick();
}