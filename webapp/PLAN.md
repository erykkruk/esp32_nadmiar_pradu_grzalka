# Plan migracji: ESP32 monolith -> Next.js + ESP8266

## Co robi obecny program (w skrócie)

ESP32 robi **wszystko** w jednym pliku:

1. **Czyta moc z licznika** DTSU666 przez Modbus RS485 (co 1s)
2. **Steruje grzałką** przez SSR (burst-fire, 100 półokresów/s)
3. **Logika AUTO** - analizuje nadwyżkę eksportu solarnego i reguluje moc grzałki (histereza 50-250W, krok ±50W, decyzje co 10s, EMA filtr)
4. **Serwer WWW** - strona z wykresami, przełącznik AUTO/MANUAL, slider mocy

---

## Nowa architektura: ESP pyta apkę

Kluczowa zmiana: **ESP8266 inicjuje komunikację**. Co 1s wysyła POST do Next.js z odczytem mocy i dostaje w odpowiedzi co ma robić (duty%).

Zalety:
- Nie trzeba znać IP ESP (może być za NATem)
- ESP sam decyduje kiedy pytać
- Jeden request = wysyłka danych + odebranie komendy
- Prostsze, mniej endpointów

### ESP8266 (firmware) - "głupi" executor

ESP8266 robi **tylko** hardware. Co 1s:
1. Czyta moc z DTSU666 (Modbus)
2. Wysyła POST do Next.js z odczytem
3. Dostaje w odpowiedzi duty% do ustawienia na SSR
4. Jeśli nie dostanie odpowiedzi - safety: ramping down + SAFE MODE (patrz sekcja Safety)

**ESP8266 wysyła co 1s:**

```
POST https://<NEXTJS_URL>/api/esp/report
Authorization: Bearer <TOKEN>
Content-Type: application/json

{
  "power_w": -245.3,        // odczyt z licznika (- = eksport, + = import)
  "uptime_s": 3600,         // czas działania ESP
  "wifi_rssi": -52,         // siła sygnału WiFi
  "free_heap": 28000,       // wolna pamięć
  "current_duty_pct": 35.0  // aktualnie ustawiony duty na SSR
}
```

**Next.js odpowiada:**

```json
{
  "duty_pct": 40.0,         // nowy duty do ustawienia (0-100)
  "mode": "auto",           // tryb: auto/manual (informacyjnie)
  "ack": true               // potwierdzenie odebrania
}
```

ESP ustawia `duty_pct` na SSR i tyle. Zero myślenia.

---

## Safety: co ESP robi gdy serwer nie odpowiada

ESP8266 ma wbudowany **mechanizm bezpieczeństwa** - nie zależy od serwera żeby się wyłączyć.

### Stany ESP

```
NORMAL  ──(brak odpowiedzi 10s)──>  RAMP_DOWN  ──(duty spadł do 0%)──>  SAFE_MODE
   ^                                                                        |
   └────────────────(serwer odpowiedział)───────────────────────────────────┘
```

### Zmienne safety na ESP

```cpp
unsigned long lastResponseMs = 0;    // kiedy serwer ostatnio odpowiedział
float         currentDuty    = 0.0;  // aktualny duty% na SSR
bool          safeMode       = false; // flaga trybu awaryjnego
```

### Logika safety (w loop(), co 1s)

```
1. Wyślij POST do serwera z odczytem mocy
2. Jeśli odpowiedź OK:
   - lastResponseMs = now
   - safeMode = false
   - currentDuty = odpowiedź.duty_pct
   - ustaw SSR na currentDuty
3. Jeśli BRAK odpowiedzi (timeout / błąd HTTP / WiFi down):
   - NIE aktualizuj lastResponseMs
   - sprawdź ile czasu minęło od ostatniej odpowiedzi:
```

### Tabela reakcji

