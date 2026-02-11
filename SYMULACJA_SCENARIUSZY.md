# Symulacja Scenariuszy Testowych

## Metodologia symulacji

Symulacja krok po kroku pokazuje zachowanie **obecnego algorytmu** vs **proponowanego nowego algorytmu**.

Założenia:
- Grzałka: 2000W max
- Odczyt licznika: co 1s
- Obecny algorytm: decyzje co 10s, krok 50W, histereza 50-250W
- Nowy algorytm: decyzje co 2s, regulator proporcjonalny, szybka reakcja na import

---

## Scenariusz 1: Nagłe włączenie czajnika (2000W)

**Sytuacja:** Słoneczny dzień, stabilna produkcja 3000W, grzałka działa na 1500W. Ktoś włącza czajnik 2000W na 3 minuty.

### Stan początkowy
```
Produkcja PV:  3000W
Zużycie domu:   500W
Grzałka:       1500W
Bilans sieci: -1000W (eksport)
```

### OBECNY ALGORYTM

| Czas | Zdarzenie | Produkcja | Zużycie domu | Grzałka | Bilans sieci | Decyzja algorytmu |
|------|-----------|-----------|--------------|---------|--------------|-------------------|
| 0s | Start | 3000W | 500W | 1500W | -1000W | - |
| 1s | Czajnik ON | 3000W | 2500W | 1500W | **+0W** | Czeka... |
| 10s | - | 3000W | 2500W | 1500W | **+0W** | avgExport=100W → brak zmian |
| 20s | - | 3000W | 2500W | 1500W | **+0W** | avgExport=0W → -50W |
| 30s | - | 3000W | 2500W | 1450W | **-50W** | avgExport≈5W → -50W |
| 40s | - | 3000W | 2500W | 1400W | **-100W** | avgExport≈25W → -50W |
| ... | ... | ... | ... | ... | ... | ... |
| 180s | Czajnik OFF | 3000W | 500W | 1100W | **-1400W** | - |

**Wynik:** Przez 180 sekund system pobierał chwilami z sieci lub balansował na granicy. Zredukował grzałkę o 400W (8 kroków × 50W).

**Problem:** Po wyłączeniu czajnika mamy eksport 1400W, a grzałka pracuje tylko na 1100W. Odbudowa do 1500W zajmie:
- 400W / 50W = 8 kroków × 10s = **80 sekund**

### NOWY ALGORYTM (proporcjonalny)

| Czas | Zdarzenie | Produkcja | Zużycie domu | Grzałka | Bilans sieci | Decyzja algorytmu |
|------|-----------|-----------|--------------|---------|--------------|-------------------|
| 0s | Start | 3000W | 500W | 1500W | -1000W | - |
| 1s | Czajnik ON | 3000W | 2500W | 1500W | **+0W** | ALERT: import! |
| 2s | Reakcja | 3000W | 2500W | **500W** | **-1000W** | Szybka redukcja o 1000W |
| 4s | Korekta | 3000W | 2500W | **400W** | **-900W** | Drobna korekta |
| 10s | Stabilizacja | 3000W | 2500W | **350W** | **-850W** | Utrzymuje rezerwę 100W |
| 180s | Czajnik OFF | 3000W | 500W | 350W | **-2150W** | Duży eksport! |
| 182s | Reakcja | 3000W | 500W | **1200W** | **-1300W** | Szybki wzrost |
| 186s | Korekta | 3000W | 500W | **1400W** | **-1100W** | Kontynuacja |
| 190s | Stabilizacja | 3000W | 500W | **1500W** | **-1000W** | Cel osiągnięty |

**Wynik:** Natychmiastowa reakcja (2s), minimalizacja poboru z sieci, szybka odbudowa po wyłączeniu czajnika (10s vs 80s).

---

## Scenariusz 2: Chmura zasłania słońce (50% spadek mocy)

**Sytuacja:** Produkcja PV spada z 4000W do 2000W w ciągu 30 sekund.

### Stan początkowy
```
Produkcja PV:  4000W
Zużycie domu:   500W
Grzałka:       2000W (MAX)
Bilans sieci: -1500W (eksport)
```

### OBECNY ALGORYTM

