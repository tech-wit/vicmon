# Victron BLE Vehicle Monitor - Project Specification

## Overview
Build a standalone ESP32-based display system to monitor Victron Bluetooth devices in a 4WD vehicle. No Home Assistant, no internet required after initial setup. Master device aggregates all data and distributes to slave displays.

## Hardware Requirements

### Master Device
- ESP32-S3 microcontroller (BLE + WiFi capable)
- 3.5" LCD display (ILI9341 or ST7789, 320x480 resolution)
- Touchscreen capability (optional but preferred)
- Physical buttons for navigation (2-4 buttons minimum)
- Mountable in vehicle (consider vibration resistance, cable management)

### Slave Devices (1-2 units)
- ESP32-S3 microcontroller
- Smaller display (1.54" - 2.4" TFT or e-ink)
- Physical buttons (1-2 for navigation)
- Compact form factor for mounting at different locations

### Display Options
- **Master:** Waveshare ESP32-S3 3.5" LCD with touchscreen (Guition JC3248W535)
- **Slaves:** ESP32 with 1.54" SPI TFT or e-ink display
- Alternative: Any ESP32-S3 with SPI display module that has LVGL support

## Software Architecture

### Master Device
1. **BLE Scanner**
   - Scan for Victron Smart Bluetooth devices
   - Support multiple device types:
     - Battery monitor (BMV-700/712 series or SmartShunt)
     - Solar charge controller (SmartSolar MPPT)
     - DC-DC converter
   - Handle BLE connection management (ESP32 can maintain ~5 connections)
   - AES decryption for Victron encrypted BLE advertisements

2. **Data Aggregation**
   - Collect data from all connected devices
   - Calculate derived metrics:
     - Battery State of Charge (%)
     - Charge/Discharge current (A)
     - Solar input current (A)
     - DC-DC converter status (if present)
     - System health indicators

3. **WiFi AP**
   - Run as WiFi access point
   - Serve simple HTTP API for slave devices to poll
   - Provide captive portal for initial configuration
   - Configurable AP name and password

4. **Display Interface**
   - Real-time dashboard with:
     - Large battery percentage indicator
     - Charge/discharge current (positive/negative)
     - Solar input current
     - System status (charging, discharging, idle)
   - Dark theme for vehicle use
   - Touch navigation (if touchscreen available)
   - Physical button navigation

### Slave Devices
1. **WiFi Client**
   - Connect to master's WiFi AP
   - Poll master HTTP API every 5-10 seconds
   - Handle reconnection if master goes offline

2. **Simplified Display**
   - Show battery SoC
   - Show charge/discharge current
   - Optional: system status indicator
   - Update frequency: every 5-10 seconds

3. **Configuration**
   - WiFi credentials stored in flash
   - Auto-connect to master on boot
   - Simple button to trigger reconnection

## Communication Protocol

### Master to Slaves
- **Primary:** HTTP API on master WiFi AP
  - `GET /api/data` returns JSON with all monitored values
  - Response format:
    ```json
    {
      "battery_soc": 75,
      "battery_current": -5.2,
      "solar_current": 3.8,
      "dc_dc_status": "active",
      "timestamp": 1234567890
    }
    ```

- **Alternative:** ESP-NOW for zero-latency peer-to-peer (if WiFi not available)
  - ESP32-S3 supports ESP-NOW natively
  - No WiFi infrastructure needed
  - Broadcast mode for multiple slaves

### BLE Data Collection
- Use existing Victron BLE library (e.g., `VictronSolarDisplayEsp` approach)
- Each device has unique AES key (user provides during setup)
- Poll devices every 10-30 seconds (they update slowly anyway)
- Cache last known values for display continuity

## Initial Setup Process
1. User powers on master device
2. Master creates WiFi AP ("Victron-Monitor-Setup")
3. User connects to AP via phone/laptop
4. User enters AES keys for each Victron device
5. Master scans and connects to devices
6. Slaves auto-connect to master's normal WiFi AP

## Data Points to Monitor
- Battery: Voltage, Current, State of Charge, Time to Go
- Solar: Input Current, Input Power, Panel Voltage
- DC-DC: Input/Output Voltage, Current, Status
- System: Total Energy In/Out, Operating Mode

## Physical Mounting Considerations
- Vibration resistant mounting
- Cable routing for ESP32 boards
- Power supply from vehicle (12V/24V to 5V regulator)
- Display orientation (landscape/portrait based on mounting)
- Cable length from ESP32 to display (if separate)

## Development Phases

### Phase 1: BLE Discovery
- Scan for Victron BLE devices
- Decrypt and parse BLE advertisements
- Display raw data on Serial monitor

### Phase 2: Single Display
- Build master device with display
- Show aggregated data on screen
- Implement navigation/buttons

### Phase 3: WiFi + Slaves
- Master runs WiFi AP
- Slaves connect and poll data
- Multi-device display

### Phase 4: Vehicle Integration
- Physical mounting
- Power supply integration
- Vibration testing
- Final UI polish

## Reference Projects
- https://github.com/wytr/VictronSolarDisplayEsp (existing implementation)
- ESP32 BLE examples for scanning and connecting
- LVGL library for display graphics
- ESP-NOW examples for peer-to-peer

## Notes
- No internet dependency after setup
- Must work in remote/offline environments
- Power efficient (vehicle power supply)
- Reliable BLE connection management
- Handle device going out of BLE range gracefully