| Czas bez odpowiedzi | Stan | Reakcja ESP | LED |
|---|---|---|---|
| 0 - 10s | `NORMAL` | Trzyma ostatni duty (normalne opóźnienie sieci) | Ciągłe świecenie |
| 10 - 30s | `RAMP_DOWN` | Co 1s zmniejsza duty o 5% (łagodne wygaszanie) | Wolne mruganie (1Hz) |
| 30s+ | `SAFE_MODE` | duty = 0%, SSR OFF, czeka na serwer | Szybkie mruganie (4Hz) |
| Serwer wraca | `NORMAL` | Przyjmuje nowy duty z serwera, wraca do pracy | Ciągłe świecenie |

### Pseudokod safety

```cpp
const unsigned long GRACE_PERIOD_MS    = 10000;  // 10s - normalne opóźnienie
const unsigned long RAMP_DOWN_START_MS = 10000;  // po 10s zaczynaj ramping
const float         RAMP_STEP          = 5.0;    // -5% co sekundę
const float         SAFE_MODE_DUTY     = 0.0;    // w safe mode = wyłączony

void checkSafety() {
  unsigned long elapsed = millis() - lastResponseMs;

  if (elapsed < GRACE_PERIOD_MS) {
    // NORMAL - wszystko OK
    safeMode = false;
    ledSolid();
  }
  else if (currentDuty > SAFE_MODE_DUTY) {
    // RAMP_DOWN - łagodnie zmniejszaj
    currentDuty -= RAMP_STEP;
    if (currentDuty < 0) currentDuty = 0;
    setSSRDuty(currentDuty);
    ledSlowBlink();
  }
  else {
    // SAFE_MODE - SSR OFF, czekamy
    safeMode = true;
    currentDuty = 0;
    setSSRDuty(0);
    ledFastBlink();
  }
}
```

### Raport safety do serwera

ESP wysyła swój stan w każdym raporcie, żeby dashboard mógł pokazać problem:

```json
{
  "power_w": -245.3,
  "uptime_s": 3600,
  "wifi_rssi": -52,
  "free_heap": 28000,
  "current_duty_pct": 15.0,
  "safe_mode": true,
  "seconds_since_last_response": 25
}
```

### Dlaczego ramping down a nie twarde cięcie?

- Skok z 80% na 0% = nagła zmiana obciążenia w sieci domowej
- Łagodne wygaszanie (5%/s) = grzałka schodzi w ~16s z max mocy
- SSR woli płynne zmiany
- Jeśli serwer wróci po 12s - duty spadł tylko o 10%, szybko wraca do normy

### Next.js (webapp) - cały mózg

| Moduł | Opis |
|-------|------|
| **API: `/api/esp/report`** | Odbiera dane z ESP, przepuszcza przez logikę, zwraca duty% |
| **Logika AUTO** | Brutto eksport (eksport + grzałka), bufor 10 pomiarów, proporcjonalne celowanie (brutto - rezerwa), EMA filtr |
| **Store** | Stan w pamięci serwera: historia pomiarów, tryb, P_step, P_applied |
| **API: `/api/status`** | Dla frontendu: aktualny stan systemu |
| **API: `/api/control`** | Dla frontendu: zmiana trybu AUTO/MANUAL, slider duty |
| **API: `/api/history`** | Dla frontendu: dane historyczne do wykresów |
| **Dashboard UI** | React: wykresy, karty z danymi, tryb AUTO/MANUAL, slider |

---

## Logika serwera - jak to liczy (krok po kroku)

### Kluczowa zmiana vs ESP32

Na ESP32 logika chodziła w `loop()` co 10ms non-stop. Na serwerze **nie ma loop'a** - logika odpala się **przy każdym POST od ESP** (co ~1s). Każdy request = jeden "tick".

### Store (singleton w pamięci serwera)

Serwer trzyma cały stan w jednym obiekcie. Żyje tak długo jak proces Node.js:

