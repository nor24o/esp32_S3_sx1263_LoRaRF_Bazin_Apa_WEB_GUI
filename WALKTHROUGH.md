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

### E. Complete Technical Documentation
- Created a comprehensive [README.md](README.md) containing architecture diagrams, wiring pinouts, packed binary packet formats, state machine logic, FreeRTOS task mappings, and serial CLI commands.

---

## 2. Verification & Validation

1. **Compilation**:
   Compiled with PlatformIO CLI (`pio run`):
   - **RAM**: `20,776 bytes (6.3%)`
   - **Flash**: `336,245 bytes (10.1%)`
   - **Result**: `[SUCCESS] in 11.89 seconds with 0 errors`

2. **Flashing & Hardware Test**:
   - Flashed to ESP32-S3 on `COM3` at 460,800 baud.
   - Connected live serial monitor at 921,600 baud:
     ```text
     [Status] Up:18s WL:2 Mode:MAN Charge Latch:0 Pond:0 | P1:OFF P2:OFF P3:OFF | GW:0s Err:0
     [Ctrl] Dispatched remote Pond charge state broadcast via LoRa -> 0
     [LoRa TX] type=2 target=0x03
     [LoRa TX] type=1 target=0x01
     ```
   - Verified that PCF8574 reads water level 2 reliably with 0 errors.

3. **Git Version History**:
   - `0e35d43`: Baseline snapshot of initial multi-tasking firmware.
   - `23db5c2`: Firmware hardening, anti-chatter, slosh filter, NVS preservation, and comprehensive `README.md`.
