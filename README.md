# ESP32 Heater Control System

A smart heater control system for ESP32 that automatically manages excess power from solar panels by controlling an electric heater via SSR (Solid State Relay) using burst-fire control.

## Features

- **Automatic Power Management**: Monitors grid import/export and automatically adjusts heater power to consume excess solar energy
- **Modbus Integration**: Reads power data from DTSU666 energy meter via RS485/Modbus RTU
- **Web Interface**: Real-time monitoring and control via web browser
- **Stabilized Logic**: Uses 10-second averaging and hysteresis to prevent oscillations
- **Burst-Fire Control**: Precise power control using SSR with 50Hz half-cycle timing
- **Manual Override**: Switch between automatic and manual control modes

## Hardware Requirements

### Components
- ESP32 development board
- DTSU666 energy meter with Modbus RTU support
- RS485 to TTL converter module
- Solid State Relay (SSR) for heater control
- Electric heater (up to 2000W)

### Connections
```
ESP32 Connections:
- GPIO 16 (RX2) → RS485 RO (Receiver Output)
- GPIO 17 (TX2) → RS485 DI (Driver Input) 
- GPIO 4       → RS485 DE/RE (Direction Enable)
- GPIO 13      → SSR Control Input
- GND          → Common ground
- 3.3V         → RS485 module power
```

### RS485 Wiring to DTSU666
```
RS485 Module    DTSU666 Meter
A+          →   A+ (Terminal 21)
B-          →   B- (Terminal 22)
```

## Software Setup