```typescript
const store = {
  // --- bufor pomiarów BRUTTO ---
  grossBuffer: number[]        // circular buffer, 10 slotów (brutto = eksport + grzałka)
  bufferIndex: number          // aktualny indeks w buforze (0-9)
  bufferFull: boolean          // czy bufor się zapełnił

  // --- logika AUTO ---
  mode: "auto" | "manual"     // tryb pracy
  pApplied: number            // moc po filtrze EMA [W] (to co realnie idzie na grzałkę)
  lastRequestMs: number       // timestamp ostatniego POST od ESP (do obliczania dt)

  // --- tryb MANUAL ---
  manualDuty: number          // ręczny duty% (0-100) ustawiony z dashboardu

  // --- ostatni raport ESP ---
  lastReport: {
    powerW: number            // ostatni odczyt mocy z licznika
    uptimeS: number
    wifiRssi: number
    freeHeap: number
    currentDutyPct: number    // jaki duty ESP realnie ma na SSR
    safeMode: boolean         // czy ESP jest w safe mode
    receivedAt: number        // kiedy serwer dostał ten raport
  }

  // --- historia do wykresów ---
  history: {
    timestamp: number
    powerW: number
    exportW: number
    grossExportW: number      // eksport brutto (eksport + grzałka)
    heaterW: number
    dutyPct: number
  }[]                         // ostatnie N minut (np. 3600 wpisów = 1h)
}
```

### Problem sprzężenia zwrotnego (dlaczego stary algorytm oscyluje)

```
STARY ALGORYTM (histereza na zmierzonym eksporcie):

Słońce daje nadwyżkę 1000W
  → licznik: eksport 1000W
  → algorytm: 1000 > 250 → +50W grzałki
  → grzałka: 50W
  → licznik: eksport 950W (bo grzałka zjadła 50W!)
  → algorytm: 950 > 250 → +50W
  → grzałka: 100W
  → ... wolno dochodzi do celu, po 50W co 10s = 180s na 900W!

  A jak słońce nagle spadnie:
  → grzałka: 900W, eksport spadł do 20W
  → algorytm: 20 < 50 → -50W
  → grzałka: 850W
  → ... znowu wolno schodzi, może ciągnąć z sieci przez chwilę

Problem: algorytm nie wie ile grzałka zjada, reaguje na SKUTEK swoich decyzji.
```

### Nowy algorytm: BRUTTO EKSPORT

```
Kluczowy wzór:

  BRUTTO EKSPORT = zmierzony eksport + aktualna moc grzałki

To jest REALNA nadwyżka solarna - ile panele produkują ponad zużycie domu.
NIE ZMIENIA SIĘ gdy zmieniamy moc grzałki!

  Cel grzałki = brutto eksport - rezerwa (100W)

Proste. Bezpośrednie. Bez kroków ±50W. Bez histerezy.
```

### Co się dzieje przy każdym POST od ESP

