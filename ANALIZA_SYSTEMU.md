# Analiza Systemu Sterowania Grzałką ESP32

## Co robi aplikacja?

System automatycznie zagospodarowuje nadwyżkę energii z paneli fotowoltaicznych poprzez grzanie wody w bojlerze. Działa jako "przekierowanie" nadmiaru prądu, który normalnie trafiłby do sieci.

### Schemat działania

```
Panele PV → Licznik DTSU666 → ESP32 → SSR → Grzałka 2kW
                ↓
         Odczyt mocy co 1s
         (Modbus RS485)
                ↓
         Eksport do sieci?
         (ujemna wartość)
                ↓
      Zwiększ moc grzałki
      (zużyj nadwyżkę)
```

### Algorytm sterowania (tryb AUTO)

1. **Odczyt** - co 1 sekundę pobiera moc z licznika DTSU666
2. **Uśrednianie** - bufor 10 ostatnich pomiarów (średnia krocząca)
3. **Decyzja** - co 10 sekund na podstawie średniego eksportu:
   - Eksport < 50W → zmniejsz moc grzałki o 50W
   - Eksport 50-249W → nic nie rób (strefa martwa)
   - Eksport ≥ 250W → zwiększ moc grzałki o 50W
4. **Filtrowanie** - płynne przejścia przez filtr EMA (TAU_UP=8s, TAU_DOWN=5s)
5. **Sterowanie SSR** - burst-fire, 100 półokresów/sekundę przy 50Hz

---

## Ograniczenia systemu

### 1. Wolna reakcja na zmiany

| Element | Opóźnienie |
|---------|------------|
| Uśrednianie | 10 sekund (bufor) |
| Interwał decyzji | 10 sekund |
| Filtr EMA (narastanie) | ~8 sekund (TAU_UP) |
| Filtr EMA (opadanie) | ~5 sekund (TAU_DOWN) |
| **Łączne opóźnienie** | **~15-25 sekund** |

**Problem:** Gdy chmura zasłoni słońce, system potrzebuje 15-25 sekund aby zacząć redukować moc. Przez ten czas pobierasz prąd z sieci.

### 2. Szeroka strefa martwa (histereza 50-250W)

```
Eksport [W]
    0    50         250       500
    |----|-----------|---------|
    ↓     \___________/    ↓
  -50W    BEZ ZMIAN      +50W
```

**Problem:** Jeśli eksport wynosi np. 150W, system nic nie robi. Ta energia "przepada" do sieci zamiast trafić do grzałki.

### 3. Małe kroki regulacji (50W)

Przy mocy grzałki 2000W:
- Osiągnięcie pełnej mocy: 2000W / 50W = **40 kroków × 10s = 400 sekund (~7 minut)**
- Zejście do zera: podobnie ~7 minut

**Problem:** Zbyt wolna adaptacja do dużych zmian produkcji PV.

### 4. Brak uwzględnienia innych odbiorników

System patrzy tylko na bilans sieci. Nie wie:
- Że za chwilę włączy się pralka (1500W)
- Że lodówka właśnie startuje sprężarkę
- Że ktoś włącza czajnik

**Problem:** Nagłe włączenie dużego odbiornika = pobór z sieci, bo grzałka nie zdąży się wyłączyć.

### 5. Stała rezerwa eksportu (100W) - niewykorzystana

```cpp
const float EXPORT_RESERVE_W = 100.0f;  // Zdefiniowana...
```

**Problem:** Ta stała jest zdefiniowana, ale **nigdzie nie używana** w logice sterowania! To prawdopodobnie błąd - miała zapobiegać poborowi z sieci.

---

## Dlaczego nie działa jak powinno?

### Problem 1: Oscylacje mocy

**Objaw:** Moc grzałki skacze w górę i w dół, nigdy się nie stabilizuje.

**Przyczyna:** Zbyt wąska strefa martwa vs. opóźnienia systemu.