| Czas | Produkcja PV | Grzałka | Bilans | avgExport | Decyzja |
|------|--------------|---------|--------|-----------|---------|
| 0s | 4000W | 2000W | -1500W | 1500W | - |
| 10s | 3500W | 2000W | -1000W | 1250W | +50W (już MAX) |
| 15s | 3000W | 2000W | -500W | 1000W | - |
| 20s | 2500W | 2000W | **+0W** | 750W | - |
| 25s | 2200W | 2000W | **+300W** | 550W | - |
| 30s | 2000W | 2000W | **+500W** | 400W | avgExport=400W → +50W (MAX!) |
| 40s | 2000W | 2000W | **+500W** | 100W | avgExport=100W → brak zmian |
| 50s | 2000W | 2000W | **+500W** | 0W | avgExport=0W → -50W |
| 60s | 2000W | 1950W | **+450W** | ~25W | -50W |
| 70s | 2000W | 1900W | **+400W** | ~50W | brak zmian |
| ... | ... | ... | ... | ... | ... |
| 200s | 2000W | 1400W | **-100W** | ~100W | stabilizacja |

**Problem:**
- Import z sieci przez **~150 sekund** (200s - 50s)
- Zużyto z sieci: ~500W × 150s = **~20 Wh** niepotrzebnie
- Bardzo wolna reakcja na spadek produkcji

### NOWY ALGORYTM

| Czas | Produkcja PV | Grzałka | Bilans | Decyzja |
|------|--------------|---------|--------|---------|
| 0s | 4000W | 2000W | -1500W | - |
| 10s | 3500W | 2000W | -1000W | OK, duży eksport |
| 15s | 3000W | 2000W | -500W | eksport spada, obserwuję trend |
| 17s | 2800W | 1800W | -500W | **prewencyjna redukcja** (trend↓) |
| 20s | 2500W | 1600W | -400W | kontynuacja redukcji |
| 25s | 2200W | 1400W | -300W | zbliżam się do celu |
| 30s | 2000W | 1400W | -100W | **stabilizacja** (rezerwa 100W) |

**Wynik:**
- Zero importu z sieci
- Reakcja prewencyjna na trend spadkowy
- Stabilizacja w 30s zamiast 200s

---

## Scenariusz 3: Oscylacje przy granicy histerzy

**Sytuacja:** Produkcja PV = 2600W, zużycie domu = 500W. System powinien ustawić grzałkę na ~2000W.

### OBECNY ALGORYTM (problem oscylacji)

| Cykl | avgExport | Decyzja | Nowa moc | Nowy bilans |
|------|-----------|---------|----------|-------------|
| 1 | 100W | (50-249W) brak zmian | 2000W | -100W |
| 2 | 100W | (50-249W) brak zmian | 2000W | -100W |

**Przypadek A:** Mała zmiana produkcji (+50W)
| Cykl | avgExport | Decyzja | Nowa moc | Nowy bilans |
|------|-----------|---------|----------|-------------|
| 1 | 150W | brak zmian | 2000W | -150W |
| 2 | 150W | brak zmian | 2000W | -150W |

**To działa OK** - histereza chroni przed oscylacjami.

**Przypadek B:** Produkcja spada o 100W (2500W)
| Cykl | avgExport | Decyzja | Nowa moc | Nowy bilans |
|------|-----------|---------|----------|-------------|
| 1 | 0W | **-50W** | 1950W | -50W |
| 2 | 50W | brak zmian | 1950W | -50W |
| 3 | 50W | brak zmian | 1950W | -50W |

**Problem:** Eksport 50W jest "zmarnowany" - moglibyśmy zużyć więcej.

**Przypadek C:** Produkcja rośnie o 200W (2800W)
| Cykl | avgExport | Decyzja | Nowa moc | Nowy bilans |
|------|-----------|---------|----------|-------------|
| 1 | 300W | **+50W** | 2000W (MAX) | -300W |
| 2 | 300W | +50W (ale MAX!) | 2000W | -300W |

**Problem:** 300W eksportu jest "zmarnowane" bo grzałka już na MAX.

### NOWY ALGORYTM (proporcjonalny)