```
ESP wysyła: { power_w: -245.3, current_duty_pct: 35, ... }
                    |
                    v
        ┌─ KROK 1: Weryfikacja tokena ─┐
        │  Bearer token OK?            │
        │  NIE -> 401 Unauthorized     │
        │  TAK -> dalej                │
        └──────────────────────────────┘
                    |
                    v
        ┌─ KROK 2: Oblicz BRUTTO eksport ──────────────┐
        │  power_w = -245.3                            │
        │  measuredExport = 245.3 (bo ujemna=eksport)  │
        │                                              │
        │  ★ KLUCZOWE:                                 │
        │  grossExport = measuredExport + pApplied     │
        │  grossExport = 245.3 + 700 = 945.3 W        │
        │                                              │
        │  To jest REALNA nadwyżka solarna.            │
        │  Nie zależy od grzałki!                      │
        └──────────────────────────────────────────────┘
                    |
                    v
        ┌─ KROK 3: Zapisz brutto do bufora ────────────┐
        │  grossBuffer[bufferIndex] = 945.3            │
        │  bufferIndex = (bufferIndex+1) % 10          │
        │                                              │
        │  Buforujemy BRUTTO, nie surowy eksport!       │
        │  To wygładza szum z licznika.                │
        └──────────────────────────────────────────────┘
                    |
                    v
        ┌─ KROK 4: Sprawdź tryb ──────────────────────┐
        │  mode == "manual"?                           │
        │  TAK -> pTarget = manualDuty% * P_MAX        │
        │         idź do KROK 6                        │
        │  NIE -> tryb AUTO, idź do KROK 5             │
        └──────────────────────────────────────────────┘
                    |
                    v
        ┌─ KROK 5: Oblicz cel (proporcjonalne) ────────┐
        │  Średnia brutto z bufora:                    │
        │  avgGross = suma(grossBuffer) / count        │
        │  avgGross = 945 W                            │
        │                                              │
        │  Cel: zjedz wszystko oprócz rezerwy          │
        │  pTarget = avgGross - EXPORT_RESERVE         │
        │  pTarget = 945 - 100 = 845 W                │
        │                                              │
        │  pTarget = clamp(pTarget, 0, P_MAX)          │
        │                                              │
        │  ★ BEZ histerezy, BEZ kroków ±50W            │
        │  ★ Celuje BEZPOŚREDNIO w prawidłową moc      │
        │  ★ Kompensacja sprzężenia jest wbudowana     │
        └──────────────────────────────────────────────┘
                    |
                    v
        ┌─ KROK 6: Filtr EMA (wygładzanie) ───────────┐
        │  dt = (now - lastRequestMs) / 1000           │
        │  (~1s zwykle, ale mierzymy realny czas)      │
        │                                              │
        │  if pTarget > pApplied:                      │
        │    k = dt / (TAU_UP + dt)    // narastanie   │
        │    (TAU_UP = 8s → k ≈ 0.11 przy dt=1s)      │
        │                                              │
        │  else:                                       │
        │    k = dt / (TAU_DOWN + dt)  // opadanie     │
        │    (TAU_DOWN = 5s → k ≈ 0.17 przy dt=1s)    │
        │                                              │
        │  pApplied = pApplied + k * (pTarget-pApplied)│
        │                                              │
        │  Np. pApplied=700, pTarget=845:              │
        │  pApplied = 700 + 0.11 * (845-700) = 716 W  │
        │  Następna sekunda: 716 + 0.11 * (845-716)    │
        │  = 730W, potem 744, 757... powoli do 845     │
        └──────────────────────────────────────────────┘
                    |
                    v
        ┌─ KROK 7: Oblicz duty% ──────────────────────┐
        │  dutyPct = (pApplied / P_MAX) * 100          │
        │  dutyPct = (716 / 2000) * 100 = 35.8%       │
        │  dutyPct = clamp(dutyPct, 0, 100)            │
        └──────────────────────────────────────────────┘
                    |
                    v
        ┌─ KROK 8: Zapisz historię ────────────────────┐
        │  history.push({                              │
        │    timestamp: now,                           │
        │    powerW: -245.3,       // surowy odczyt    │
        │    exportW: 245.3,       // zmierzony eksp.  │
        │    grossExportW: 945.3,  // brutto eksport   │
        │    heaterW: 716,         // pApplied         │
        │    dutyPct: 35.8                             │
        │  })                                          │
        │  if history.length > 3600: history.shift()   │
        └──────────────────────────────────────────────┘
                    |
                    v
        ┌─ KROK 9: Odpowiedz do ESP ──────────────────┐
        │  return { duty_pct: 35.8, mode: "auto",     │
        │           ack: true }                        │
        └──────────────────────────────────────────────┘
```

### Przykład liczbowy - NOWY vs STARY

Scenariusz: panele dają 1200W nadwyżki, grzałka startuje z 0.

