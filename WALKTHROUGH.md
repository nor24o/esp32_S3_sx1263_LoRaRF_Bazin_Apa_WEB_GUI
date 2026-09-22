# Walkthrough: Pond-Tank Consensus Accord Protocol & Autonomous Station Controls

## 1. Overview of Requirements & Architecture

Following the user's specification:
> *"no no pound should not controll main things on the thank but it also should implai its status like it also has buttons for it's operation and if im at that place and want to stop the punt it also should notrify the thank if not received first hand it should notify it at the next message from the thank until they reach a common acord"*

The system architecture and communication protocol were updated and verified live:
1. **Security & Role Boundary**:
   - The Pond Node (`0x03`) is an inflow pumping station and water source. It **does not** control Tank Node (`0x02`) core operational modes (`AUTO`/`MANUAL`) or Tank outflow relays (`P1`, `P2`, `P3`).
   - The Tank Node firmware explicitly enforces this security boundary, rejecting any unauthorized attempts by `NODE_POND` to alter mode or outflow pump states.
2. **Pond Station Local Operation**:
   - The Pond station card on the SCADA dashboard and CLI has its own **Local Station Panel**:
     - `🛑 Local Stop (Notify Tank)`: Stops the pump on-site at the pond and initiates the Tank accord handshake.
     - `▶️ Local Start (Notify Tank)`: Starts the pump on-site at the pond and initiates the Tank accord handshake.
     - Setup controls for simulated reservoir level (0%, 33%, 66%, 100%) and pumping flow rates (30, 60, 100 L/min).
3. **Consensus Accord Protocol (Handshake State Machine)**:
   - **First-Hand Immediate Notification**: When the operator stops or starts the pump locally at the pond station, the Pond Node immediately transmits a direct command and status frame to the Tank Node (`0x02`) over LoRa RF.
   - **Mutual Accord Condition**: Both nodes agree on the pond pump state (`pond_sim.pump_running == tank_node.pond_demand`).
   - **Continuous Retry Handshake**: If the first-hand transmission was not received or if Tank has not yet updated, Pond intercepts every incoming packet from Tank, detects the accord mismatch, and immediately re-notifies Tank until mutual agreement is reached.
   - **Immediate Tank Acknowledgment**: When Tank receives Pond's stop status or command, it updates `gConfig.pond_enabled = false`, saves to flash, and triggers immediate telemetry (`gState.triggerImmediateTelem = true`), transmitting confirmation back to Pond and Gateway to conclude the accord loop.

---

## 2. Technical Implementation Details

### A. ESP32-S3 Firmware (`src/main.cpp`)
- **Permission Enforcement**:
  In `Task_ControlEngine`:
  ```cpp
  if (rx.header.sender_id == NODE_POND) {
    if (rx.payload.cmd.target & (CMD_TARGET_MODE | CMD_TARGET_P1 | CMD_TARGET_P2 | CMD_TARGET_P3)) {
      Serial.printf("[Ctrl] Blocked unauthorized command from Pond (0x03) on Tank core controls!\n");
      continue;
    }
  }
  ```
- **Pond Stop & Status Synchronization**:
  When `NODE_POND` sends `MSG_STATUS` or `MSG_CMD` `{CMD_TARGET_POND}`:
  - If pond pump stopped locally at pond (`pondRunning == false`): Tank automatically clears `gConfig.pond_enabled = false`, sets `gState.pond = false`, saves config to flash, and sets `gState.triggerImmediateTelem = true`.
  - Tank immediately fires confirmation packets back over LoRa RF: both `MSG_STATUS` (`Flags: 0x00`) and `MSG_CMD` `{POND_PUMP -> SET OFF/MANUAL}`.
- **Compiled & Flashed to ESP32-S3 on `COM3`**:
  - `RAM: 15.5%`, `Flash: 27.8%`. Uploaded cleanly in 25s.

### B. Universal Station & SCADA Web GUI (`run.py`)
- **PondSimulator Consensus Engine**:
  - Attributes: `tank_accord` (bool), `accord_retry_count` (int), `last_accord_notify` (timestamp).
  - `set_pump(running, reason, notify_tank_first_hand=True)`:
    - Breaks accord (`tank_accord = False`).
    - Dispatches first-hand LoRa downlinks to `NODE_TANK` (`0x02`): direct `MSG_CMD` `{CMD_TARGET_POND, CMD_ACTION_SET, 1/0}` and `MSG_STATUS` beacon.
  - `check_tank_accord(tank_pond_flag)`:
    - Called on every incoming Tank packet (`MSG_STATUS` flags and `MSG_CMD`).
    - If `tank_pond_flag == pump_running`: accord achieved (`tank_accord = True`)!
    - If mismatch detected: increments `accord_retry_count` and immediately re-notifies Tank over LoRa RF downlinks until accord is established.
- **SCADA Web Dashboard (`http://localhost:8080`)**:
  - **Pond Node Card Accord Banner**:
    - In Sync: `🤝 COMMON ACCORD (Tank 0x02 & Pond in Mutual Agreement)` (green).
    - Syncing: `⚠️ AWAITING TANK ACCORD... (Handshake in progress [Retry #N])` (amber pulse).
  - **Pond Local Station Panel**:
    - `🛑 Stop Pump (Notify Tank)`: calls `/pond local stop`.
    - `▶️ Start Pump (Notify Tank)`: calls `/pond local start`.
    - `📡 Send Status Beacon` and `🔄 Toggle Sim Engine`.
    - Removed intrusive tank controls (Mode, P1-P3).

---

## 3. Live Hardware Verification Results

Live tests executed against ESP32-S3 hardware on `COM3` communicating via SX1302 Concentrator Gateway at `192.168.1.29`:

| Test Event | Protocol Action | Hardware & RF Result | Status |
| :--- | :--- | :--- | :--- |
| **Startup Discrepancy Reconciliation** | Tank previously had `pond_demand = True`. Pond restarted with pump `OFF`. Tank sent telemetry (`Flags: 0x10`). | Pond detected mismatch (`tank=True, pond=False`), logged `⚠️ [AWAITING COMMON ACCORD #1]`, and re-notified Tank with `CMD_TARGET_POND SET OFF` (`6B5A020302100100`). Tank immediately replied with `Flags: 0x00` and `MSG_CMD SET OFF/MANUAL` (`6B5A030202100100`). | **PASS** (`tank_accord = True`) |
| **Local Start at Pond** | Operator entered `/pond local start` (or clicked "Start Pump"). | Pond sent first-hand notification (`6B5A020302100101`). Tank hardware accepted command, engaged pond demand, and replied with `MSG_CMD SET ON/AUTO` (`6B5A030202100101`). | **PASS** (`tank_accord = True`, `pump_running = True`) |
| **Local Stop at Pond** | Operator entered `/pond local stop` (or clicked "Stop Pump"). | Pond sent first-hand stop notification (`6B5A020302100100`). Tank hardware cleared `pond_demand`, updated flash, and immediately replied with `MSG_CMD SET OFF/MANUAL` (`6B5A030202100100`). | **PASS** (`tank_accord = True`, `pump_running = False`) |
| **Security Boundary Verification** | Attempt from Pond to alter Tank mode or outflow pumps. | Blocked at Tank firmware level: `Blocked unauthorized command from Pond (0x03) on Tank core controls!`. Tank mode and pumps unaffected. | **PASS** |
| **SCADA REST API Telemetry** | Polled `http://localhost:8080/api/data`. | `tank_accord: true`, `pump_running: false`, Tank metrics: `pond_demand: false`, `flags: 0`. Live dashboard displays green Common Accord badge. | **PASS** |
