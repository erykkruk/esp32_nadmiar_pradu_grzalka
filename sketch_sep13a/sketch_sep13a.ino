/*
  ESP32 + DTSU666 (RS485/Modbus RTU) -> mini WWW z odczytami (typy + skale OK)
  - WiFi: TP-Link_E6D1 / 80246459
  - Statyczny IP: 192.168.255.50
  - UART RS485: Serial2  (RX=16, TX=17)
  - DE/RE: GPIO4
  - Modbus: 9600 bps, O-8-1, slave id = 1
  Biblioteka: ModbusMaster (Doc Walker)
  
  POPRAWKA: Moce dzielone przez 10 (DTSU666 zwraca w 0.1W)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>

//////////////// Wi-Fi ////////////////
const char* WIFI_SSID = "TP-Link_E6D1";
const char* WIFI_PASS = "80246459";
IPAddress local_IP(192,168,255,50), gateway(192,168,255,1),
         subnet(255,255,255,0), dns1(192,168,255,1), dns2(8,8,8,8);

////////////// RS485 / Modbus //////////
#define RS485_DE_RE 4
#define UART_BAUD   9600
#define SERIAL_MODE SERIAL_8O1
const uint8_t  METER_ID = 1;

HardwareSerial& RS485 = Serial2;
ModbusMaster node;
WebServer server(80);

void preTransmission(){ digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission(){ digitalWrite(RS485_DE_RE, LOW); }

////////////// Helpers /////////////////
bool readFloat32(uint16_t reg, float& outVal) {
  for (int i=0;i<2;i++){
    uint8_t r = node.readHoldingRegisters(reg, 2);
    if (r == node.ku8MBSuccess){
      uint16_t hi = node.getResponseBuffer(0);
      uint16_t lo = node.getResponseBuffer(1);
      uint32_t raw = ((uint32_t)hi << 16) | lo; // w razie „kosmosu" zamień na (lo<<16)|hi
      memcpy(&outVal, &raw, sizeof(float));
      return true;
    }
    delay(5);
  }
  return false;
}

bool readFloat32Scaled(uint16_t reg, float scale, float& outVal) {
  float rawVal;
  if (readFloat32(reg, rawVal)) {
    outVal = rawVal / scale;
    return true;
  }
  return false;
}

bool readInt16Scaled(uint16_t reg, float scale, float& outVal){
  for (int i=0;i<2;i++){
    uint8_t r = node.readHoldingRegisters(reg, 1);
    if (r == node.ku8MBSuccess){
      int16_t raw = (int16_t)node.getResponseBuffer(0);
      outVal = raw / scale;
      return true;
    }
    delay(5);
  }
  return false;
}
inline void setIfOk(bool ok, float v, float& dst){ if(ok) dst = v; }

////////////// Dane /////////////////
struct MeterData {
  // skorygowane jednostki: V, A, W, Hz, kWh, PF
  float Ua=0, Ub=0, Uc=0;
  float Ia=0, Ib=0, Ic=0;
  float Pt=0, Pa=0, Pb=0, Pc=0;
  float PFt=0;
  float Freq=0;
  float ImpEp_kWh=0, ExpEp_kWh=0;
  unsigned long lastOkMs=0;
} md;

unsigned long lastPoll=0;
void smallPause(){ delay(8); }

void pollMeter(){
  float v;

  // INT16 + skale - NAPIĘCIA
  setIfOk(readInt16Scaled(0x2006, 10.0,   v), v, md.Ua); smallPause(); // U L1 [V]
  setIfOk(readInt16Scaled(0x2008, 10.0,   v), v, md.Ub); smallPause(); // U L2
  setIfOk(readInt16Scaled(0x200A, 10.0,   v), v, md.Uc); smallPause(); // U L3

  // INT16 + skale - PRĄDY
  setIfOk(readInt16Scaled(0x200C, 1000.0, v), v, md.Ia); smallPause(); // I L1 [A]
  setIfOk(readInt16Scaled(0x200E, 1000.0, v), v, md.Ib); smallPause(); // I L2
  setIfOk(readInt16Scaled(0x2010, 1000.0, v), v, md.Ic); smallPause(); // I L3

  // MOCE – FLOAT32 - DTSU666 zwraca w 0.1W, więc dzielimy przez 10
  setIfOk(readFloat32Scaled(0x2012, 10.0, v), v, md.Pt); smallPause(); // P Σ [W]
  setIfOk(readFloat32Scaled(0x2014, 10.0, v), v, md.Pa); smallPause(); // P L1 [W]
  setIfOk(readFloat32Scaled(0x2016, 10.0, v), v, md.Pb); smallPause(); // P L2 [W]
  setIfOk(readFloat32Scaled(0x2018, 10.0, v), v, md.Pc); smallPause(); // P L3 [W]

  // PF – INT16 /1000
  if (readInt16Scaled(0x202A, 1000.0, v)) { md.PFt = constrain(v, -1.0f, 1.0f); }
  smallPause();

  // Freq – INT16 /100
  setIfOk(readInt16Scaled(0x2044, 100.0, v), v, md.Freq); smallPause();

  // Energie – FLOAT32 (kWh)
  setIfOk(readFloat32(0x101E, v), v, md.ImpEp_kWh); smallPause(); // import
  setIfOk(readFloat32(0x1028, v), v, md.ExpEp_kWh);               // export

  md.lastOkMs = millis();
}

////////////// WWW /////////////////
String htmlPage(){
  String s = R"HTML(
<!doctype html><html lang="pl"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DTSU666 – odczyty</title>
<style>
:root{ --ok:#0ea37a; --bad:#d64545; --card:#fff; --b:#e5e7eb; }
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px;background:#fafafa}
h1{margin:0 0 8px}
.status{border-radius:16px;padding:14px 16px;margin:6px 0 14px;color:#fff;font-weight:600;display:flex;justify-content:space-between;align-items:center}
.mono{font-variant-numeric:tabular-nums}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--b);border-radius:14px;padding:12px}
.kv{display:flex;justify-content:space-between;margin:2px 0}
small{opacity:.6}
footer{margin-top:12px;opacity:.65}
.big{font-size:22px}
</style>
<h1>DTSU666 – odczyty</h1>
<div id="status" class="status" style="background:gray">
  <span id="statusText">Ładowanie…</span>
  <span class="mono big" id="statusPower">– W</span>
</div>
<div class="grid" id="grid"></div>
<footer><small>Odświeżanie co 2 s • ESP32</small></footer>
<script>
function fmt(v,d=2){return Number(v).toFixed(d)}
function render(j){
  const sEl=document.getElementById('status');
  const tEl=document.getElementById('statusText');
  const pEl=document.getElementById('statusPower');
  const pt=Number(j.Pt); 
  const dead=1; // Próg martwej strefy w watach (teraz prawidłowy)
  
  if(pt<-dead){ 
    sEl.style.background='var(--ok)'; 
    tEl.textContent='Nadwyżka prądu (oddajesz do sieci)'; 
  }
  else if(pt>dead){ 
    sEl.style.background='var(--bad)'; 
    tEl.textContent='Pobierany prąd (pobór z sieci)'; 
  }
  else{ 
    sEl.style.background='gray'; 
    tEl.textContent='Bilans ~0 W'; 
  }
  
  pEl.textContent=(pt<0?'':'+') + fmt(pt,1) + ' W';

  const g=document.getElementById('grid');
  const S=(k,v,u='')=>`<div class="card"><div class="kv"><b>${k}</b><span class="mono">${v} ${u}</span></div></div>`;
  g.innerHTML =
    S('Napięcie L1', fmt(j.Ua,1),'V') +
    S('Napięcie L2', fmt(j.Ub,1),'V') +
    S('Napięcie L3', fmt(j.Uc,1),'V') +
    S('Prąd L1', fmt(j.Ia,3),'A') +
    S('Prąd L2', fmt(j.Ib,3),'A') +
    S('Prąd L3', fmt(j.Ic,3),'A') +
    S('Moc chwilowa Σ', fmt(j.Pt,1),'W') +
    S('Moc L1', fmt(j.Pa,1),'W') +
    S('Moc L2', fmt(j.Pb,1),'W') +
    S('Moc L3', fmt(j.Pc,1),'W') +
    S('Cos φ (Σ)', fmt(j.PFt,3)) +
    S('Częstotliwość', fmt(j.Freq,2),'Hz') +
    S('Energia import', fmt(j.ImpEp_kWh,3),'kWh') +
    S('Energia export', fmt(j.ExpEp_kWh,3),'kWh') +
    `<div class="card"><div class="kv"><b>Ostatnia aktualizacja</b><span class="mono">${new Date().toLocaleTimeString()}</span></div></div>`;
}
async function tick(){
  try{ const r=await fetch('/data.json'); const j=await r.json(); render(j); }
  catch(e){ document.getElementById('statusText').textContent='Błąd pobierania danych…'; }
}
setInterval(tick,2000); tick();
</script>
</html>
)HTML";
  return s;
}

void handleRoot(){ server.send(200,"text/html; charset=utf-8", htmlPage()); }

void handleJSON(){
  char buf[1024];
  snprintf(buf, sizeof(buf),
    "{\"Ua\":%.3f,\"Ub\":%.3f,\"Uc\":%.3f,"
    "\"Ia\":%.6f,\"Ib\":%.6f,\"Ic\":%.6f,"
    "\"Pt\":%.3f,\"Pa\":%.3f,\"Pb\":%.3f,\"Pc\":%.3f,"
    "\"PFt\":%.6f,\"Freq\":%.3f,"
    "\"ImpEp_kWh\":%.6f,\"ExpEp_kWh\":%.6f}",
    md.Ua,md.Ub,md.Uc,
    md.Ia,md.Ib,md.Ic,
    md.Pt,md.Pa,md.Pb,md.Pc,
    md.PFt,md.Freq,
    md.ImpEp_kWh, md.ExpEp_kWh
  );
  server.send(200,"application/json; charset=utf-8", buf);
}

void setup(){
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);

  RS485.begin(UART_BAUD, SERIAL_MODE, 16, 17);
  RS485.setTimeout(500);
  node.begin(METER_ID, RS485);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns1, dns2);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  server.on("/", handleRoot);
  server.on("/data.json", handleJSON);
  server.begin();

  pollMeter();
}

void loop(){
  server.handleClient();
  if (millis() - lastPoll > 2000) { lastPoll = millis(); pollMeter(); }
}