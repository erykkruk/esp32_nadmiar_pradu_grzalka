# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32 Arduino project for a smart heater control system that automatically consumes excess solar power by controlling an electric heater via SSR (Solid State Relay) using burst-fire control.

## Build & Upload

**Arduino IDE:**
1. Board: ESP32 Dev Module (Tools → Board → ESP32 Arduino → ESP32 Dev Module)
2. Upload: Ctrl+U or Upload button

**PlatformIO (if configured):**
```bash
pio run -t upload
```

## Hardware Configuration

- **RS485/Modbus:** Serial2 (RX=GPIO16, TX=GPIO17), DE/RE=GPIO4, 9600bps O-8-1
- **SSR Control:** GPIO13 (burst-fire, 50Hz half-cycle timing)
- **Energy Meter:** DTSU666 via Modbus RTU, slave ID=1

## Required Library

- `ModbusMaster` by Doc Walker (install via Arduino Library Manager)

## Architecture

Single-file Arduino sketch (`sketch_sep13a/sketch_sep13a.ino`) with these main components:

### Power Reading
- `pollMeter()` - Reads total active power from DTSU666 register 0x2012 every 1 second
- 10-sample circular buffer (`exportBuffer`) for rolling average calculation
- Positive power = grid import, negative = grid export

### Control Logic (AUTO mode)
- Decision interval: 10 seconds using averaged export data
- Hysteresis band: 50-250W export (no change in this range)
- Power step: ±50W adjustments
- Export reserve: maintains minimum 100W export to prevent grid import

### SSR Burst-Fire Control
- 1-second control window divided into 100 half-cycles (50Hz)
- `halfCycleTick()` runs every 10ms to toggle SSR based on duty cycle
- Exponential moving average filter for smooth power transitions (TAU_UP=8s, TAU_DOWN=5s)

### Web Interface
- Static IP: configurable in code (default 192.168.255.50)
- Endpoints: `/` (UI), `/data.json` (measurements), `/ctrl.json` (control state), `/set` (settings)
- Real-time charts showing 60-second history

## Key Constants

```cpp
P_MAX = 2000.0f         // Maximum heater power (W)
EXPORT_RESERVE_W = 100.0f  // Minimum export reserve
STEP_W = 50.0f          // Power adjustment step
EXPORT_LOW = 50.0f      // Lower hysteresis threshold
EXPORT_HIGH = 250.0f    // Upper hysteresis threshold
DECISION_INTERVAL_MS = 10000  // Decision interval
```

## Configuration

WiFi and network settings are at lines 31-34 in the sketch. Modify before upload:
```cpp
const char* WIFI_SSID = "Your_WiFi";
const char* WIFI_PASS = "Your_Password";
IPAddress local_IP(192,168,1,50);
```

## Debugging

Serial Monitor at 115200 baud shows:
- Power adjustment decisions (in English and Polish)
- Modbus communication status
- System initialization messages