```
Target = aktualny_eksport - REZERWA(100W)
P_nowa = P_aktualna + Kp × Target
```

| Produkcja | Eksport | Target | Kp=0.5 | Nowa moc | Nowy bilans |
|-----------|---------|--------|--------|----------|-------------|
| 2600W | 100W | 0W | 0W | 2000W | -100W |
| 2650W | 150W | 50W | +25W | 2000W (MAX) | -150W |
| 2500W | 0W | -100W | -50W | 1950W | -50W |
| 2800W | 300W | 200W | +100W | 2000W (MAX) | -300W |

**Wynik:** Płynne dostosowanie, brak oscylacji, minimalne straty.

---

## Scenariusz 4: Włączenie wielu urządzeń jednocześnie

**Sytuacja:** W ciągu 30 sekund włączają się: pralka (500W), zmywarka (1500W), odkurzacz (1800W).

### Stan początkowy
```
Produkcja PV:  5000W
Zużycie domu:   300W
Grzałka:       2000W (MAX)
Bilans sieci: -2700W (eksport)
```

### OBECNY ALGORYTM

| Czas | Zdarzenie | Zużycie domu | Grzałka | Bilans | Akcja |
|------|-----------|--------------|---------|--------|-------|
| 0s | Start | 300W | 2000W | -2700W | - |
| 5s | Pralka ON | 800W | 2000W | -2200W | - |
| 10s | Decyzja | 800W | 2000W | -2200W | eksport>250W → MAX |
| 15s | Zmywarka ON | 2300W | 2000W | **-700W** | - |
| 20s | Decyzja | 2300W | 2000W | **-700W** | eksport>250W → MAX |
| 25s | Odkurzacz ON | 4100W | 2000W | **+1100W IMPORT!** | - |
| 30s | Decyzja | 4100W | 2000W | **+1100W** | avgExport=~0W → -50W |
| 40s | Decyzja | 4100W | 1950W | **+1050W** | avgExport=0W → -50W |
| ... | ... | ... | ... | ... | ... |
| 230s | Po 20 decyzjach | 4100W | 1000W | **+100W** | stabilizacja |

**Katastrofa:**
- Import 1100W przez **~200 sekund**
- Zużycie z sieci: ~1000W × 200s = **~55 Wh**
- System kompletnie nie nadąża

### NOWY ALGORYTM

| Czas | Zdarzenie | Zużycie domu | Grzałka | Bilans | Akcja |
|------|-----------|--------------|---------|--------|-------|
| 0s | Start | 300W | 2000W | -2700W | - |
| 5s | Pralka ON | 800W | 2000W | -2200W | OK |
| 15s | Zmywarka ON | 2300W | 2000W | -700W | eksport spada |
| 17s | Korekta | 2300W | 1800W | -900W | prewencja |
| 25s | Odkurzacz ON | 4100W | 1800W | **+300W** | ALERT! |
| 26s | **EMERGENCY** | 4100W | **0W** | **-900W** | natychmiastowe wyłączenie |
| 28s | Restart | 4100W | 200W | -700W | ostrożny restart |
| 35s | Korekta | 4100W | 500W | -400W | zwiększanie |
| 45s | Stabilizacja | 4100W | 700W | -200W | cel: 100W rezerwy |
| 55s | Stabilizacja | 4100W | 800W | -100W | **STABILNE** |

**Wynik:**
- Import tylko przez **~2 sekundy** (300W × 2s = ~0.2 Wh)
- Natychmiastowa reakcja na import (EMERGENCY mode)
- Stabilizacja w 55s zamiast 230s

---

## Scenariusz 5: Zachód słońca (powolny spadek produkcji)

**Sytuacja:** Produkcja spada z 4000W do 0W w ciągu 2 godzin (liniowo).

### OBECNY ALGORYTM

```
Tempo spadku: 4000W / 7200s = 0.56 W/s = 33 W/min
Decyzje co: 10s
Spadek między decyzjami: 5.6W

Reakcja: -50W gdy eksport < 50W
```