```
STARY (histereza ±50W co 10s):
───────────────────────────────────────────────────────────
  0s   eksport=1200W  → czeka...           grzałka=0W
 10s   eksport=1200W  → 1200>250 → +50W   grzałka=50W
 20s   eksport=1150W  → 1150>250 → +50W   grzałka=100W
 30s   eksport=1100W  → 1100>250 → +50W   grzałka=150W
 ...
220s   eksport=100W   → 50<100<250 → OK   grzałka=1100W   ← 3.5 MINUTY!
 ...po EMA jeszcze dłużej...

NOWY (brutto eksport, proporcjonalne):
───────────────────────────────────────────────────────────
  0s   eksport=1200W, grzałka=0W
       brutto = 1200 + 0 = 1200W
       cel = 1200 - 100 = 1100W
       EMA: 0 + 0.11*(1100-0) = 121W      grzałka=121W
  1s   eksport=1079W, grzałka=121W
       brutto = 1079 + 121 = 1200W  ← TAKA SAMA! bo brutto nie zależy od grzałki
       cel = 1100W
       EMA: 121 + 0.11*(1100-121) = 229W  grzałka=229W
  2s   brutto = 1200W, cel = 1100W
       EMA: 229 + 0.11*(1100-229) = 325W  grzałka=325W
  ...
  8s   EMA: ~750W                          grzałka=750W
 16s   EMA: ~1000W                         grzałka=1000W
 24s   EMA: ~1080W                         grzałka=1080W    ← 24s zamiast 220s!
 30s   EMA: ~1095W                         grzałka=1095W    ← stabilne, 105W rezerwy
```

**Różnica:** stary potrzebował ~220s, nowy ~24s. I nowy jest stabilny od razu.

### Dlaczego brutto eksport rozwiązuje problem

```
Zmierzony eksport = produkcja_solarna - zużycie_domu - moc_grzałki
                    ─────────────────   ────────────   ───────────
                    zmienia się wolno   zmienia się    my to kontrolujemy
                    (chmury)            (dom)

Brutto eksport    = zmierzony eksport + moc_grzałki
                  = produkcja_solarna - zużycie_domu
                    ─────────────────   ────────────
                    NIEZALEŻNE OD GRZAŁKI!

Czyli: brutto zmienia się TYLKO gdy zmienia się słońce lub zużycie domu.
Grzałka NIE wpływa na brutto. Zero sprzężenia zwrotnego.
```

### Filtr EMA - po ludzku

EMA (Exponential Moving Average) to po prostu: "nie skacz od razu do celu, idź tam powoli".

```
pApplied = pApplied + k * (pTarget - pApplied)
```

- `k` jest mały (np. 0.11 przy dt=1s, TAU=8s) = wolna zmiana
- `k` bliski 1 = szybka zmiana
- TAU_UP = 8s = narastanie zajmuje ~8 sekund (żeby nie wpierdolić nagle 2kW)
- TAU_DOWN = 5s = opadanie trochę szybsze (lepiej szybciej wyłączyć niż włączyć)

### Ważna różnica: czas na serwerze

Na ESP32 `dt` zawsze = 1s (bo `WINDOW_SECONDS = 1`).
Na serwerze `dt` liczymy z rzeczywistego czasu między requestami:

```typescript
const now = Date.now()
const dt = (now - lastRequestMs) / 1000  // sekundy od ostatniego POST
lastRequestMs = now

// dt zwykle ~1s, ale może być 2-3s jeśli ESP miał problem z WiFi
// filtr EMA automatycznie to kompensuje bo używa dt w obliczeniach
```

To jest lepsze niż ESP32 - tam zakładał sztywno 1s, tu mierzymy realny czas.

### Tryb MANUAL

Kiedy user kliknie "MANUAL" na dashboardzie i ustawi slider na 60%:

```
1. POST /api/control  { mode: "manual", duty: 60 }
2. store.mode = "manual"
3. store.manualDuty = 60

Następny POST od ESP:
4. KROK 4: mode == "manual"
5. pTarget = 60% * 2000W = 1200W
6. pApplied = 1200W (natychmiast, bez EMA)
7. dutyPct = 60%
8. Odpowiedź: { duty_pct: 60 }
```

W trybie MANUAL nie ma bufora ani EMA - user mówi 60% i tyle.

### Przełączenie z MANUAL na AUTO

