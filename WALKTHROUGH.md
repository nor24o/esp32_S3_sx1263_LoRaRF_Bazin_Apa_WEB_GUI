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
- **Always-On Web Server & Home Wi-Fi STA with mDNS**:
  - Web server auto-starts on boot by default (`DEFAULT_WEB_ALWAYS_ON 1`, persistent across reboots via `web always on/off`).
  - Optional Home Wi-Fi connection (`set wifi <ssid> <pass>`) with mDNS responder allowing connection via `http://tanknode.local`.
  - Fallback to standalone SoftAP (`TankNode-Debug`, `192.168.4.1`) if STA is unconfigured.
- **Ultra-Fancy Cyberpunk / SCADA Glassmorphic Web Dashboard**:
  - `data/index.html`: Complete glassmorphic HUD layout with animated header pulse, audio mute toggle, SVG reservoir tank, top inflow jet & splash ripples, 3 outflow pipes with rotating turbine impellers, float sensor buoys, pump cards with auto-mask selectors, pond card, 40-point sparkline canvas, and slide-out settings drawer.
  - `data/style.css`: Modern glassmorphic styles with CSS `@keyframes waveBack`, `@keyframes waveFront`, `@keyframes bubbleRise`, `@keyframes flowSlide`, `@keyframes splashRipple`, `@keyframes spinTurbine`, and `@keyframes streamRight`.
  - `data/app.js`: Web Audio API sound synthesizer (tactile clicks, relay chimes, alert tones), live 40-point sparkline canvas graph, WebSocket streaming, and dynamic setting dispatcher.
- **Gateway Station & Autonomous Pond Simulator (`run.py`)**:
  - Telemetry parser decoding incoming `MSG_STATUS` frames from Tank Node (`0x02`).
  - Pond Node (`0x03`) Simulator intercepting refill demand packets (`MSG_CMD`), simulating pumping at 60 L/min, tracking run time and volume delivered.
  - Interactive commands: `/tank status`, `/tank mode`, `/tank p1..p3`, `/tank pond`, `/pond sim on/off`, `/pond status`.

---

## 2. Verification & Validation

1. **Compilation & Filesystem Packaging**:
   - Packaged LittleFS filesystem image (`pio run --target buildfs`):
     ```text
     Building FS image from 'data' directory to littlefs.bin:
     /app.js (10,957 bytes), /index.html (9,634 bytes), /style.css (12,987 bytes)
     Compressed to 15,761 bytes in LittleFS partition
     [SUCCESS] in 3.42s
     ```
   - Compiled firmware with PlatformIO (`pio run`):
     - **RAM**: `54,348 bytes (16.6%)`
     - **Flash**: `959,201 bytes (28.7%)`
     - **Result**: `[SUCCESS] with 0 errors`

2. **Filesystem & Firmware Flashing**:
   - Flashed LittleFS partition (`pio run --target uploadfs --upload-port COM3`) - verified.
   - Flashed firmware binary (`pio run --target upload --upload-port COM3`) - verified.

3. **Live Hardware Execution & Serial CLI Verification**:
   - Connected to ESP32-S3 over COM3 at 921,600 baud.
   - Verified boot sequence:
     ```text
     [Boot] #28 autoMode=0
     [WebServer] Starting in Always-On mode...
     [DebugWeb] Hotspot online! SSID='TankNode-Debug' Pass='12345678' IP=192.168.4.1
     [mDNS] Responder started: http://tanknode.local
     [WebServer] auto-started by config (Always Available)
     [PCF8574] found
     [LoRa] OK - listening
     ```
   - Verified CLI commands:
     - `config` $\rightarrow$ shows `AlwaysOn:1`, `WiFi-STA:''`.
     - `web always off` $\rightarrow$ `WebServer Always-On DISABLED`.
     - `web always on` $\rightarrow$ `WebServer Always-On ENABLED (will auto-start on boot)`.
     - `set wifi MySSID MySecret` $\rightarrow$ `WiFi STA credentials saved. SSID: MySSID`.
     - `set wifi clear` $\rightarrow$ `WiFi STA credentials cleared`.

4. **Gateway Station & Pond Simulator Automated Tests**:
   - Executed test suite (`scratch/test_gateway_station.py`) through Python:
     ```text
     ─── TEST 1: Decode incoming MSG_STATUS (Tank Node -> Gateway) ───
       [Device Frame] MsgType: 0x01 | Sender: 0x02 → Target: 0x01
       Flags = 0x03 → Auto=True P1=True | Water = 2 | 24.5 °C, 55% RH
     ─── TEST 2: Verify /tank status command ───
       Water Level: 2/3 (MID (66%)) | Mode: AUTO | P1: ON, P2: OFF, P3: OFF
     ─── TEST 3: Pond Node Simulator (Tank Node -> Pond Node) ───
       🌊 [POND SIMULATOR] Tank demanded refill -> POND PUMP ENERGIZED (Rate: 60 L/min)
     ─── TEST 4: Verify /pond status while pumping ───
       Relay: RUNNING (PUMPING) | Rate: 60.0 L/min
     ─── TEST 5: Tank Node commands Pond Node to turn OFF ───
       🛑 [POND SIMULATOR] Tank stopped refill -> Pump stopped. Delivered ~1.0 L in 1.0s
     ─── TEST 6: Verify /pond status after pumping stops ───
       Relay: IDLE (STOPPED) | Total Delivered: ~1.0 Liters
     SUCCESS: All gateway station & pond simulator tests PASSED!
     ```

---

## 3. Git Version History
- `0e35d43`: Baseline snapshot of initial multi-tasking firmware.
- `23db5c2`: Firmware hardening, anti-chatter, slosh filter, NVS preservation.
- `27c76f6`: Comprehensive `README.md` and `WALKTHROUGH.md` documentation.
- `daea6f3`: On-demand Debug Web & WebSocket dashboard in `data/` and expanded Serial CLI.
- *(Current)*: Always-available Cyberpunk SCADA glassmorphic dashboard with dual Wi-Fi/mDNS, and Gateway Station + Autonomous Pond Simulator in `run.py`.
