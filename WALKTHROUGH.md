# Walkthrough: Universal Multi-Node Coordinator & Pond Node Simulator (`run.py`)

We transformed `run.py` into a **Universal LoRa Network Coordinator and Multi-Node Station & Simulator**:
1. **Universal Dynamic Node Registry (`0x01`–`0xFF`)**:
   - Manages all physical and simulated field nodes with automatic over-the-air discovery.
   - Default built-in node profiles for **Gateway Coordinator (`0x01`)**, **Tank Node (`0x02`)**, and **Pond Node (`0x03`)**.
   - Tracks per-node online/offline/timeout health, packet counters, RSSI/SNR signal quality, and live metrics.
2. **Active Pond Node (`0x03`) Simulator with Real LoRa RF Transmissions**:
   - Intercepts remote refill requests (`MSG_CMD` targeting `0x03`) issued by the Tank Node (`0x02`), updates pump state, and accumulates volume at a configurable rate (**60.0 L/min**).
   - **Real Over-the-Air LoRa Transmissions**: Broadcasts genuine binary `MSG_STATUS` frames from `sender_id = 0x03` into the air via the SX1302 gateway downlink (UDP `:5556`), allowing physical nodes to receive real over-the-air responses.
   - Provides full telemetry simulation: reservoir level percentage, solar battery bank (12.8V), solar charging power (45W), and water temperature.
3. **Multi-Node Fleet SCADA Web Dashboard (`http://localhost:8080`)**:
   - Dynamic Cyberpunk grid featuring separate interactive cards for every node in the network.
   - **Interactive Pond Simulator Card**: Live pump relay status, manual start/stop controls, volume counter, flow rate input, reservoir gauge, and "Send LoRa RF Beacon" button.
   - **Interactive Tank Card**: Animated water reservoir graphic, mode tag, outflow pump status, and quick control buttons.
   - **Universal Command Console**: Execute arbitrary station CLI commands directly in the browser.
   - **Decoded Packet Stream**: 1-second auto-refreshing table with node color badges and hex payloads.
   - **REST API**: `GET /api/data`, `GET /api/nodes`, `POST /api/cmd`.
4. **Universal Command Interface**:
   - `/nodes` - Formatted ASCII table of all network nodes.
   - `/node <id> status` - Deep inspection of any node's metrics and signal quality.
   - `/node <id> cmd <t> <a> <v>` - Transmit arbitrary command to any node.
   - `/send <tgt> <src> <typ> <hex>` - Custom packet injection between any two nodes.
   - `/pond status`, `/pond sim start|stop|tx|flow|level|rf`.
   - `/tank status`, `/tank mode`, `/tank p1|p2|p3|pond`.
   - `/gui` or `/web` - Open the Fleet Web Dashboard in the browser.

---

## Verification & Validation

1. **Automated Test Suite (`scratch/test_suite.py`)**:
   - `NodeRegistry`: verified initial nodes (`0x01`, `0x02`, `0x03`), and verified auto-discovery of dynamically introduced node `0x04`.
   - `PondSimulator`: verified state transitions, volume integration at 60 L/min, and binary `MSG_STATUS` frame generation.
   - Packet Decoder: tested decoding Tank Node status frames and command interception by the Pond simulator.
   - Delta Detection: verified `🔥 [DELTA DETECTED - Node 0x02] P1: OFF -> ON`.
   - `[SUCCESS] in 1.12 seconds`.

2. **Web Server & REST API Tests (`scratch/test_web.py`)**:
   - `GET /`: Fleet HTML loaded with status 200.
   - `GET /api/data`: returned live nodes dictionary, pond simulator state, and packet stream.
   - `GET /api/nodes`: returned list of all registered node profiles.
   - `POST /api/cmd`: executed `/nodes` command with `status: ok`.
   - `[SUCCESS] in 0.85 seconds`.

3. **Live Daemon Execution**:
   - `run.py` is actively running as a background daemon process listening on UDP `0.0.0.0:5555` and TCP `0.0.0.0:8080`.
   - Simulated pond pump start/stop verified via live REST API commands.