```
1. POST /api/control  { mode: "auto" }
2. store.mode = "auto"
3. pApplied zostaje jaki był (bez skoku!)
4. Bufor brutto napełnia się od nowa
5. EMA powoli dociąga do prawidłowego celu
```

---

## Przepływ danych

```
[DTSU666] --Modbus--> [ESP8266]
                          |
                    co 1s POST /api/esp/report
                    { power_w, uptime, rssi, duty, safe_mode }
                          |
                          v
                    [Next.js Server]
                          |
                   ┌──────┴──────┐
                   │  store.ts   │ (stan w pamięci)
                   │  singleton  │
                   └──────┬──────┘
                          |
                   heater-logic.ts
                   1. bufor → 2. średnia →
                   3. histereza → 4. EMA →
                   5. duty%
                          |
                    odpowiedź JSON
                    { duty_pct: 40.0 }
                          |
                          v
                    [ESP8266]
                    ustawia duty na SSR
                          |
                    burst-fire SSR
                          |
                    [GRZAŁKA]

[Next.js Server] --polling 1s--> [Browser Dashboard]
                    GET /api/status
                    (czyta z tego samego store)
```

---

## Struktura projektu Next.js

```
webapp/
├── src/
│   ├── app/
│   │   ├── page.tsx                  # Dashboard (główna strona)
│   │   ├── layout.tsx
│   │   └── api/
│   │       ├── esp/
│   │       │   └── report/route.ts   # POST: ESP wysyła dane, dostaje duty%
│   │       ├── status/route.ts       # GET: stan dla frontendu
│   │       ├── control/route.ts      # POST: zmiana trybu/duty z UI
│   │       └── history/route.ts      # GET: dane historyczne
│   ├── lib/
│   │   ├── heater-logic.ts           # *** CAŁA LOGIKA AUTO ***
│   │   │   - brutto eksport (eksport + grzałka)
│   │   │   - bufor brutto (circular buffer 10 próbek)
│   │   │   - proporcjonalne celowanie (brutto - rezerwa)
│   │   │   - filtr EMA (TAU_UP=8s / TAU_DOWN=5s)
│   │   │   - obliczanie duty cycle
│   │   ├── constants.ts              # P_MAX, STEP_W, EXPORT_LOW/HIGH, itp.
│   │   └── store.ts                  # Stan w pamięci serwera (singleton)
│   ├── components/
│   │   ├── Dashboard.tsx             # Główny layout
│   │   ├── PowerChart.tsx            # Wykres mocy grzałki
│   │   ├── ExportChart.tsx           # Wykres eksportu
│   │   ├── StatusCards.tsx           # Karty: import, eksport, moc grzałki
│   │   ├── ModeControl.tsx           # Przełącznik AUTO/MANUAL + slider
│   │   └── StabilizationInfo.tsx     # Badge'e z parametrami
│   └── types/
│       └── index.ts                  # Typy: EspReport, ControlState, itp.
├── .env.local                        # ESP_TOKEN
├── next.config.ts
├── package.json
└── tsconfig.json
```

---

## Co zostaje na ESP8266, a co idzie do Next.js

| Funkcja | ESP8266 | Next.js |
|---------|---------|---------|
| Odczyt Modbus (DTSU666) | **TAK** | nie |
| Burst-fire SSR (halfCycleTick) | **TAK** | nie |
| Wysyłanie raportu co 1s | **TAK** | nie |
| Safety: grace period 10s (trzyma ostatni duty) | **TAK** | nie |
| Safety: ramping down -5%/s (po 10s bez odpowiedzi) | **TAK** | nie |
| Safety: SAFE MODE duty=0% (po 30s bez odpowiedzi) | **TAK** | nie |
| Safety: powrót do NORMAL po reconnect | **TAK** | nie |
| LED sygnalizacja stanu (solid/slow/fast blink) | **TAK** | nie |
| Auth token (wysyłanie) | **TAK** | nie |
| Auth token (weryfikacja) | nie | **TAK** |
| Bufor 10 pomiarów BRUTTO | nie | **TAK** |
| Średnia krocząca brutto | nie | **TAK** |
| Logika AUTO (brutto - rezerwa, proporcjonalne) | nie | **TAK** |
| Filtr EMA | nie | **TAK** |
| Obliczanie duty% | nie | **TAK** |
| Web UI (dashboard) | nie | **TAK** |
| Historia wykresów | nie | **TAK** |

