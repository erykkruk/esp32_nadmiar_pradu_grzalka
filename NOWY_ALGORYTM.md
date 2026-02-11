# Optymalny Algorytm Sterowania Grzałką v2.0

## Filozofia nowego algorytmu

```
PRIORYTET 1: NIGDY nie pobieraj z sieci (import = 0)
PRIORYTET 2: Minimalizuj eksport (zużyj maksimum nadwyżki)
PRIORYTET 3: Płynna praca SSR (bez szarpania)
```

---

## Architektura systemu

```
┌─────────────────────────────────────────────────────────────────┐
│                    WARSTWA ODCZYTU                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │ Modbus Read  │→ │ Filtr Spike  │→ │ Trend Calc   │           │
│  │ (1s interval)│  │ (odrzuć >3σ) │  │ (dP/dt)      │           │
│  └──────────────┘  └──────────────┘  └──────────────┘           │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                    WARSTWA DECYZYJNA                             │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                   STATE MACHINE                           │   │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐      │   │
│  │  │ STARTUP │→ │ NORMAL  │⇄ │ CAUTION │→ │EMERGENCY│      │   │
│  │  └─────────┘  └─────────┘  └─────────┘  └─────────┘      │   │
│  └──────────────────────────────────────────────────────────┘   │
│                              ↓                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │              REGULATOR PROPORCJONALNY                     │   │
│  │         P_target = P_current + Kp × (Export - Reserve)    │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                    WARSTWA WYKONAWCZA                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │ Rate Limiter │→ │ EMA Filter   │→ │ SSR Burst    │           │
│  │ (max ΔP/s)   │  │ (smooth)     │  │ (50Hz)       │           │
│  └──────────────┘  └──────────────┘  └──────────────┘           │
└─────────────────────────────────────────────────────────────────┘
```

---

## Stany systemu (State Machine)

### 1. STARTUP (po restarcie)
```cpp
Warunek wejścia: boot systemu lub utrata komunikacji Modbus > 30s
Zachowanie:     szybkie zwiększanie mocy do poziomu eksportu
Parametry:      Kp = 2.0, brak rate limitera
Wyjście:        gdy |P_target - P_applied| < 100W przez 5s → NORMAL
```

### 2. NORMAL (normalna praca)
```cpp
Warunek wejścia: stabilna praca
Zachowanie:     regulator proporcjonalny z rezerwą
Parametry:      Kp = 0.5, Reserve = 100W
Wyjście:        gdy import > 0W → CAUTION
                gdy eksport < 50W i trend ↓ → CAUTION
```

### 3. CAUTION (ostrzeżenie)
```cpp
Warunek wejścia: niski eksport lub lekki import
Zachowanie:     szybsza redukcja mocy, zwiększona rezerwa
Parametry:      Kp = 1.0, Reserve = 200W
Wyjście:        gdy import > 100W → EMERGENCY
                gdy eksport > 300W przez 10s → NORMAL
```

### 4. EMERGENCY (awaryjne)
```cpp
Warunek wejścia: znaczny import z sieci
Zachowanie:     NATYCHMIASTOWE wyłączenie grzałki
Parametry:      P_target = 0, ignoruj filtry
Wyjście:        gdy eksport > Reserve przez 5s → STARTUP (restart)
```

---

## Regulator proporcjonalny

### Podstawowe równanie
```cpp
error = currentExport - RESERVE
P_target = P_current + Kp × error

// Ograniczenia
P_target = constrain(P_target, 0, P_MAX)
```

### Adaptacyjne Kp
```cpp
// Kp zależy od stanu i wielkości błędu
if (state == STARTUP) {
    Kp = 2.0;  // Szybki start
} else if (state == EMERGENCY) {
    Kp = 10.0; // Natychmiastowa reakcja
} else if (abs(error) > 500) {
    Kp = 1.5;  // Duży błąd = szybsza korekta
} else if (abs(error) > 200) {
    Kp = 0.8;  // Średni błąd
} else {
    Kp = 0.3;  // Mały błąd = delikatna korekta
}
```

### Analiza trendu (predykcja)
```cpp
// Oblicz tempo zmiany eksportu
float dExport_dt = (currentExport - prevExport) / dt;  // W/s

// Predykcja na 5 sekund
float predictedExport = currentExport + dExport_dt * 5.0;

// Jeśli trend spadkowy i zbliżamy się do zera
if (dExport_dt < -50 && predictedExport < RESERVE) {
    // Prewencyjna redukcja
    P_target -= abs(dExport_dt) * 2.0;
}
```