**Scenariusz:**
1. Eksport = 300W → system zwiększa moc o 50W
2. Po 10s eksport spada do 40W (bo grzałka pobiera więcej)
3. System zmniejsza moc o 50W
4. Eksport rośnie do 260W
5. Powtórz od punktu 1...

### Problem 2: Pobór z sieci przy zmiennej pogodzie

**Objaw:** Mimo że "średnio" jest nadwyżka, opłaty za prąd rosną.

**Przyczyna:** Opóźnienie 15-25s + asymetria kosztów:
- Eksport nadwyżki → niski zysk (taryfa eksportowa)
- Import deficytu → wysoki koszt (taryfa importowa + opłaty)

**Przykład:**
```
Czas    Produkcja PV    Grzałka    Bilans sieci
0s      1500W           0W         -1500W (eksport)
10s     500W (chmura)   0W         -500W  (eksport)
15s     500W            500W       0W     (system reaguje)
20s     1500W (słońce)  500W       -1000W (eksport, ale grzałka za słaba)
30s     1500W           550W       -950W  (powolne zwiększanie...)
```

### Problem 3: EXPORT_RESERVE_W nie działa

**Kod w `updateStepFromExport()`:**
```cpp
if (avgExport < EXPORT_LOW) {        // < 50W
    P_step -= STEP_W;                 // Zmniejsz
}
else if (avgExport >= EXPORT_HIGH) { // >= 250W
    P_step += STEP_W;                 // Zwiększ
}
// Brak sprawdzenia EXPORT_RESERVE_W!
```

**Problem:** System powinien zmniejszać moc gdy eksport < 100W (rezerwa), ale sprawdza < 50W.

### Problem 4: Brak ochrony przed importem

Gdy eksport = 0W lub jest import, system tylko powoli zmniejsza moc (o 50W co 10s). Nie ma mechanizmu "natychmiastowego wyłączenia" gdy zaczynamy pobierać z sieci.

---

## Sugestie naprawy

### Szybka poprawa (bez zmiany architektury)

1. **Użyć EXPORT_RESERVE_W:**
   ```cpp
   if (avgExport < EXPORT_RESERVE_W) {  // < 100W zamiast < 50W
       P_step -= STEP_W;
   }
   ```

2. **Zmniejszyć interwał decyzji:**
   ```cpp
   const unsigned long DECISION_INTERVAL_MS = 5000;  // 5s zamiast 10s
   ```

3. **Zwiększyć krok przy dużych zmianach:**
   ```cpp
   if (avgExport < 20.0f) {
       P_step -= 200.0f;  // Szybkie zmniejszenie przy krytycznie niskim eksporcie
   }
   ```

### Gruntowna poprawa

1. **Regulator proporcjonalny** zamiast schodkowego:
   ```cpp
   float error = avgExport - EXPORT_RESERVE_W;
   P_target = P_current + Kp * error;
   ```

2. **Szybka ścieżka dla importu:**
   ```cpp
   if (md.Pt > 0) {  // Import z sieci!
       P_step = max(0, P_step - 500.0f);  // Natychmiastowe zmniejszenie
   }
   ```

3. **Predykcja na podstawie trendu:**
   - Jeśli eksport szybko spada → zmniejsz moc prewencyjnie
   - Jeśli eksport szybko rośnie → zwiększ moc szybciej

---

## Podsumowanie

| Aspekt | Stan obecny | Problem |
|--------|-------------|---------|
| Czas reakcji | 15-25s | Za wolno przy zmiennej pogodzie |
| Strefa martwa | 50-250W | Traci 50-249W eksportu |
| Krok regulacji | 50W | Za mały dla szybkich zmian |
| Rezerwa eksportu | Niezaimplementowana | Brak ochrony przed importem |
| Ochrona przed importem | Brak | Pobiera z sieci w oczekiwaniu na decyzję |

**Główny wniosek:** System jest zbyt powolny i konserwatywny dla realnych warunków pracy z fotowoltaiką. Działa dobrze przy stabilnym nasłonecznieniu, ale przy zmiennej pogodzie będzie powodował pobór z sieci.
