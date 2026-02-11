# Symulacje i Testy Scenariuszowe

## Scenariusz 1: Nagłe włączenie urządzenia zewnętrznego

### Warunki początkowe
- Produkcja PV: 1500W
- Grzałka: 1000W (50%)
- Eksport do sieci: 500W
- System stabilny

### Zdarzenie: Włączenie czajnika 2000W

```
Czas   PV      Grzałka  Czajnik  Bilans    Akcja systemu
─────────────────────────────────────────────────────────
0s     1500W   1000W    0W       -500W     (eksport, OK)
1s     1500W   1000W    2000W    +1500W    IMPORT! Bufor: 0W eksportu
2s     1500W   1000W    2000W    +1500W    Bufor: 0W
...
10s    1500W   1000W    2000W    +1500W    Średnia < 50W → -50W
11s    1500W   950W     2000W    +1450W    Filtr EMA zaczyna działać
...
20s    1500W   950W     2000W    +1450W    Średnia < 50W → -50W (900W)
...
60s    1500W   700W     0W       -800W     Czajnik wyłączony
```

### Wynik: KATASTROFA
- **Import z sieci przez 60s: ~1450W × 60s = 24 Wh**
- System zredukował grzałkę tylko o 300W w 60 sekund
- Po wyłączeniu czajnika - duży eksport, system powoli wraca

### Co powinno się stać?
```
0s     Wykrycie importu → NATYCHMIAST grzałka na 0W
1s     Bilans: +500W (tylko czajnik)
60s    Czajnik off → Powolny powrót grzałki
```

---

## Scenariusz 2: Przejście chmury (szybki spadek produkcji)

### Warunki początkowe
- Produkcja PV: 2000W
- Grzałka: 1800W (90%)
- Eksport: 200W

### Zdarzenie: Chmura zasłania panel (produkcja 500W)

```
Czas   PV      Grzałka  Bilans    Średni eksport  Decyzja
───────────────────────────────────────────────────────────
0s     2000W   1800W    -200W     200W            -
1s     500W    1800W    +1300W    180W            (czeka)
2s     500W    1800W    +1300W    160W            (czeka)
...
9s     500W    1800W    +1300W    20W             (czeka na 10s)
10s    500W    1800W    +1300W    0W              → -50W (1750W)
11s    500W    1750W    +1250W    0W              EMA: ~1740W
...
20s    500W    1700W    +1200W    0W              → -50W (1650W)
...
```

### Wynik: BARDZO ZŁY
- **Import 1300W przez pierwsze 10 sekund = 3.6 Wh**
- Po 60 sekundach grzałka nadal ~1400W, ciągle import ~900W
- **Łączny import: ~50-60 Wh** zanim system się ustabilizuje

### Co powinno się stać?
```
1s     Wykrycie trendu spadkowego → P_target = 500W - 100W = 400W
2s     Grzałka szybko schodzi do 400W (bez filtra EMA w dół!)
       Bilans: +100W (mały import, akceptowalny)
```

---

## Scenariusz 3: Oscylacje przy granicy progu

### Warunki początkowe
- Produkcja PV: 1300W (lekko zmienna ±50W)
- Grzałka: 1000W
- Eksport: ~300W

### Problem: System na granicy EXPORT_HIGH (250W)

```
Czas   PV      Grzałka  Eksport   Decyzja
─────────────────────────────────────────
0s     1300W   1000W    300W      → +50W
10s    1300W   1050W    250W      → +50W (na granicy!)
20s    1300W   1100W    200W      - (strefa martwa)
30s    1250W   1100W    150W      - (strefa martwa)
40s    1350W   1100W    250W      → +50W
50s    1300W   1150W    150W      - (strefa martwa)
...
```

### Wynik: SUBOPTYMALNE
- Eksport oscyluje 150-250W
- ~200W ciągle "ucieka" do sieci
- Nigdy nie osiągnie optimum (eksport = 100W rezerwy)

---

## Scenariusz 4: Start systemu przy dużej produkcji

### Warunki początkowe
- Produkcja PV: 2500W (więcej niż moc grzałki)
- Grzałka: 0W (start)
- Eksport: 2500W

### Obecne zachowanie

```
Czas   PV      Grzałka  Eksport   Decyzja
─────────────────────────────────────────
0s     2500W   0W       2500W     → +50W
10s    2500W   50W      2450W     → +50W
20s    2500W   100W     2400W     → +50W
...
400s   2500W   2000W    500W      MAKSIMUM
```

### Wynik: BARDZO WOLNO
- **400 sekund (~7 minut)** żeby osiągnąć maksymalną moc
- Przez ten czas wyeksportowano: ~1000W × 400s / 2 = **55 Wh do sieci**

---

## Scenariusz 5: Mikro-oscylacje produkcji PV

### Warunki
- Produkcja PV: 1000W ± 100W (fluktuacje co 2-3s od wiatru)
- Grzałka: 800W
- Cel: eksport ~100W (rezerwa)

### Problem z uśrednianiem

```
Próbki eksportu (co 1s): 200, 100, 250, 150, 50, 300, 100, 150, 200, 180
Średnia: 168W → strefa martwa, brak akcji

ALE: niektóre próbki (50W) oznaczają chwilowy import!
```

### Wynik: UKRYTE STRATY
- Średnia wygląda OK
- Ale chwilowe importy kosztują (taryfa importowa > eksportowa)

---

## Scenariusz 6: Awaria komunikacji Modbus

### Warunki
- System działa normalnie
- Grzałka: 1500W
- Nagle licznik przestaje odpowiadać

### Obecne zachowanie

```cpp
bool readFloat32_raw(uint16_t reg, float& outVal) {
  for (int i=0;i<2;i++){  // 2 próby
    ...
  }
  return false;  // I co dalej? NIC!
}
```

### Wynik: NIEBEZPIECZNY
- `md.Pt` pozostaje na starej wartości
- Grzałka dalej grzeje 1500W
- Jeśli produkcja PV spadła → import z sieci bez limitu

---

## Tabela podsumowująca

| Scenariusz | Czas reakcji | Strata energii | Ryzyko |
|------------|--------------|----------------|--------|
| 1. Czajnik 2kW | 60s+ | ~24 Wh import | WYSOKIE |
| 2. Chmura | 60s+ | ~50 Wh import | WYSOKIE |
| 3. Oscylacje | ∞ | ~200W ciągły eksport | ŚREDNIE |
| 4. Zimny start | 400s | ~55 Wh eksport | NISKIE |
| 5. Mikro-fluktuacje | - | Ukryte importy | ŚREDNIE |
| 6. Awaria Modbus | ∞ | Nieograniczone | KRYTYCZNE |

---

## Wnioski z symulacji

### Główne słabości obecnego algorytmu

1. **Brak rozróżnienia import/eksport**
   - System traktuje eksport 0W i import 500W tak samo
   - Nie ma "trybu alarmowego" przy imporcie

2. **Symetryczna reakcja góra/dół**
   - Zwiększanie i zmniejszanie mocy trwa tak samo długo
   - Zmniejszanie powinno być NATYCHMIASTOWE

3. **Średnia arytmetyczna zamiast ważonej**
   - Stara próbka (sprzed 9s) ma taką samą wagę jak nowa
   - Nie wykrywa trendów

4. **Brak failsafe przy utracie komunikacji**
   - System nie wie, że dane są nieaktualne
   - Powinien wyłączyć grzałkę po X sekundach bez odczytu

5. **Stałe parametry dla różnych warunków**
   - Ten sam algorytm dla słonecznego dnia i pochmurnego
   - Powinien być bardziej agresywny przy zmiennej pogodzie
