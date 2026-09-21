# Walkthrough: Tank Node Firmware Hardening & Documentation

We upgraded the firmware for the **ESP32-S3 + SX1262 Tank Node** ("Bazin Apă") with robust safety protections, sensor validation, flash wear reduction, and complete documentation.

---

## 1. What Was Completed

### A. Safety & Pump Anti-Chatter Engine
- **Minimum Runtime (`20s`) & Minimum Cooldown (`20s`)**: Implemented [`canChangePumpState()`](src/main.cpp). Pumps cannot toggle on/off rapidly due to rippling water or repeated button presses.
- **Emergency Dry-Run Stop**: If the tank reaches `waterLevel == 0`, minimum runtime is bypassed immediately to shut down pumps and prevent mechanical seal damage from running dry.

### B. Float Switch Plausibility & Wave Debounce Filter
- **Slosh Filter (`1500 ms`)**: Water level transitions must remain stable for `FLOAT_DEBOUNCE_MS` before the system accepts a level change.
- **Plausibility Verification**: Validates that submerged float switches form a contiguous sequence from the bottom up (`0b111` $\rightarrow$ WL:0, `0b110` $\rightarrow$ WL:1, `0b100` $\rightarrow$ WL:2, `0b000` $\rightarrow$ WL:3). If an upper float switch reads submerged while a lower float reads dry (indicating a stuck switch or wire fault), `ERR_FLOAT_PLAUSIBILITY` is raised, `waterLevelValid` is invalidated, and automatic pumps are locked out.
- **I²C Disconnect Fail-Safe**: If the PCF8574 expander fails or is unplugged, `waterLevelValid` is set to `false`, `errorCode = ERR_PCF_I2C` is raised, and LED 4 shines yellow.

### C. LoRa RF Duty Cycle & Airtime Optimization
- **Heartbeat & Event-Driven Telemetry**: Telemetry interval adjusted to 30s (`DEFAULT_TELEMETRY_INTERVAL_MS = 30000UL`) with immediate transmission on state changes, guaranteeing $<1\%$ duty cycle compliance with EU868 regulations.
- **SX1262 FIFO Base Address Reset**: Fixed continuous RX buffer alignment by resetting the base address (`sx126x_setBufferBaseAddress(0, 0)`) in `setForRx()` and before TX.

### D. Flash Memory (NVS) Wear Reduction
- Runtime counters are calculated in RAM and flushed to NVS periodically (every 10 minutes) or upon `reboot`, rather than writing flash on every pump shutoff.
- Config writes use dirty-checking (`memcmp`) to eliminate redundant erase/write cycles.

### F. On-Demand Debug Mode & Web Dashboard
- **Web Assets in `data/`**: Clean separation of frontend files:
  - `data/index.html`: Responsive dark-mode dashboard with animated reservoir tank, level indicators, pump toggle controls, config forms, and live metrics.
  - `data/style.css`: Modern glassmorphic styling, responsive layout, fluid CSS wave animations.
  - `data/app.js`: Bi-directional WebSocket communication (`ws://192.168.4.1:81/`), auto-reconnect, JSON commands, and toast notifications.
- **Embedded PROGMEM Fallback (`src/web_assets.h`)**: Compiled standalone HTML/CSS/JS fallback in flash memory ensuring the web interface functions even if the LittleFS partition is not uploaded.
- **Task_DebugWebServer**: Dedicated FreeRTOS task on Core 0 managing SoftAP (`TankNode-Debug`, `192.168.4.1`), DNS captive portal (port 53), HTTP web server (port 80), and WebSocket server (port 81).
- **Zero Idle Overhead**: WiFi radio is completely powered off (`WIFI_OFF`) when debug mode is disabled, conserving power and preventing RF interference with LoRa.
- **Expanded Serial CLI**: Full remote parameter configuration from Serial terminal (`set runtime`, `set cooldown`, `set telem`, `set mask`, `set pond`, `set mode`, `set defaults`, `debug on/off`, `save`).

---

## 2. Verification & Validation

1. **Compilation & Filesystem Packaging**:
   - Packaged LittleFS filesystem image (`pio run --target buildfs`):
     ```text
     Building FS image from 'data' directory to littlefs.bin:
     /app.js, /index.html, /style.css
     [SUCCESS] in 3.91s
     ```
   - Compiled firmware with PlatformIO (`pio run`):
     - **RAM**: `48,712 bytes (14.9%)`
     - **Flash**: `898,081 bytes (26.9%)`
     - **Result**: `[SUCCESS] with 0 errors`

2. **Filesystem & Firmware Flashing**:
   - Flashed LittleFS partition (`pio run --target uploadfs --upload-port COM3`) - 1.5 MB partition written and verified.
   - Flashed firmware binary (`pio run --target upload --upload-port COM3`) - verified.

3. **Live Hardware Execution & Serial CLI Verification**:
   - Connected to ESP32-S3 over COM3 at 921,600 baud.
   - Verified boot sequence:
     ```text
     [Boot] #25 autoMode=0
     [PCF8574] found
     [LoRa] OK - listening
     [Status] Up:2s WL:0 Mode:MAN Charge Latch:0 Pond:0 | P1:OFF P2:OFF P3:OFF
     ```
   - Tested interactive commands via Serial:
     - `debug on` $\rightarrow$ SoftAP launched: `[DebugWeb] Hotspot online! SSID='TankNode-Debug' Pass='12345678' IP=192.168.4.1`
     - `set runtime 25000` $\rightarrow$ runtime updated to 25000 ms.
     - `set cooldown 15000` $\rightarrow$ cooldown updated to 15000 ms.
     - `set telem 12000` $\rightarrow$ telemetry broadcast updated to 12000 ms.
     - `set mask 1 1 0` $\rightarrow$ auto pump mask updated to P1=1, P2=1, P3=0.
     - `set pond 1` $\rightarrow$ pond enabled, remote packet dispatched via LoRa.
     - `set mode auto` $\rightarrow$ switched to AUTO, turned ON pumps P1 & P2 based on water level.
     - `set defaults` $\rightarrow$ restored default configuration in NVS.
     - `debug off` $\rightarrow$ Hotspot stopped and WiFi powered OFF (`debug=0`).

---

## 3. Git Version History
- `0e35d43`: Baseline snapshot of initial multi-tasking firmware.
- `23db5c2`: Firmware hardening, anti-chatter, slosh filter, NVS preservation.
- `27c76f6`: Comprehensive `README.md` and `WALKTHROUGH.md` documentation.
- *(Current)*: On-demand Debug Mode, Web & WebSocket dashboard in `data/`, PROGMEM fallback, and expanded Serial CLI.