---

## Filtry i ograniczenia

### 1. Filtr spike'ów (odrzucanie błędnych odczytów)
```cpp
// Odrzuć odczyty różniące się o więcej niż 3σ od średniej
const float SPIKE_THRESHOLD = 1000.0;  // W

if (abs(newReading - avgReading) > SPIKE_THRESHOLD) {
    // Prawdopodobny błąd odczytu - użyj poprzedniej wartości
    newReading = prevReading;
    spikeCount++;
}
```

### 2. Rate limiter (ograniczenie szybkości zmian)
```cpp
// Maksymalna zmiana mocy na sekundę
const float MAX_INCREASE_RATE = 500.0;  // W/s (narastanie)
const float MAX_DECREASE_RATE = 1000.0; // W/s (opadanie - szybsze!)

float deltaP = P_target - P_current;

if (deltaP > 0) {
    deltaP = min(deltaP, MAX_INCREASE_RATE * dt);
} else {
    deltaP = max(deltaP, -MAX_DECREASE_RATE * dt);
}

P_limited = P_current + deltaP;
```

### 3. Filtr EMA (wygładzanie)
```cpp
// Stała czasowa zależna od kierunku
float tau = (P_target > P_applied) ? TAU_UP : TAU_DOWN;
float alpha = dt / (tau + dt);

P_applied = P_applied + alpha * (P_limited - P_applied);
```

---

## Parametry konfiguracyjne

### Podstawowe (edytowalne przez WWW)
```cpp
struct Config {
    // Moc
    float P_MAX = 2000.0;           // Maksymalna moc grzałki [W]

    // Rezerwa eksportu
    float RESERVE_NORMAL = 100.0;   // Rezerwa w trybie NORMAL [W]
    float RESERVE_CAUTION = 200.0;  // Rezerwa w trybie CAUTION [W]

    // Progi stanów
    float IMPORT_CAUTION = 0.0;     // Próg importu dla CAUTION [W]
    float IMPORT_EMERGENCY = 100.0; // Próg importu dla EMERGENCY [W]
    float EXPORT_NORMAL = 300.0;    // Eksport do przejścia do NORMAL [W]

    // Regulator
    float Kp_NORMAL = 0.5;          // Wzmocnienie w NORMAL
    float Kp_CAUTION = 1.0;         // Wzmocnienie w CAUTION
    float Kp_STARTUP = 2.0;         // Wzmocnienie w STARTUP

    // Filtry
    float TAU_UP = 3.0;             // Stała czasowa narastania [s]
    float TAU_DOWN = 1.5;           // Stała czasowa opadania [s]
    float MAX_INCREASE_RATE = 500;  // Max wzrost [W/s]
    float MAX_DECREASE_RATE = 1000; // Max spadek [W/s]

    // Czasowe
    int DECISION_INTERVAL_MS = 1000; // Interwał decyzji [ms]
    int TREND_WINDOW_S = 5;          // Okno analizy trendu [s]
};
```

### Preset'y
```cpp
// TRYB: Maksymalna oszczędność (priorytet: zero importu)
Config PRESET_SAVINGS = {
    .RESERVE_NORMAL = 150.0,
    .RESERVE_CAUTION = 300.0,
    .IMPORT_EMERGENCY = 50.0,
    .Kp_NORMAL = 0.3,
    .MAX_DECREASE_RATE = 2000.0,
};

// TRYB: Maksymalne zużycie nadwyżki (priorytet: minimalizuj eksport)
Config PRESET_GREEDY = {
    .RESERVE_NORMAL = 50.0,
    .RESERVE_CAUTION = 100.0,
    .IMPORT_EMERGENCY = 200.0,
    .Kp_NORMAL = 0.8,
    .MAX_INCREASE_RATE = 1000.0,
};

// TRYB: Zbalansowany (domyślny)
Config PRESET_BALANCED = {
    // domyślne wartości
};
```

---

## Pseudokod głównej pętli