---

## Kolejność implementacji

### Faza 1: Scaffolding Next.js
1. `npx create-next-app@latest` (App Router, TypeScript, Tailwind)
2. Struktura folderów jak wyżej
3. `.env.local` z `ESP_TOKEN`
4. Typy TypeScript (`types/index.ts`)
5. Stałe (`lib/constants.ts`)

### Faza 2: Logika serwerowa
1. `heater-logic.ts` - NOWY algorytm (brutto eksport):
   - `CircularBuffer` class (bufor brutto eksportu)
   - `calculateGrossExport(measuredExport, pApplied)` → brutto
   - `calculateTarget(avgGross)` → brutto - rezerwa (proporcjonalne)
   - `applyEmaFilter(pApplied, pTarget, dt)` → wygładzone przejście
   - `calculateDutyCycle(pApplied)` → duty%
   - `processReport(powerW)` → główna funkcja: cały pipeline
2. `store.ts` - stan w pamięci (singleton): tryb, pApplied, bufor brutto, historia
3. `POST /api/esp/report` - główny endpoint:
   - weryfikuj token
   - odbierz power_w z ESP
   - przepuść przez logikę
   - zwróć duty_pct

### Faza 3: API Routes dla frontendu
1. `GET /api/status` - aktualny stan systemu
2. `POST /api/control` - zmiana trybu AUTO/MANUAL, duty
3. `GET /api/history` - dane do wykresów

### Faza 4: Dashboard UI
1. `Dashboard.tsx` - layout z kartami
2. `StatusCards.tsx` - import, eksport, moc grzałki, moc zadana
3. `ModeControl.tsx` - checkbox AUTO + slider
4. `PowerChart.tsx` + `ExportChart.tsx` - wykresy (recharts)
5. `StabilizationInfo.tsx` - badge'e parametrów
6. Polling co 1s z frontendu do `/api/status`

### Faza 5: Firmware ESP8266
1. Nowy sketch: Modbus + SSR + HTTP POST co 1s
2. Bearer token w nagłówku
3. Parsowanie JSON odpowiedzi -> ustawienie duty
4. Safety system:
   - `checkSafety()` w loop co 1s
   - Grace period 10s (trzyma ostatni duty)
   - Ramping down -5%/s (10-30s bez odpowiedzi)
   - SAFE MODE duty=0% (30s+ bez odpowiedzi)
   - Auto-recovery po reconnect
5. LED sygnalizacja: solid (OK), slow blink (ramp down), fast blink (safe mode)
6. Raport stanu safety w każdym POST (`safe_mode`, `seconds_since_last_response`)
7. OTA update (opcjonalnie)

---

## Uwagi techniczne

- **ESP8266 vs ESP32**: ESP8266 ma jeden UART hardware, Modbus po SoftwareSerial. GPIO piny inne niż ESP32. Trzeba dostosować.
- **Latencja**: ESP pyta co 1s, logika odpowiada natychmiast = max 1s opóźnienia. Dla grzałki OK.
- **Safety 3-stopniowy**: Grace 10s -> Ramping -5%/s -> SAFE MODE (duty=0%). ESP nie potrzebuje serwera żeby się bezpiecznie wyłączyć.
- **Bezpieczeństwo**: Token w `.env.local`, nigdy w repo. Next.js weryfikuje Bearer token z ESP.
- **Skalowalność**: Jeden endpoint `/api/esp/report` obsługuje wszystko - dane IN, komenda OUT. Prosty protokół.
- **Next.js cold start**: Przy serverless (Vercel) pierwszy request może mieć opóźnienie. Store w pamięci resetuje się po cold start. Rozważyć Redis/DB dla persystencji albo hostować na VPS.