### Prerequisites
1. Download and install [Arduino IDE](https://www.arduino.cc/en/software)
2. Install ESP32 board package in Arduino IDE:
   - Go to File → Preferences
   - Add this URL to "Additional Board Manager URLs": 
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Go to Tools → Board → Boards Manager
   - Search for "ESP32" and install the package

### Required Libraries
Install these libraries via Arduino IDE Library Manager (Tools → Manage Libraries):
- `ModbusMaster` by Doc Walker

### Configuration
1. Open `sketch_sep13a/sketch_sep13a.ino` in Arduino IDE
2. Modify WiFi settings (lines 29-32):
   ```cpp
   const char* WIFI_SSID = "Your_WiFi_Name";
   const char* WIFI_PASS = "Your_WiFi_Password";
   IPAddress local_IP(192,168,1,50);  // Set desired static IP
   ```
3. Adjust Modbus settings if needed (lines 35-38)

### Upload Process
1. Connect ESP32 to computer via USB
2. Select board: Tools → Board → ESP32 Arduino → ESP32 Dev Module
3. Select correct COM port: Tools → Port → (your ESP32 port)
4. Click Upload button or press Ctrl+U

## How It Works

### Power Control Logic
The system implements a stabilized control algorithm:

1. **Measurement Phase**: Reads power data from DTSU666 meter every second
2. **Averaging**: Maintains 10-second rolling average to smooth out fluctuations  
3. **Decision Making**: Makes control decisions every 10 seconds based on average export power
4. **Hysteresis Control**: Uses wide hysteresis band (50-250W) to prevent oscillations
5. **Power Adjustment**: Adjusts heater power in 50W steps for stability

### Control Thresholds
- **Export < 50W**: Decrease heater power by 50W (not enough excess power)
- **Export 50-249W**: No change (stable zone with hysteresis)
- **Export ≥ 250W**: Increase heater power by 50W (significant excess power)
- **Reserve**: Always maintains minimum 100W export to prevent grid import

### SSR Control
- Uses burst-fire control for precise power regulation
- 1-second control window divided into 100 half-cycles (50Hz)
- Duty cycle determines how many half-cycles SSR is ON
- Smooth transitions via exponential moving average filter

## Web Interface

Access the control interface by navigating to the ESP32's IP address in a web browser.

### Features
- Real-time power monitoring (grid import, export, heater power)
- Automatic/Manual mode switching
- Manual power control slider (0-100%)
- Live charts showing 60-second history
- System status and parameter display

### API Endpoints
- `/` - Main web interface
- `/data.json` - Real-time power data
- `/ctrl.json` - Control system status
- `/set?mode=auto&duty=50` - Set control parameters

## System Parameters

### Configurable Constants
```cpp
const float P_MAX = 2000.0f;           // Maximum heater power (W)
const float EXPORT_RESERVE_W = 100.0f; // Minimum export reserve (W)
const float STEP_W = 50.0f;            // Power adjustment step (W)
const float EXPORT_LOW = 50.0f;        // Lower threshold (W)
const float EXPORT_HIGH = 250.0f;      // Upper threshold (W)
```

### Timing Parameters
```cpp
const unsigned long DECISION_INTERVAL_MS = 10000; // Decision interval (ms)
const int AVG_BUFFER_SIZE = 10;                   // Averaging buffer size
const float TAU_UP = 8.0f;                        // Rise time constant
const float TAU_DOWN = 5.0f;                      // Fall time constant
```

## Troubleshooting

### Common Issues
1. **No Modbus Communication**: Check RS485 wiring and termination resistors
2. **WiFi Connection Failed**: Verify SSID/password and signal strength
3. **SSR Not Switching**: Check GPIO 13 connection and SSR power supply
4. **Unstable Control**: Adjust TAU_UP/TAU_DOWN constants for smoother response

### Serial Monitor Output
Enable Serial Monitor (115200 baud) to see:
- System initialization messages
- WiFi connection status
- Power adjustment decisions
- Modbus communication errors

---

# System Kontroli Grzałki ESP32

Inteligentny system kontroli grzałki dla ESP32, który automatycznie zarządza nadwyżkami energii z paneli słonecznych poprzez sterowanie grzałką elektryczną za pomocą przekaźnika półprzewodnikowego (SSR) z kontrolą burst-fire.

## Funkcje

- **Automatyczne Zarządzanie Mocą**: Monitoruje pobór/eksport z sieci i automatycznie dostosowuje moc grzałki do zużycia nadwyżki energii słonecznej
- **Integracja Modbus**: Odczytuje dane mocy z licznika energii DTSU666 przez RS485/Modbus RTU
- **Interfejs WWW**: Monitorowanie i kontrola w czasie rzeczywistym przez przeglądarkę internetową
- **Stabilna Logika**: Używa 10-sekundowego uśredniania i histerezy aby zapobiec oscylacjom
- **Kontrola Burst-Fire**: Precyzyjna kontrola mocy za pomocą SSR z taktowaniem półokresów 50Hz
- **Ręczne Przesterowanie**: Przełączanie między trybem automatycznym a ręcznym

## Wymagania Sprzętowe

### Komponenty
- Płytka rozwojowa ESP32
- Licznik energii DTSU666 z obsługą Modbus RTU
- Moduł konwertera RS485 na TTL
- Przekaźnik półprzewodnikowy (SSR) do sterowania grzałką
- Grzałka elektryczna (do 2000W)

### Połączenia
```
Połączenia ESP32:
- GPIO 16 (RX2) → RS485 RO (Wyjście Odbiornika)
- GPIO 17 (TX2) → RS485 DI (Wejście Nadajnika)
- GPIO 4       → RS485 DE/RE (Sterowanie Kierunkiem)
- GPIO 13      → Wejście Sterowania SSR
- GND          → Masa wspólna
- 3.3V         → Zasilanie modułu RS485
```

### Okablowanie RS485 do DTSU666
```
Moduł RS485     Licznik DTSU666
A+          →   A+ (Terminal 21)
B-          →   B- (Terminal 22)
```

## Konfiguracja Oprogramowania

### Wymagania Wstępne
1. Pobierz i zainstaluj [Arduino IDE](https://www.arduino.cc/en/software)
2. Zainstaluj pakiet płytek ESP32 w Arduino IDE:
   - Idź do Plik → Preferencje
   - Dodaj ten URL do "Dodatkowe adresy URL menedżera płytek":
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Idź do Narzędzia → Płytka → Menedżer Płytek
   - Wyszukaj "ESP32" i zainstaluj pakiet

### Wymagane Biblioteki
Zainstaluj te biblioteki przez Menedżer Bibliotek Arduino IDE (Narzędzia → Zarządzaj Bibliotekami):
- `ModbusMaster` autorstwa Doc Walker

### Konfiguracja
1. Otwórz `sketch_sep13a/sketch_sep13a.ino` w Arduino IDE
2. Zmodyfikuj ustawienia WiFi (linie 29-32):
   ```cpp
   const char* WIFI_SSID = "Nazwa_Twojego_WiFi";
   const char* WIFI_PASS = "Hasło_WiFi";
   IPAddress local_IP(192,168,1,50);  // Ustaw żądane statyczne IP
   ```
3. Dostosuj ustawienia Modbus w razie potrzeby (linie 35-38)

### Proces Wgrywania
1. Połącz ESP32 z komputerem przez USB
2. Wybierz płytkę: Narzędzia → Płytka → ESP32 Arduino → ESP32 Dev Module
3. Wybierz właściwy port COM: Narzędzia → Port → (port twojego ESP32)
4. Kliknij przycisk Upload lub naciśnij Ctrl+U

## Jak to Działa

### Logika Kontroli Mocy
System implementuje stabilny algorytm sterowania:

1. **Faza Pomiaru**: Odczytuje dane mocy z licznika DTSU666 co sekundę
2. **Uśrednianie**: Utrzymuje 10-sekundową średnią kroczącą aby wygładzić fluktuacje
3. **Podejmowanie Decyzji**: Podejmuje decyzje sterowania co 10 sekund na podstawie średniego eksportu
4. **Kontrola Histerezy**: Używa szerokiego pasma histerezy (50-250W) aby zapobiec oscylacjom
5. **Dostosowanie Mocy**: Dostosowuje moc grzałki w krokach 50W dla stabilności

### Progi Sterowania
- **Eksport < 50W**: Zmniejsz moc grzałki o 50W (za mało nadwyżki mocy)
- **Eksport 50-249W**: Bez zmian (strefa stabilna z histerezą)
- **Eksport ≥ 250W**: Zwiększ moc grzałki o 50W (znacząca nadwyżka mocy)
- **Rezerwa**: Zawsze utrzymuje minimum 100W eksportu aby zapobiec poborowi z sieci

### Sterowanie SSR
- Używa kontroli burst-fire dla precyzyjnej regulacji mocy
- Okno sterowania 1-sekundowe podzielone na 100 półokresów (50Hz)
- Współczynnik wypełnienia określa ile półokresów SSR jest WŁĄCZONY
- Płynne przejścia przez filtr średniej ruchomej wykładniczej

## Interfejs WWW

Uzyskaj dostęp do interfejsu sterowania nawigując do adresu IP ESP32 w przeglądarce internetowej.

### Funkcje
- Monitorowanie mocy w czasie rzeczywistym (pobór z sieci, eksport, moc grzałki)
- Przełączanie trybu Automatyczny/Ręczny
- Suwak ręcznej kontroli mocy (0-100%)
- Wykresy na żywo pokazujące 60-sekundową historię
- Wyświetlanie stanu systemu i parametrów

### Punkty Końcowe API
- `/` - Główny interfejs WWW
- `/data.json` - Dane mocy w czasie rzeczywistym
- `/ctrl.json` - Stan systemu sterowania
- `/set?mode=auto&duty=50` - Ustawienie parametrów sterowania

## Parametry Systemu

### Konfigurowalne Stałe
```cpp
const float P_MAX = 2000.0f;           // Maksymalna moc grzałki (W)
const float EXPORT_RESERVE_W = 100.0f; // Minimalna rezerwa eksportu (W)
const float STEP_W = 50.0f;            // Krok dostosowania mocy (W)
const float EXPORT_LOW = 50.0f;        // Próg dolny (W)
const float EXPORT_HIGH = 250.0f;      // Próg górny (W)
```

### Parametry Czasowe
```cpp
const unsigned long DECISION_INTERVAL_MS = 10000; // Interwał decyzji (ms)
const int AVG_BUFFER_SIZE = 10;                   // Rozmiar bufora uśredniania
const float TAU_UP = 8.0f;                        // Stała czasowa narastania
const float TAU_DOWN = 5.0f;                      // Stała czasowa opadania
```

## Rozwiązywanie Problemów

### Częste Problemy
1. **Brak Komunikacji Modbus**: Sprawdź okablowanie RS485 i rezystory terminujące
2. **Błąd Połączenia WiFi**: Zweryfikuj SSID/hasło i siłę sygnału
3. **SSR się nie Przełącza**: Sprawdź połączenie GPIO 13 i zasilanie SSR
4. **Niestabilne Sterowanie**: Dostosuj stałe TAU_UP/TAU_DOWN dla płynniejszej odpowiedzi

### Wyjście Monitora Szeregowego
Włącz Monitor Szeregowy (115200 baud) aby zobaczyć:
- Komunikaty inicjalizacji systemu
- Stan połączenia WiFi
- Decyzje dostosowania mocy
- Błędy komunikacji Modbus