| Czas | Produkcja | Grzałka (idealna) | Grzałka (rzeczywista) | Strata |
|------|-----------|-------------------|----------------------|--------|
| 0min | 4000W | 2000W | 2000W | 0W |
| 30min | 3000W | 2000W | 2000W | 0W |
| 60min | 2000W | 1400W | 2000W | **import 100W** |
| 70min | 1670W | 1070W | 1850W | **import 680W** |
| 90min | 1000W | 400W | 1550W | **import 1050W** |
| 120min | 0W | 0W | 850W | **import 1350W** |

**Problem:** System nie nadąża za spadkiem produkcji. Przy tempie spadku 33W/min, a reakcji -50W/10s = -300W/min teoretycznie powinien nadążać, ALE:
- Histereza 50-250W opóźnia reakcję
- Filtr EMA dodatkowo spowalnia

### NOWY ALGORYTM

```
Predykcja trendu: jeśli eksport spada o >20W/min → przyspieszenie redukcji
```

| Czas | Produkcja | Grzałka | Bilans | Trend | Akcja |
|------|-----------|---------|--------|-------|-------|
| 0min | 4000W | 2000W | -1500W | - | - |
| 30min | 3000W | 2000W | -500W | ↓30W/min | prewencyjna redukcja |
| 32min | 2930W | 1800W | -630W | - | kontynuacja |
| 60min | 2000W | 1400W | -100W | ↓30W/min | stały spadek |
| 90min | 1000W | 400W | -100W | ↓30W/min | stały spadek |
| 120min | 0W | 0W | -100W→0W | - | wyłączenie |

**Wynik:** Zero importu przez cały zachód słońca.

---

## Scenariusz 6: Restart systemu w środku dnia

**Sytuacja:** ESP32 restartuje się. Produkcja 3000W, zużycie domu 500W.

### OBECNY ALGORYTM

| Czas | Grzałka | Bilans | Decyzja |
|------|---------|--------|---------|
| 0s (restart) | 0W | -2500W | P_step = 0 |
| 10s | 0W | -2500W | eksport>250W → +50W |
| 20s | 50W | -2450W | eksport>250W → +50W |
| ... | ... | ... | ... |
| 400s (40 decyzji) | 2000W | -500W | MAX osiągnięty |

**Problem:** 400 sekund (~7 minut) aby osiągnąć optymalną moc. Przez ten czas eksportujemy >2000W do sieci zamiast grzać wodę.

### NOWY ALGORYTM (z szybkim startem)

| Czas | Grzałka | Bilans | Decyzja |
|------|---------|--------|---------|
| 0s (restart) | 0W | -2500W | STARTUP MODE |
| 2s | 500W | -2000W | szybki wzrost |
| 4s | 1000W | -1500W | szybki wzrost |
| 6s | 1500W | -1000W | szybki wzrost |
| 8s | 1900W | -600W | zbliżanie do celu |
| 10s | 2000W | -500W | **stabilizacja** |

**Wynik:** Pełna moc w 10 sekund zamiast 400.

---

## Podsumowanie symulacji

| Scenariusz | Obecny algorytm | Nowy algorytm | Poprawa |
|------------|-----------------|---------------|---------|
| Czajnik 2000W | Import ~30s, odbudowa 80s | Import 0s, odbudowa 10s | **90% szybciej** |
| Chmura 50% | Import 150s, ~20Wh strata | Import 0s | **100% eliminacja** |
| Oscylacje | Straty 50-300W w strefie martwej | Minimalne straty | **~80% redukcja** |
| Wiele urządzeń | Import 200s, ~55Wh | Import 2s, ~0.2Wh | **99.6% redukcja** |
| Zachód słońca | Import w końcowej fazie | Zero importu | **100% eliminacja** |
| Restart | 400s do pełnej mocy | 10s do pełnej mocy | **97.5% szybciej** |

---

## Wnioski

Obecny algorytm **nie nadaje się** do realnych warunków pracy z:
- Zmienną pogodą
- Aktywnymi domownikami włączającymi urządzenia
- Szybkimi zmianami produkcji PV

**Konieczne zmiany:**
1. Regulator proporcjonalny zamiast schodkowego
2. Tryb EMERGENCY przy imporcie
3. Analiza trendu dla predykcji
4. Tryb STARTUP dla szybkiego startu
5. Konfigurowalne parametry przez interfejs WWW
