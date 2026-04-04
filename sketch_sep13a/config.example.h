/*
  ============================================================
  KONFIGURACJA ESP32 - SKOPIUJ TEN PLIK JAKO config.h
  ============================================================

  1. Skopiuj ten plik:
     cp config.example.h config.h

  2. Uzupelnij wszystkie pola ponizej w config.h

  3. config.h jest w .gitignore - NIE trafi do repo

  ============================================================
*/

#ifndef CONFIG_H
#define CONFIG_H

// ========================
// WiFi - Twoja siec domowa
// ========================
#define CFG_WIFI_SSID    "TUTAJ_NAZWA_WIFI"       // Nazwa sieci WiFi
#define CFG_WIFI_PASS    "TUTAJ_HASLO_WIFI"        // Haslo do WiFi

// ========================
// Statyczne IP ESP32
// ========================
// Ustaw IP ktore nie koliduje z innymi urzadzeniami w sieci
// i jest poza zakresem DHCP routera
#define CFG_IP_ADDR      192,168,1,50              // IP ESP32
#define CFG_IP_GATEWAY   192,168,1,1               // IP routera (bramka)
#define CFG_IP_SUBNET    255,255,255,0             // Maska podsieci
#define CFG_IP_DNS1      192,168,1,1               // DNS1 (zwykle IP routera)
#define CFG_IP_DNS2      8,8,8,8                   // DNS2 (Google DNS)

// ========================
// Serwer (Next.js na Coolify)
// ========================
// Adres URL webappu - endpoint na ktory ESP wysyla dane co 1s
// Dostaniesz go po deploymencie na Coolify
#define CFG_SERVER_URL   "https://TWOJ-ADRES.coolify.app/api/esp/report"

// Klucz API - MUSI BYC TAKI SAM jak w .env.local webappu (zmienna API_KEY)
// Wygeneruj losowy string, np: openssl rand -hex 16
#define CFG_API_KEY      "TUTAJ_TWOJ_KLUCZ_API"

#endif // CONFIG_H