```cpp
void loop() {
    // 1. Odczyt i filtracja
    if (timeForReading()) {
        float rawPower = readModbus();
        float filteredPower = filterSpikes(rawPower);
        updateTrendBuffer(filteredPower);

        currentExport = (filteredPower < 0) ? -filteredPower : 0;
        currentImport = (filteredPower > 0) ? filteredPower : 0;
    }

    // 2. Aktualizacja stanu
    if (timeForDecision()) {
        updateStateMachine();

        // 3. Oblicz błąd i cel
        float reserve = getReserveForState();
        float Kp = getKpForState();
        float error = currentExport - reserve;

        // 4. Predykcja trendu
        float trend = calculateTrend();
        if (trend < -50 && currentExport < reserve * 2) {
            error -= abs(trend) * config.TREND_WINDOW_S;  // Prewencyjna korekta
        }

        // 5. Regulator
        P_target = P_current + Kp * error;
        P_target = constrain(P_target, 0, config.P_MAX);

        // 6. EMERGENCY override
        if (state == EMERGENCY) {
            P_target = 0;
        }
    }

    // 7. Rate limiting
    P_limited = applyRateLimiter(P_target);

    // 8. Filtr EMA
    P_applied = applyEMAFilter(P_limited);

    // 9. Sterowanie SSR
    updateSSRBurstFire(P_applied);

    // 10. Web server
    handleWebRequests();
}
```

---

## Porównanie algorytmów

| Cecha | Stary algorytm | Nowy algorytm |
|-------|----------------|---------------|
| Typ regulatora | Schodkowy (±50W) | Proporcjonalny |
| Interwał decyzji | 10s | 1s |
| Reakcja na import | Powolna (-50W/10s) | Natychmiastowa (EMERGENCY) |
| Predykcja | Brak | Analiza trendu |
| Stany systemu | 2 (AUTO/MANUAL) | 4 (STARTUP/NORMAL/CAUTION/EMERGENCY) |
| Konfigurowalność | Brak | Pełna przez WWW |
| Preset'y | Brak | 3 tryby |
| Ochrona przed importem | Słaba | Wielopoziomowa |
| Czas reakcji | 15-25s | 1-3s |
| Restart | 400s do pełnej mocy | 10s |

---

## Diagram przepływu

```
                    ┌─────────────┐
                    │   START     │
                    └──────┬──────┘
                           ↓
                    ┌─────────────┐
                    │   STARTUP   │←─────────────────────┐
                    │  (Kp=2.0)   │                      │
                    └──────┬──────┘                      │
                           ↓                             │
                    ┌─────────────┐                      │
            ┌──────→│   NORMAL    │←────────┐            │
            │       │  (Kp=0.5)   │         │            │
            │       └──────┬──────┘         │            │
            │              ↓                │            │
            │       Import > 0 lub          │            │
            │       trend ↓ ostry           │            │
            │              ↓                │            │
            │       ┌─────────────┐         │            │
            │       │  CAUTION    │─────────┘            │
            │       │  (Kp=1.0)   │  eksport > 300W      │
            │       └──────┬──────┘  przez 10s           │
            │              ↓                             │
            │       Import > 100W                        │
            │              ↓                             │
            │       ┌─────────────┐                      │
            │       │ EMERGENCY   │──────────────────────┘
            │       │  P=0, OFF!  │  eksport > reserve
            │       └──────┬──────┘  przez 5s
            │              ↓
            └──────────────┘
              eksport stabilny
              przez 30s
```

---

## Zabezpieczenia

### 1. Watchdog Modbus
```cpp
if (millis() - lastModbusOk > 30000) {
    // Brak komunikacji z licznikiem > 30s
    state = EMERGENCY;
    P_target = 0;
    modbusError = true;
}
```

### 2. Watchdog WiFi
```cpp
if (WiFi.status() != WL_CONNECTED) {
    // WiFi rozłączone - kontynuuj pracę z ostatnimi parametrami
    // ale nie przyjmuj nowych komend
    wifiError = true;
}
```

### 3. Limit temperatury SSR (opcjonalnie)
```cpp
if (ssrTemperature > 80) {
    // SSR za gorący - ogranicz moc
    P_MAX_TEMP = P_MAX * 0.5;
}
```

### 4. Suma kontrolna konfiguracji
```cpp
// Weryfikacja poprawności konfiguracji w EEPROM
if (configCRC != calculateCRC(config)) {
    // Uszkodzona konfiguracja - użyj domyślnych
    config = DEFAULT_CONFIG;
}
```
