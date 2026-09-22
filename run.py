import socket
import struct
import threading
import time
import json
import base64
import sys
import webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler

# Ensure UTF-8 output and set terminal window title on Windows
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except Exception:
        pass
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleTitleW("SX1302 LoRa Multi-Node Gateway & Simulator [run.py]")
    except Exception:
        pass

# Import prompt_toolkit components for advanced thread-safe terminal handling
try:
    from prompt_toolkit import PromptSession
    from prompt_toolkit.patch_stdout import patch_stdout
    HAS_PROMPT_TOOLKIT = True
except ImportError:
    HAS_PROMPT_TOOLKIT = False

# ─── Configuration & Network Identity ──────────────────────────────────────────
LISTEN_PORT_UP   = 5555   # SX1302 gateway -> PC (uplink JSON & binary packets)
SEND_PORT_DOWN   = 5556   # PC -> SX1302 gateway (downlink TX JSON packets)
TX_QUEUE_SIZE    = 32     # Hardware TX queue depth
STATION_WEB_PORT = 8080   # Embedded Fleet SCADA Web GUI

NETWORK_MAGIC    = 0x5A6B # Network identifier (0x6B 0x5A in little-endian / wire format)
NODE_GATEWAY     = 0x01   # Gateway / Coordinator ID
NODE_TANK        = 0x02   # Tank Node (Bazin Apă)
NODE_POND        = 0x03   # Pond Node (Iaz / Pompa Alimentare)
NODE_BROADCAST   = 0xFF   # Broadcast to all nodes

# Message Types
MSG_STATUS       = 0x01
MSG_CMD          = 0x02

# Command Targets
CMD_TARGET_MODE  = 0x01
CMD_TARGET_P1    = 0x02
CMD_TARGET_P2    = 0x04
CMD_TARGET_P3    = 0x08
CMD_TARGET_POND  = 0x10

# Command Actions
CMD_ACTION_TOGGLE = 0x00
CMD_ACTION_SET    = 0x01

# SX1302 binary packet headers
PKT_TYPE_HEARTBEAT = 0x02
PKT_TYPE_BOOT      = 0x03
HB_FMT             = ">BQIIIIIIBBII"
BOOT_FMT           = ">BQ32s"
HB_SIZE            = struct.calcsize(HB_FMT)
BOOT_SIZE          = struct.calcsize(BOOT_FMT)

# UDP socket & Runtime Globals
sock            = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
gateway_ip      = None
pkt_counter     = 0
last_hb         = {}
active_web_port = STATION_WEB_PORT
packet_history  = []
telemetry_history = []


# ─── Universal Node Registry ───────────────────────────────────────────────────

class NodeRegistry:
    """
    Universal registry for all devices in the LoRa RF ecosystem.
    Tracks live metrics, online/offline health, hardware roles, and simulation status.
    """
    def __init__(self):
        self.lock = threading.RLock()
        self.nodes = {}
        self._init_defaults()

    def _init_defaults(self):
        # Default known network nodes
        self.register(NODE_GATEWAY, "Gateway Coordinator", "GATEWAY", "SX1302 LoRa Concentrator & UDP Hub", False)
        self.register(NODE_TANK, "Tank Node (Bazin)", "RESERVOIR_CTRL", "ESP32-S3 Level Floats & 3x Outflow Relays", False)
        self.register(NODE_POND, "Pond Node (Iaz)", "INFLOW_PUMP", "Remote Pond Charging Pump & Source Monitor", True)

    def register(self, node_id: int, name: str, n_type: str, role: str, is_simulated: bool = False):
        with self.lock:
            if node_id not in self.nodes:
                self.nodes[node_id] = {
                    "id": node_id,
                    "id_hex": f"0x{node_id:02X}",
                    "name": name,
                    "type": n_type,
                    "role": role,
                    "status": "SIMULATED" if is_simulated else "OFFLINE",
                    "is_simulated": is_simulated,
                    "last_seen": 0,
                    "pkt_count": 0,
                    "last_rssi": None,
                    "last_snr": None,
                    "metrics": {},
                    "prev_metrics": {}
                }
            return self.nodes[node_id]

    def get(self, node_id: int):
        with self.lock:
            if node_id in (0x00, 0xFF):
                return {
                    "id": node_id,
                    "id_hex": f"0x{node_id:02X}",
                    "name": "Broadcast (All Nodes)" if node_id == 0xFF else "Null",
                    "type": "BROADCAST",
                    "role": "LoRa Broadcast Destination",
                    "status": "VIRTUAL",
                    "is_simulated": False,
                    "last_seen": 0,
                    "pkt_count": 0,
                    "last_rssi": None,
                    "last_snr": None,
                    "metrics": {},
                    "prev_metrics": {}
                }
            if node_id not in self.nodes:
                # Auto-discover previously unseen node in the air
                self.nodes[node_id] = {
                    "id": node_id,
                    "id_hex": f"0x{node_id:02X}",
                    "name": f"Field Node 0x{node_id:02X}",
                    "type": "GENERIC_NODE",
                    "role": "Auto-discovered LoRa Field Node",
                    "status": "ONLINE",
                    "is_simulated": False,
                    "last_seen": time.time(),
                    "pkt_count": 0,
                    "last_rssi": None,
                    "last_snr": None,
                    "metrics": {},
                    "prev_metrics": {}
                }
            return self.nodes[node_id]

    def touch(self, node_id: int, rssi: float = None, snr: float = None):
        if node_id in (0x00, 0xFF):
            return
        with self.lock:
            node = self.get(node_id)
            node["last_seen"] = time.time()
            node["pkt_count"] += 1
            if not node["is_simulated"]:
                node["status"] = "ONLINE"
            if rssi is not None:
                node["last_rssi"] = rssi
            if snr is not None:
                node["last_snr"] = snr

    def update_metrics(self, node_id: int, metrics: dict):
        if node_id in (0x00, 0xFF):
            return
        with self.lock:
            node = self.get(node_id)
            node["prev_metrics"] = dict(node["metrics"])
            node["metrics"].update(metrics)
            node["last_seen"] = time.time()
            if not node["is_simulated"]:
                node["status"] = "ONLINE"

    def get_all(self):
        with self.lock:
            now = time.time()
            res = {}
            for nid, node in self.nodes.items():
                copy_node = dict(node)
                # Compute online status based on timeout if physical node
                if not copy_node["is_simulated"] and nid != NODE_GATEWAY:
                    if copy_node["last_seen"] == 0:
                        copy_node["status"] = "OFFLINE"
                    elif (now - copy_node["last_seen"]) > 65:
                        copy_node["status"] = "TIMEOUT"
                    else:
                        copy_node["status"] = "ONLINE"
                elif nid == NODE_GATEWAY:
                    copy_node["status"] = "ONLINE" if gateway_ip else "WAITING"
                res[nid] = copy_node
            return res

node_registry = NodeRegistry()


# ─── Autonomous Pond Node Simulator (0x03) ────────────────────────────────────

class PondSimulator:
    """
    Simulates the physical Pond Node (0x03).
    - Tracks pump state, water delivery volume, flow rate, pond water level, and power metrics.
    - Transmits real LoRa RF MSG_STATUS packets via gateway downlink back onto the RF network!
    - Responds immediately to LoRa MSG_CMD refill requests from the Tank Node.
    """
    def __init__(self):
        self.lock = threading.RLock()
        self.enabled = True
        self.rf_tx_enabled = True       # Broadcast real LoRa packets into the air via gateway
        self.tx_interval_s = 30         # Periodic status broadcast interval
        self.pump_running = False
        self.pump_started_at = None
        self.total_runtime_s = 0.0
        self.flow_lpm = 60.0            # Nominal pump capacity in Liters/min
        self.pond_level_pct = 88.0      # Pond reservoir volume percentage
        self.battery_v = 12.8           # Solar battery bank voltage
        self.solar_w = 45.0             # Solar array charging wattage
        self.water_temp_c = 8.5         # Pond water temperature
        self.last_tx_time = 0.0
        self.tank_accord = True         # Mutual agreement between Tank (0x02) and Pond (0x03)
        self.accord_retry_count = 0     # Retries dispatched awaiting Tank consensus
        self.last_accord_notify = 0.0

    def set_pump(self, running: bool, reason: str = "", notify_tank_first_hand: bool = True):
        with self.lock:
            now = time.time()
            # Low-Pond Reservoir Safety Protection Cutoff (< 20%)
            if running and self.pond_level_pct < 20.0:
                safe_print(f"  ⚠️ [POND RESERVOIR PROTECTION] Refused pump start ({reason}): Pond level critically low ({self.pond_level_pct:.1f}% < 20%)!")
                running = False
                notify_tank_first_hand = True  # Ensure Tank is informed that refill cannot proceed

            state_changed = (running != self.pump_running)
            self.pump_running = running
            if state_changed:
                if running:
                    self.pump_started_at = now
                    safe_print(f"  🌊 [POND SIMULATOR] Pump ENERGIZED ({reason}) | Rate: {self.flow_lpm} L/min")
                else:
                    dur = (now - self.pump_started_at) if self.pump_started_at else 0
                    self.total_runtime_s += dur
                    delivered = (dur / 60.0) * self.flow_lpm
                    self.pump_started_at = None
                    safe_print(f"  🛑 [POND SIMULATOR] Pump STOPPED ({reason}) | Delivered ~{delivered:.1f} L in {dur:.1f}s")
                
                # Mark accord pending until Tank acknowledges / synchronizes state
                self.tank_accord = False
                self.accord_retry_count = 0
                self._update_registry()

            # First-hand notification directly to Tank Node over LoRa RF
            if notify_tank_first_hand and self.rf_tx_enabled and self.enabled:
                safe_print(f"  🤝 [POND -> TANK FIRST-HAND] Dispatched local pump state={('ON' if running else 'OFF')} to Tank (0x02) | Awaiting Accord...")
                # 1. Send direct command to align Tank's demand switch
                threading.Thread(target=send_node_cmd, args=(NODE_POND, NODE_TANK, CMD_TARGET_POND, CMD_ACTION_SET, 1 if running else 0), daemon=True).start()
                # 2. Broadcast direct Status beacon to Tank and RF network
                threading.Thread(target=self.send_status_beacon, kwargs={"target_id": NODE_TANK, "reason": f"First-hand: {reason}"}, daemon=True).start()
                self.last_accord_notify = now

    def check_tank_accord(self, tank_pond_flag: bool):
        """
        Consensus Accord Handshake Protocol:
        Verifies whether Tank Node (0x02) has reached common accord with Pond Node (0x03).
        If Tank still expects Pond ON when Pond was stopped locally (or vice versa), Pond
        re-notifies Tank on EVERY incoming Tank message until common accord is reached!
        """
        with self.lock:
            if not self.enabled:
                return

            in_accord = (tank_pond_flag == self.pump_running)
            if in_accord:
                if not self.tank_accord:
                    self.tank_accord = True
                    safe_print(f"\n  🤝 [COMMON ACCORD REACHED] Tank Node (0x02) & Pond Node (0x03) in full agreement! (Pond Pump: {'RUNNING' if self.pump_running else 'STOPPED'}, Handshake Retries: {self.accord_retry_count})\n")
            else:
                self.tank_accord = False
                self.accord_retry_count += 1
                safe_print(f"\n  ⚠️ [AWAITING COMMON ACCORD #{self.accord_retry_count}] Tank Node reports Pond={tank_pond_flag}, but Pond is actually {self.pump_running}!")
                safe_print(f"  🔄 [ACCORD HANDSHAKE RETRY] Re-notifying Tank Node (0x02) at incoming message over LoRa RF...")
                if self.rf_tx_enabled:
                    threading.Thread(target=send_node_cmd, args=(NODE_POND, NODE_TANK, CMD_TARGET_POND, CMD_ACTION_SET, 1 if self.pump_running else 0), daemon=True).start()
                    threading.Thread(target=self.send_status_beacon, kwargs={"target_id": NODE_TANK, "reason": f"Accord handshake retry #{self.accord_retry_count}"}, daemon=True).start()
                self.last_accord_notify = time.time()

            self._update_registry()

    def get_metrics(self):
        with self.lock:
            now = time.time()
            cur_dur = (now - self.pump_started_at) if (self.pump_running and self.pump_started_at) else 0
            tot_dur = self.total_runtime_s + cur_dur
            tot_liters = (tot_dur / 60.0) * self.flow_lpm
            return {
                "enabled": self.enabled,
                "rf_tx_enabled": self.rf_tx_enabled,
                "pump_running": self.pump_running,
                "tank_accord": self.tank_accord,
                "accord_retry_count": self.accord_retry_count,
                "cur_dur_s": round(cur_dur, 1),
                "total_runtime_s": round(tot_dur, 1),
                "total_liters": round(tot_liters, 1),
                "flow_lpm": self.flow_lpm,
                "pond_level_pct": round(self.pond_level_pct, 1),
                "battery_v": round(self.battery_v, 2),
                "solar_w": round(self.solar_w, 1),
                "water_temp_c": round(self.water_temp_c, 1),
                "tx_interval_s": self.tx_interval_s
            }

    def _update_registry(self):
        m = self.get_metrics()
        node_registry.update_metrics(NODE_POND, {
            "pump_running": m["pump_running"],
            "tank_accord": m["tank_accord"],
            "accord_retry_count": m["accord_retry_count"],
            "flow_lpm": m["flow_lpm"],
            "total_liters": m["total_liters"],
            "pond_level_pct": m["pond_level_pct"],
            "battery_v": m["battery_v"],
            "solar_w": m["solar_w"],
            "temp_c": m["water_temp_c"]
        })

    def send_status_beacon(self, target_id: int = NODE_BROADCAST, reason: str = "periodic"):
        """
        Builds and transmits a genuine LoRa MSG_STATUS frame from NODE_POND (0x03)
        via the SX1302 gateway downlink (UDP :5556).
        """
        if not self.enabled:
            return False

        with self.lock:
            # Construct 11-byte StatusPayload:
            # [0..1] Magic (6B 5A)
            # [2]    Target ID (0x01 or 0xFF)
            # [3]    Sender ID (0x03 - Pond Node)
            # [4]    MsgType   (0x01 - MSG_STATUS)
            # [5]    Flags     (Bit0: Auto=1, Bit4: Pond Pump running)
            # [6]    Water     (0-3 scaled from pond level %)
            # [7..8] Temp      (int16 little endian = temp_c * 10)
            # [9]    Humidity  (level % 0-100)
            # [10]   Reserved  (battery deci-volts, e.g. 128 = 12.8V)
            flags = 0x01 | (0x10 if self.pump_running else 0x00)
            water_step = 3 if self.pond_level_pct >= 75 else (2 if self.pond_level_pct >= 50 else (1 if self.pond_level_pct >= 25 else 0))
            temp_int = int(self.water_temp_c * 10)
            hum_int = min(100, max(0, int(self.pond_level_pct)))
            batt_dv = min(255, max(0, int(self.battery_v * 10)))

            payload = bytearray()
            payload.extend([0x6B, 0x5A, target_id, NODE_POND, MSG_STATUS])
            payload.append(flags)
            payload.append(water_step)
            payload.extend(struct.pack("<h", temp_int))
            payload.append(hum_int)
            payload.append(batt_dv)

            self.last_tx_time = time.time()

        tx_json = build_custom(bytes(payload), freq=868_100_000, power=14, sf=9, bw=125, cr="4/5", invert=True)
        success = send(tx_json)
        if success:
            safe_print(f"  📡 [POND SIM -> RF TX] Broadcasted MSG_STATUS via LoRa ({reason}) | Pump={'ON' if self.pump_running else 'OFF'} Level={self.pond_level_pct}%")
            # Record in packet history
            record_packet_history("MSG_STATUS (SIM)", f"0x{NODE_POND:02X}", f"0x{target_id:02X}",
                                  f"Pond Sim Beacon: Pump={'ON' if self.pump_running else 'OFF'} Lvl={self.pond_level_pct}% Batt={self.battery_v}V",
                                  bytes(payload).hex().upper())
        return success

    def step(self):
        """Called every second by background simulation loop."""
        if not self.enabled:
            return
        now = time.time()
        with self.lock:
            # Low-Pond Reservoir Safety Protection Cutoff (< 20%)
            if self.pump_running and self.pond_level_pct < 20.0:
                safe_print(f"\n  🚨 [POND RESERVOIR PROTECTION] EMERGENCY SHUTDOWN: Pond level dropped to {self.pond_level_pct:.1f}% (< 20.0%)! Stopping pump and notifying Tank Node (0x02)...")
                self.set_pump(False, reason="Low-Pond Cutoff (<20%)", notify_tank_first_hand=True)
        self._update_registry()
        if self.rf_tx_enabled and (now - self.last_tx_time >= self.tx_interval_s):
            self.send_status_beacon(target_id=NODE_BROADCAST, reason="periodic beacon")

pond_sim = PondSimulator()


# ─── Thread-Safe Terminal Printing & History ──────────────────────────────────

def safe_print(msg: str):
    """Prints logs safely with immediate stdout flush."""
    print(msg)
    sys.stdout.flush()

def record_packet_history(pkt_type: str, sender: str, target: str, summary: str, raw_hex: str):
    packet_history.append({
        "time": time.strftime("%H:%M:%S"),
        "type": pkt_type,
        "sender": sender,
        "target": target,
        "summary": summary,
        "hex": raw_hex
    })
    if len(packet_history) > 120:
        packet_history.pop(0)


# ─── Downlink JSON Builders & Sockets ─────────────────────────────────────────

def build_default(payload: bytes) -> bytes:
    data_b64 = base64.b64encode(payload).decode('utf-8')
    pkt = {
        "imme": True,
        "freq": 868.1,
        "rfch": 0,
        "powe": 14,
        "modu": "LORA",
        "datr": "SF9BW125",
        "codr": "4/5",
        "ipol": True,
        "ncrc": False,
        "size": len(payload),
        "data": data_b64
    }
    return json.dumps(pkt).encode('utf-8')

def build_custom(payload: bytes, freq=868_100_000, power=14, sf=9,
                 bw=125, cr="4/5", invert=True, no_crc=False,
                 imme=True, rfch=0, modu="LORA") -> bytes:
    freq_mhz = freq / 1e6
    data_b64 = base64.b64encode(payload).decode('utf-8')
    datr_str = f"SF{sf}BW{bw}" if modu == "LORA" else str(sf)
    pkt = {
        "imme": imme,
        "freq": round(freq_mhz, 4),
        "rfch": rfch,
        "powe": power,
        "modu": modu,
        "datr": datr_str,
        "codr": cr,
        "ipol": invert,
        "ncrc": no_crc,
        "size": len(payload),
        "data": data_b64
    }
    return json.dumps(pkt).encode('utf-8')

def send(data: bytes) -> bool:
    if gateway_ip is None:
        safe_print("  [!] No gateway seen yet — waiting for gateway packet.")
        return False
    sock.sendto(data, (gateway_ip, SEND_PORT_DOWN))
    return True

def send_node_cmd(sender_id: int, target_id: int, target_byte: int, action_byte: int, val_byte: int):
    """
    Universal Command Dispatcher:
    Sends an 8-byte MSG_CMD frame over LoRa RF from ANY sender to ANY target.
    """
    pkt = bytearray([0x6B, 0x5A, target_id, sender_id, MSG_CMD, target_byte, action_byte, val_byte])
    tx_json = build_custom(bytes(pkt), freq=868_100_000, power=14, sf=9, bw=125, cr="4/5", invert=True)
    if send(tx_json):
        t_name = {0x01: "MODE", 0x02: "P1", 0x04: "P2", 0x08: "P3", 0x10: "POND_PUMP"}.get(target_byte, f"0x{target_byte:02X}")
        a_name = "TOGGLE" if action_byte == 0 else ("SET ON" if val_byte == 1 else "SET OFF")
        safe_print(f"  [TX -> 0x{target_id:02X}] Cmd from 0x{sender_id:02X}: {t_name} -> {a_name}")
        record_packet_history("MSG_CMD (TX)", f"0x{sender_id:02X}", f"0x{target_id:02X}",
                              f"Cmd: {t_name} -> {a_name}", bytes(pkt).hex().upper())
        return True
    return False


# ─── Universal Payload Decoders & Parsers ─────────────────────────────────────

def decode_device_payload(payload_bytes: bytes, rssi: float = None, snr: float = None):
    if len(payload_bytes) < 5:
        return None
        
    magic = payload_bytes[0:2]
    if magic != b'\x6B\x5A':
        return None

    target_id = payload_bytes[2]
    sender_id = payload_bytes[3]
    msg_type  = payload_bytes[4]

    # Touch both nodes in the registry (filter broadcast/null)
    if sender_id not in (0x00, 0xFF):
        node_registry.touch(sender_id, rssi, snr)
    if target_id not in (0x00, 0xFF):
        node_registry.touch(target_id)

    sender_node = node_registry.get(sender_id)
    target_node = node_registry.get(target_id)

    result = [
        f"  │   [LoRa Frame] Msg: 0x{msg_type:02X} | {sender_node['name']} (0x{sender_id:02X}) → {target_node['name']} (0x{target_id:02X})"
    ]

    # 1. Status Telemetry (>= 11 bytes)
    if len(payload_bytes) >= 11:
        flags = payload_bytes[5]
        water = payload_bytes[6]
        temp_raw = int.from_bytes(payload_bytes[7:9], byteorder='little', signed=True)
        humidity = payload_bytes[9]
        reserved = payload_bytes[10]

        auto_mode = bool(flags & 0x01)
        p1 = bool(flags & 0x02)
        p2 = bool(flags & 0x04)
        p3 = bool(flags & 0x08)
        pond_flag = bool(flags & 0x10)

        allowed_p1 = bool(reserved & 0x01) if sender_id == NODE_TANK else None
        allowed_p2 = bool(reserved & 0x02) if sender_id == NODE_TANK else None
        allowed_p3 = bool(reserved & 0x04) if sender_id == NODE_TANK else None

        run_left = [0, 0, 0]
        cd_left  = [0, 0, 0]
        if len(payload_bytes) >= 17 and sender_id == NODE_TANK:
            run_left = [int(payload_bytes[11]), int(payload_bytes[12]), int(payload_bytes[13])]
            cd_left  = [int(payload_bytes[14]), int(payload_bytes[15]), int(payload_bytes[16])]

        # Delta Detection against node's previous metrics
        deltas = []
        prev = sender_node.get("metrics", {})
        if prev:
            if prev.get("water_level") is not None and prev.get("water_level") != water:
                wl_names = ["0/3 (EMPTY)", "1/3 (LOW)", "2/3 (MID)", "3/3 (FULL)"]
                deltas.append(f"Water: {wl_names[min(prev.get('water_level',0),3)]} -> {wl_names[min(water,3)]}")
            if prev.get("auto_mode") is not None and prev.get("auto_mode") != auto_mode:
                deltas.append(f"Mode: {'AUTO' if prev.get('auto_mode') else 'MANUAL'} -> {'AUTO' if auto_mode else 'MANUAL'}")
            if prev.get("p1") is not None and prev.get("p1") != p1:
                deltas.append(f"P1: {'ON' if prev.get('p1') else 'OFF'} -> {'ON' if p1 else 'OFF'}")
            if prev.get("p2") is not None and prev.get("p2") != p2:
                deltas.append(f"P2: {'ON' if prev.get('p2') else 'OFF'} -> {'ON' if p2 else 'OFF'}")
            if prev.get("p3") is not None and prev.get("p3") != p3:
                deltas.append(f"P3: {'ON' if prev.get('p3') else 'OFF'} -> {'ON' if p3 else 'OFF'}")
            if prev.get("pond_demand") is not None and prev.get("pond_demand") != pond_flag:
                deltas.append(f"PondDemand: {'ON' if prev.get('pond_demand') else 'OFF'} -> {'ON' if pond_flag else 'OFF'}")

        # Update node registry
        metrics_dict = {
            "water_level": water,
            "auto_mode": auto_mode,
            "p1": p1, "p2": p2, "p3": p3,
            "pond_demand": pond_flag,
            "temp_c": round(temp_raw / 10.0, 1),
            "humidity": humidity,
            "flags": flags
        }
        if sender_id == NODE_TANK:
            metrics_dict["allowed_p1"] = allowed_p1
            metrics_dict["allowed_p2"] = allowed_p2
            metrics_dict["allowed_p3"] = allowed_p3
            metrics_dict["run_left"] = run_left
            metrics_dict["cd_left"]  = cd_left
        node_registry.update_metrics(sender_id, metrics_dict)

        # Consensus Accord Protocol Verification between Tank (0x02) and Pond (0x03)
        if sender_id == NODE_TANK:
            pond_sim.check_tank_accord(tank_pond_flag=pond_flag)

        record_packet_history("MSG_STATUS", f"0x{sender_id:02X}", f"0x{target_id:02X}",
                              f"Lvl={water}/3 Auto={int(auto_mode)} Relays=[{int(p1)},{int(p2)},{int(p3)}] Locks=[Run:{run_left},Cd:{cd_left}] {temp_raw/10.0:.1f}°C",
                              payload_bytes.hex().upper())

        extra_info = f" (Auto Allowed: P1={int(allowed_p1)} P2={int(allowed_p2)} P3={int(allowed_p3)})" if sender_id == NODE_TANK else ""
        lock_info = f" | RunLock={run_left}s CdLock={cd_left}s" if sender_id == NODE_TANK else ""
        result.extend([
            f"  │   Type       = MSG_STATUS ({len(payload_bytes)} bytes)",
            f"  │   Flags      = 0x{flags:02X} → Auto={auto_mode} P1={p1} P2={p2} P3={p3} POND_PUMP={pond_flag}",
            f"  │   Water      = {water} (level 0-3)",
            f"  │   Temperature= {temp_raw/10.0:.1f} °C",
            f"  │   Humidity   = {humidity} % RH",
            f"  │   Reserved   = 0x{reserved:02X}{extra_info}{lock_info}"
        ])
        if deltas:
            result.append(f"  │   🔥 [DELTA DETECTED - Node 0x{sender_id:02X}] " + " | ".join(deltas))

    # 2. Command Packet (8 bytes)
    elif len(payload_bytes) == 8:
        target = payload_bytes[5]
        action = payload_bytes[6]
        value  = payload_bytes[7]

        target_str = {0x01: "MODE", 0x02: "P1", 0x04: "P2", 0x08: "P3", 0x10: "POND_PUMP"}.get(target, f"0x{target:02X}")
        action_str = {0x00: "TOGGLE", 0x01: "SET"}.get(action, f"0x{action:02X}")
        value_str  = {0x00: "OFF/MANUAL", 0x01: "ON/AUTO"}.get(value, f"0x{value:02X}")

        record_packet_history("MSG_CMD", f"0x{sender_id:02X}", f"0x{target_id:02X}",
                              f"Target={target_str} Act={action_str} Val={value_str}",
                              payload_bytes.hex().upper())

        result.extend([
            f"  │   Type       = MSG_CMD (8 bytes)",
            f"  │   Target     = {target_str}",
            f"  │   Action     = {action_str}",
            f"  │   Value      = {value_str}"
        ])

        # Active Interception by Pond Node Simulator (NODE_POND = 0x03)
        if target_id == NODE_POND and (target & 0x10):
            turn_on = bool(value == 1) if action == 0x01 else not pond_sim.pump_running
            # Verify accord between Tank's command and Pond's state
            pond_sim.check_tank_accord(tank_pond_flag=turn_on)
    else:
        result.append(f"  │   Generic / Custom Payload ({len(payload_bytes)} bytes)")
        record_packet_history(f"MSG_0x{msg_type:02X}", f"0x{sender_id:02X}", f"0x{target_id:02X}",
                              f"Raw {len(payload_bytes)} bytes", payload_bytes.hex().upper())

    return "\n".join(result)

def parse_rxpk_json(text):
    try:
        obj  = json.loads(text)
        pkts = obj.get("rxpk", [])
        lines = []
        for pkt in pkts:
            modu    = pkt.get("modu", "?")
            datr    = pkt.get("datr", "?")
            codr    = pkt.get("codr", "")
            freq    = pkt.get("freq", 0.0)
            rssi_s  = pkt.get("rssis", 0.0)
            rssi_c  = pkt.get("rssi",  0.0)
            lsnr    = pkt.get("lsnr",  0.0)
            foff    = pkt.get("foff",  0)
            size    = pkt.get("size",  0)
            data_b64= pkt.get("data",  "")
            stat    = pkt.get("stat",  0)
            tmst    = pkt.get("tmst",  0)
            chan    = pkt.get("chan",   0)
            rfch    = pkt.get("rfch",  0)

            stat_str = {1: "CRC_OK", -1: "CRC_BAD", 0: "NO_CRC"}.get(stat, str(stat))
            try:
                payload_bytes = base64.b64decode(data_b64 + "==")
                payload_disp_HEX = " ".join(f"{b:02X}" for b in payload_bytes)
                device_decode = decode_device_payload(payload_bytes, rssi=rssi_s, snr=lsnr)
               
                try:
                    payload_disp = f'"{payload_bytes.decode("utf-8")}"'
                except Exception:
                    payload_disp = payload_disp_HEX

                if device_decode:
                    payload_disp = device_decode   
            except Exception:
                payload_disp = data_b64

            codr_disp = f"  CR{codr}" if codr else ""
            lines += [
                f"  ┌─ LoRa RX ─────────────────────────────────────────",
                f"  │ Freq      {freq:.4f} MHz  chan={chan}  rf={rfch}",
                f"  │ Mode      {modu}  {datr}{codr_disp}",
                f"  │ RSSI      sig={rssi_s:.1f} dBm   ch={rssi_c:.1f} dBm",
                f"  │ SNR       {lsnr:.1f} dB   foff={foff} Hz",
                f"  │ CRC       {stat_str}",
                f"  │ tmst      {tmst} µs",
                f"  │ Size      {size} bytes",
                f"  │ Payload   {payload_disp}",
                f"  │ Payload-HEX  {payload_disp_HEX}",
                f"  └───────────────────────────────────────────────────",
            ]
        return "\n".join(lines)
    except Exception as e:
        return f"  [!] Bad JSON: {e}\n  Raw: {text[:200]}"

def parse_heartbeat(data):
    if len(data) < HB_SIZE:
        return f"  [!] Truncated heartbeat ({len(data)} < {HB_SIZE} bytes)"
    (pkt_type, eui, uptime, rx_count, tx_count, crc_err, tx_err, q_drop,
     q_depth, sync_word, rf0_freq, rf1_freq) = struct.unpack_from(HB_FMT, data)

    last_hb.update(dict(uptime=uptime, rx=rx_count, tx=tx_count,
                        crc_err=crc_err, tx_err=tx_err,
                        q_drop=q_drop, q_depth=q_depth,
                        sync_word=sync_word,
                        rf0_freq=rf0_freq, rf1_freq=rf1_freq))

    node_registry.update_metrics(NODE_GATEWAY, {
        "uptime": uptime,
        "rx_count": rx_count,
        "tx_count": tx_count,
        "crc_err": crc_err,
        "tx_err": tx_err,
        "q_depth": q_depth,
        "rf0_mhz": round(rf0_freq/1e6, 3),
        "rf1_mhz": round(rf1_freq/1e6, 3)
    })
    node_registry.touch(NODE_GATEWAY)

    h, m, s = uptime // 3600, (uptime % 3600) // 60, uptime % 60
    q_bar    = "█" * q_depth + "░" * (TX_QUEUE_SIZE - q_depth)
    sync_str = "public(0x34)" if sync_word == 0x34 else f"private(0x{sync_word:02X})"
    return (
        f"  ♥  uptime={h:02d}:{m:02d}:{s:02d}  "
        f"rx={rx_count}  tx={tx_count}  "
        f"crc_err={crc_err}  tx_err={tx_err}  "
        f"q=[{q_bar}]{q_depth}/{TX_QUEUE_SIZE}  "
        f"q_drop={q_drop}  "
        f"sync={sync_str}  "
        f"rf0={rf0_freq/1e6:.3f}MHz  rf1={rf1_freq/1e6:.3f}MHz"
    )

def parse_boot(data):
    if len(data) < BOOT_SIZE:
        return f"  [!] Truncated boot packet ({len(data)} < {BOOT_SIZE} bytes)"
    (pkt_type, eui, version_raw) = struct.unpack_from(BOOT_FMT, data)
    version = version_raw.decode("utf-8", errors="ignore").rstrip("\x00")
    node_registry.update_metrics(NODE_GATEWAY, {"eui": f"{eui:016X}", "version": version})
    node_registry.touch(NODE_GATEWAY)
    return f"  ★  EUI={eui:016X}  HAL={version}"


# ─── Multi-Node Fleet SCADA Web Dashboard ─────────────────────────────────────

FLEET_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SX1302 Universal LoRa Station & Fleet Deck</title>
  <style>
    :root {
      --bg: #070b14;
      --card-bg: rgba(13, 20, 36, 0.85);
      --card-sim: rgba(10, 30, 35, 0.85);
      --border: rgba(0, 240, 255, 0.2);
      --border-sim: rgba(0, 255, 136, 0.3);
      --cyan: #00f0ff;
      --green: #00ff88;
      --amber: #ffaa00;
      --red: #ff3366;
      --purple: #b056ff;
      --text: #e2e8f0;
      --muted: #64748b;
      --font-mono: 'Consolas', 'Fira Code', monospace;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      padding: 20px;
      min-height: 100vh;
      background-image: radial-gradient(circle at 10% 15%, rgba(0, 240, 255, 0.05) 0%, transparent 40%),
                        radial-gradient(circle at 90% 85%, rgba(0, 255, 136, 0.04) 0%, transparent 40%);
    }
    .header {
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      align-items: center;
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 14px 20px;
      margin-bottom: 20px;
      box-shadow: 0 8px 32px rgba(0,0,0,0.5);
      gap: 12px;
    }
    .brand h1 {
      font-family: var(--font-mono);
      font-size: 1.15rem;
      letter-spacing: 0.06em;
      color: #fff;
    }
    .brand p { font-size: 0.72rem; color: var(--muted); margin-top: 3px; }
    .badges { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
    .badge {
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.1);
      padding: 5px 10px;
      border-radius: 6px;
      font-size: 0.72rem;
      font-family: var(--font-mono);
      color: var(--cyan);
    }
    .pulse {
      display: inline-block; width: 8px; height: 8px; border-radius: 50%;
      background: var(--green); box-shadow: 0 0 8px var(--green); margin-right: 6px;
      animation: pulse 1.2s infinite alternate;
    }
    @keyframes pulse { from { opacity: 0.4; } to { opacity: 1; } }

    /* Fleet Grid of Nodes */
    .section-title {
      font-family: var(--font-mono);
      font-size: 0.8rem;
      color: var(--muted);
      letter-spacing: 0.08em;
      margin-bottom: 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
      gap: 16px;
      margin-bottom: 20px;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px;
      box-shadow: 0 6px 20px rgba(0,0,0,0.4);
      display: flex;
      flex-direction: column;
      justify-content: space-between;
    }
    .card-sim {
      border-color: var(--border-sim);
      background: var(--card-sim);
    }
    .card-head {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      margin-bottom: 14px;
      border-bottom: 1px solid rgba(255, 255, 255, 0.06);
      padding-bottom: 8px;
    }
    .node-id-tag {
      font-family: var(--font-mono);
      font-size: 0.7rem;
      padding: 2px 6px;
      border-radius: 4px;
      background: rgba(0, 240, 255, 0.1);
      color: var(--cyan);
      border: 1px solid var(--border);
    }
    .node-name { font-weight: 700; font-size: 0.92rem; color: #fff; margin-top: 2px; }
    .node-role { font-size: 0.68rem; color: var(--muted); font-family: var(--font-mono); }

    .tag {
      padding: 2px 7px;
      border-radius: 4px;
      font-size: 0.68rem;
      font-weight: 700;
      font-family: var(--font-mono);
    }
    .tag-online { background: rgba(0, 255, 136, 0.2); color: var(--green); border: 1px solid rgba(0, 255, 136, 0.4); }
    .tag-sim { background: rgba(0, 240, 255, 0.2); color: var(--cyan); border: 1px solid rgba(0, 240, 255, 0.4); }
    .tag-off { background: rgba(255, 255, 255, 0.06); color: var(--muted); }
    .tag-alert { background: rgba(255, 51, 102, 0.2); color: var(--red); border: 1px solid rgba(255, 51, 102, 0.4); }

    /* Details Rows */
    .details { display: flex; flex-direction: column; gap: 6px; font-family: var(--font-mono); font-size: 0.74rem; }
    .row { display: flex; justify-content: space-between; align-items: center; }
    .row-k { color: var(--muted); }
    .row-v { font-weight: 700; color: #fff; }

    /* Visual Reservoir in Tank Card */
    .tank-view {
      display: flex;
      gap: 14px;
      align-items: center;
      margin-bottom: 12px;
      background: rgba(0, 0, 0, 0.25);
      padding: 10px;
      border-radius: 8px;
    }
    .tank-vessel {
      width: 50px;
      height: 80px;
      border: 2px solid #334155;
      border-radius: 6px;
      background: rgba(0,0,0,0.5);
      position: relative;
      overflow: hidden;
      flex-shrink: 0;
    }
    .water-fill {
      position: absolute; bottom: 0; left: 0; right: 0;
      background: linear-gradient(180deg, rgba(0, 240, 255, 0.8) 0%, rgba(0, 100, 255, 0.9) 100%);
      transition: height 0.5s ease;
    }

    /* Actions toolbar */
    .actions { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 14px; }
    button {
      background: rgba(0, 240, 255, 0.08);
      border: 1px solid var(--border);
      color: #fff;
      padding: 6px 10px;
      border-radius: 6px;
      font-family: var(--font-mono);
      font-size: 0.7rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.15s;
    }
    button:hover { background: rgba(0, 240, 255, 0.2); border-color: var(--cyan); transform: translateY(-1px); }
    button.btn-sim { background: rgba(0, 255, 136, 0.1); border-color: rgba(0, 255, 136, 0.3); color: var(--green); }
    button.btn-sim:hover { background: rgba(0, 255, 136, 0.25); border-color: var(--green); }

    /* Universal Console Deck */
    .cmd-deck {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 14px 18px;
      margin-bottom: 20px;
    }
    .cmd-bar {
      display: flex;
      gap: 10px;
      margin-top: 10px;
    }
    .cmd-input {
      flex: 1;
      background: rgba(0, 0, 0, 0.4);
      border: 1px solid var(--border);
      color: var(--cyan);
      font-family: var(--font-mono);
      padding: 8px 12px;
      border-radius: 6px;
      font-size: 0.8rem;
    }
    .cmd-input:focus { outline: none; border-color: var(--cyan); box-shadow: 0 0 10px rgba(0,240,255,0.3); }

    /* Packet Stream Table */
    .packet-box {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px;
    }
    .table-scroll { max-height: 260px; overflow-y: auto; margin-top: 10px; }
    table { width: 100%; border-collapse: collapse; font-family: var(--font-mono); font-size: 0.72rem; }
    th { background: rgba(255, 255, 255, 0.04); padding: 8px 10px; text-align: left; color: var(--muted); position: sticky; top: 0; }
    td { padding: 6px 10px; border-bottom: 1px solid rgba(255, 255, 255, 0.04); }
    tr:hover { background: rgba(255, 255, 255, 0.03); }

    .toast {
      position: fixed; bottom: 20px; right: 20px;
      background: rgba(13, 20, 36, 0.95); border-left: 4px solid var(--cyan);
      padding: 10px 16px; border-radius: 6px; font-family: var(--font-mono); font-size: 0.75rem;
      box-shadow: 0 4px 16px rgba(0,0,0,0.5); display: none; z-index: 999;
    }
    .alarm-card {
      display: flex; align-items: center; gap: 10px;
      padding: 10px 16px; border-radius: 8px; font-family: var(--font-mono); font-size: 0.76rem;
      margin-bottom: 8px; box-shadow: 0 4px 16px rgba(0,0,0,0.4);
    }
    .alarm-critical {
      background: rgba(255, 51, 102, 0.15); border: 1px solid var(--red); color: #ff99aa;
    }
    .alarm-warning {
      background: rgba(255, 170, 0, 0.15); border: 1px solid var(--amber); color: #ffe099;
    }
  </style>
</head>
<body>
  <div class="header">
    <div class="brand">
      <h1>UNIVERSAL LORA FLEET & SIMULATOR <span style="font-size:0.75rem;color:var(--cyan)">// SX1302 DECK</span></h1>
      <p class="subtitle">Multi-Node Coordinator (UDP :5555 / :5556) & Autonomous LoRa Field Node Simulator</p>
    </div>
    <div class="badges">
      <div class="badge"><span class="pulse"></span><span id="gwIp">Gateway: Waiting...</span></div>
      <div class="badge" style="color:var(--purple);border-color:rgba(176,86,255,0.4)"><span class="pulse" style="background:var(--purple);box-shadow:0 0 8px var(--purple)"></span>PSRAM: 2048 KB (Q-SPI)</div>
      <div class="badge">Nodes: <span id="nodeCount" style="color:#fff">0</span></div>
      <div class="badge">Packets: <span id="pktCount" style="color:#fff">0</span></div>
      <button onclick="refreshData()" title="Refresh Telemetry">🔄 REFRESH</button>
    </div>
  </div>

  <!-- Real-Time Alarms Banner -->
  <div id="alarmContainer" style="display:none;margin-bottom:18px;"></div>

  <div class="section-title">
    <span>🌐 ACTIVE & SIMULATED NODE FLEET</span>
    <span style="font-size:0.7rem;color:var(--muted)">Auto-discovering IDs 0x01–0xFE</span>
  </div>

  <div class="grid" id="nodeGrid">
    <!-- Rendered dynamically by JavaScript -->
  </div>

  <!-- SCADA Real-Time & Historical Telemetry Trend Chart -->
  <div class="cmd-deck" style="padding:16px;margin-bottom:20px;">
    <div class="section-title" style="margin-bottom:8px;">
      <div style="display:flex;align-items:center;gap:8px">
        <span>📈 SCADA REAL-TIME TELEMETRY TRENDS (PSRAM TELEMETRY BUFFER)</span>
        <span class="badge" style="padding:1px 6px;font-size:0.65rem;color:var(--cyan);border-color:var(--cyan)">10-SEC SNAPSHOTS</span>
      </div>
      <div style="display:flex;flex-wrap:wrap;gap:12px;font-size:0.72rem;font-family:var(--font-mono)">
        <span style="color:var(--cyan);display:flex;align-items:center;gap:4px">● Tank Level (%)</span>
        <span style="color:var(--green);display:flex;align-items:center;gap:4px">┄ Pond Reservoir (%)</span>
        <span style="color:var(--amber);display:flex;align-items:center;gap:4px">■ Inflow Pumping Active</span>
      </div>
    </div>
    <div style="width:100%;overflow-x:auto;background:rgba(0,0,0,0.3);border-radius:8px;border:1px solid rgba(255,255,255,0.06);padding:10px 4px 4px 4px;">
      <svg id="trendSvg" viewBox="0 0 900 160" style="width:100%;height:auto;display:block"></svg>
    </div>
  </div>

  <!-- Universal Command Console Deck -->
  <div class="cmd-deck">
    <div class="section-title" style="margin-bottom:6px;">
      <span>⚡ UNIVERSAL COMMAND DISPATCHER</span>
      <span style="font-size:0.7rem;color:var(--muted)">Send LoRa commands to any target ID</span>
    </div>
    <div class="cmd-bar">
      <input type="text" id="cmdInput" class="cmd-input" placeholder="Enter station command (e.g. /tank status, /tank p1 toggle, /pond status, /sim pond tx, /help)" onkeydown="if(event.key==='Enter') executeCmd()">
      <button onclick="executeCmd()">EXECUTE</button>
      <button onclick="sendCmd('/gui')">LAUNCH GUI</button>
    </div>
  </div>

  <!-- Packet Stream Table -->
  <div class="packet-box">
    <div class="section-title" style="margin-bottom:0;">
      <span>📡 LIVE DECODED LORA RF PACKET STREAM</span>
      <span style="font-size:0.7rem;color:var(--muted)">Stream updates every 1s</span>
    </div>
    <div class="table-scroll">
      <table>
        <thead>
          <tr>
            <th>Time</th>
            <th>Type</th>
            <th>Sender → Target</th>
            <th>Decoded Metrics / Summary</th>
            <th>Raw Hex Payload</th>
          </tr>
        </thead>
        <tbody id="pktBody">
          <tr><td colspan="5" style="text-align:center;color:var(--muted);padding:14px;">Waiting for incoming packets from gateway...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <div class="toast" id="toast"></div>

  <script>
    function showToast(msg) {
      const t = document.getElementById('toast');
      t.innerText = msg;
      t.style.display = 'block';
      setTimeout(() => { t.style.display = 'none'; }, 2500);
    }

    function sendCmd(cmdStr) {
      fetch('/api/cmd', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ cmd: cmdStr })
      })
      .then(r => r.json())
      .then(d => {
        showToast(d.msg || 'Command dispatched');
        refreshData();
      })
      .catch(e => showToast('Error sending command'));
    }

    function executeCmd() {
      const el = document.getElementById('cmdInput');
      const val = el.value.trim();
      if (!val) return;
      sendCmd(val);
      el.value = '';
    }

    function refreshData() {
      fetch('/api/data')
        .then(r => r.json())
        .then(d => {
          if (d.gateway_ip) {
            d_gw_ip = d.gateway_ip;
            document.getElementById('gwIp').innerText = 'Gateway: ' + d.gateway_ip;
          }
          if (d.pkt_counter !== undefined) document.getElementById('pktCount').innerText = '#' + d.pkt_counter;
          
          const nodes = d.nodes || {};
          const nodeKeys = Object.keys(nodes);
          document.getElementById('nodeCount').innerText = nodeKeys.length;

          renderAlarms(d.alarms || []);
          renderNodeCards(nodes, d.pond_sim || {}, d.gateway_ip);
          renderTrendChart(d.history || []);

          if (d.packets && d.packets.length > 0) {
            const tbody = document.getElementById('pktBody');
            tbody.innerHTML = d.packets.slice().reverse().map(p => {
              const isSim = p.type.includes('SIM');
              const isStatus = p.type.includes('STATUS');
              const badgeClass = isSim ? 'tag-sim' : (isStatus ? 'tag-online' : 'tag-alert');
              return `
                <tr>
                  <td>${p.time}</td>
                  <td><span class="tag ${badgeClass}">${p.type}</span></td>
                  <td><strong>${p.sender}</strong> → <strong>${p.target}</strong></td>
                  <td>${p.summary}</td>
                  <td style="color:var(--muted)">${p.hex}</td>
                </tr>
              `;
            }).join('');
          }
        })
        .catch(e => console.warn('Fetch error:', e));
    }

    function renderAlarms(alarms) {
      const el = document.getElementById('alarmContainer');
      if (!el) return;
      if (!alarms || alarms.length === 0) {
        el.style.display = 'none';
        el.innerHTML = '';
        return;
      }
      el.style.display = 'block';
      el.innerHTML = alarms.map(a => {
        const isCrit = a.level === 'CRITICAL';
        const cardCls = isCrit ? 'alarm-critical' : 'alarm-warning';
        const pulseColor = isCrit ? 'var(--red)' : 'var(--amber)';
        return `
          <div class="alarm-card ${cardCls}">
            <span class="pulse" style="background:${pulseColor};box-shadow:0 0 8px ${pulseColor}"></span>
            <div style="flex:1">
              <span style="font-weight:bold;letter-spacing:0.04em">${a.title}</span> — <span>${a.msg}</span>
            </div>
          </div>
        `;
      }).join('');
    }

    function renderTrendChart(history) {
      const svg = document.getElementById('trendSvg');
      if (!svg) return;
      const W = 900, H = 160;
      const padL = 40, padR = 25, padT = 22, padB = 26;
      const plotW = W - padL - padR;
      const plotH = H - padT - padB;

      let svgContent = `
        <defs>
          <linearGradient id="tankGrad" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stop-color="#00f0ff" stop-opacity="0.35"/>
            <stop offset="100%" stop-color="#00f0ff" stop-opacity="0.0"/>
          </linearGradient>
        </defs>
      `;

      // Horizontal grid lines: 0, 25, 50, 75, 100%
      [0, 25, 50, 75, 100].forEach(pct => {
        const y = padT + plotH - (pct / 100.0 * plotH);
        svgContent += `
          <line x1="${padL}" y1="${y.toFixed(1)}" x2="${padL + plotW}" y2="${y.toFixed(1)}" stroke="rgba(255,255,255,0.07)" stroke-dasharray="3,3" />
          <text x="${padL - 6}" y="${(y + 3).toFixed(1)}" font-family="monospace" font-size="9" fill="#64748b" text-anchor="end">${pct}%</text>
        `;
      });

      if (!history || history.length < 2) {
        svgContent += `<text x="${W/2}" y="${H/2 + 4}" text-anchor="middle" font-family="monospace" font-size="12" fill="#64748b">Collecting real-time 10-second telemetry data points...</text>`;
        svg.innerHTML = svgContent;
        return;
      }

      const n = history.length;
      const stepX = plotW / (n - 1);

      const tankPts = [];
      const pondPts = [];
      let pumpBars = '';

      history.forEach((pt, i) => {
        const x = padL + i * stepX;
        const tankY = padT + plotH - ((pt.tank_pct || 0) / 100.0 * plotH);
        const pondY = padT + plotH - ((pt.pond_pct !== undefined ? pt.pond_pct : 50) / 100.0 * plotH);
        tankPts.push({ x, y: tankY });
        pondPts.push({ x, y: pondY });

        if (pt.pond_pump) {
          const barW = Math.max(4, stepX);
          pumpBars += `<rect x="${(x - barW/2).toFixed(1)}" y="${(padT + plotH - 10).toFixed(1)}" width="${barW.toFixed(1)}" height="10" fill="rgba(255,170,0,0.5)" rx="2"/>`;
        }
      });

      // Pump activity shading
      svgContent += pumpBars;

      // Tank area fill under line
      const areaD = `M ${tankPts[0].x.toFixed(1)} ${(padT + plotH).toFixed(1)} ` +
                    tankPts.map(p => `L ${p.x.toFixed(1)} ${p.y.toFixed(1)}`).join(' ') +
                    ` L ${tankPts[tankPts.length - 1].x.toFixed(1)} ${(padT + plotH).toFixed(1)} Z`;
      svgContent += `<path d="${areaD}" fill="url(#tankGrad)" />`;

      // Pond line (dashed green)
      const pondLineD = 'M ' + pondPts.map(p => `${p.x.toFixed(1)},${p.y.toFixed(1)}`).join(' L ');
      svgContent += `<path d="${pondLineD}" fill="none" stroke="#00ff88" stroke-width="2" stroke-dasharray="5,3" />`;

      // Tank line (cyan)
      const tankLineD = 'M ' + tankPts.map(p => `${p.x.toFixed(1)},${p.y.toFixed(1)}`).join(' L ');
      svgContent += `<path d="${tankLineD}" fill="none" stroke="#00f0ff" stroke-width="2.5" />`;

      // Draw latest point marker
      const lastTank = tankPts[tankPts.length - 1];
      const lastPond = pondPts[pondPts.length - 1];
      svgContent += `<circle cx="${lastTank.x.toFixed(1)}" cy="${lastTank.y.toFixed(1)}" r="4.5" fill="#00f0ff" stroke="#fff" stroke-width="1.5" />`;
      svgContent += `<circle cx="${lastPond.x.toFixed(1)}" cy="${lastPond.y.toFixed(1)}" r="3.5" fill="#00ff88" stroke="#fff" stroke-width="1.2" />`;

      // Timestamp labels along bottom
      const timeIndices = [0, Math.floor(n / 2), n - 1];
      timeIndices.forEach(idx => {
        const pt = history[idx];
        const x = padL + idx * stepX;
        const anchor = idx === 0 ? "start" : (idx === n - 1 ? "end" : "middle");
        svgContent += `<text x="${x.toFixed(1)}" y="${(H - 6).toFixed(1)}" font-family="monospace" font-size="9" fill="#64748b" text-anchor="${anchor}">${pt.time || ''}</text>`;
      });

      svg.innerHTML = svgContent;
    }

    function renderNodeCards(nodes, pondSim, currentGwIp) {
      const container = document.getElementById('nodeGrid');
      let html = '';

      // Sort nodes by ID: 0x02 (Tank), 0x03 (Pond), 0x01 (Gateway), Others
      const sortedKeys = Object.keys(nodes).map(Number).sort((a, b) => {
        const order = { 2: 1, 3: 2, 1: 3 };
        return (order[a] || (a + 10)) - (order[b] || (b + 10));
      });

      for (const nid of sortedKeys) {
        const n = nodes[nid];
        const isTank = (nid === 2);
        const isPond = (nid === 3);
        const isGateway = (nid === 1);
        const m = n.metrics || {};

        let cardClass = n.is_simulated ? 'card card-sim' : 'card';
        let statusTag = `<span class="tag tag-off">${n.status}</span>`;
        if (n.status === 'ONLINE') statusTag = `<span class="tag tag-online">ONLINE</span>`;
        if (n.status === 'SIMULATED') statusTag = `<span class="tag tag-sim">SIMULATOR ACTIVE</span>`;
        if (n.status === 'TIMEOUT') statusTag = `<span class="tag tag-alert">TIMEOUT</span>`;

        html += `<div class="${cardClass}"><div>`;
        html += `
          <div class="card-head">
            <div>
              <span class="node-id-tag">${n.id_hex}</span>
              <div class="node-name">${n.name}</div>
              <div class="node-role">${n.role}</div>
            </div>
            ${statusTag}
          </div>
        `;

        if (isTank) {
          const wl = m.water_level !== undefined ? m.water_level : 0;
          const heights = [0, 33, 66, 100];
          const labels = ['0/3 (EMPTY)', '1/3 (LOW 33%)', '2/3 (MID 66%)', '3/3 (FULL 100%)'];
          const isAuto = Boolean(m.auto_mode);
          
          let tankAlert = '';
          if (wl === 0 && isAuto) {
            tankAlert = `
              <div style="display:flex;align-items:center;gap:7px;padding:6px 10px;background:rgba(255,51,102,0.18);border:1px solid var(--red);border-radius:6px;color:#ff99aa;font-size:0.73rem;font-family:var(--font-mono);margin-bottom:8px">
                <span class="pulse" style="background:var(--red);box-shadow:0 0 8px var(--red)"></span>
                <span>🚨 EMPTY TANK (0/3) — DRY-RUN RELAY LOCKOUT ACTIVE</span>
              </div>
            `;
          }

          html += `
            ${tankAlert}
            <div class="tank-view">
              <div class="tank-vessel">
                <div class="water-fill" style="height: ${heights[wl] || 0}%;"></div>
              </div>
              <div class="details" style="flex:1">
                <div class="row"><span class="row-k">Water Level:</span><span class="row-v" style="color:var(--cyan)">${labels[wl] || (wl + '/3')}</span></div>
                <div class="row"><span class="row-k">Operational Mode:</span><span class="tag ${isAuto ? 'tag-online' : 'tag-alert'}">${isAuto ? 'AUTO' : 'MANUAL'}</span></div>
                <div class="row"><span class="row-k">Pond Demand:</span><span class="tag ${m.pond_demand ? 'tag-online' : 'tag-off'}">${m.pond_demand ? 'DEMAND ACTIVE' : 'IDLE'}</span></div>
                <div class="row"><span class="row-k">Auto Refill Rule:</span><span class="row-v" style="color:var(--green);font-size:0.7rem">ON &le; 1/3 &rarr; OFF &ge; 3/3</span></div>
                <div class="row"><span class="row-k">ESP32-S3 PSRAM:</span><span class="tag" style="background:rgba(176,86,255,0.15);color:var(--purple);border:1px solid rgba(176,86,255,0.4)">2048 KB ACTIVE</span></div>
              </div>
            </div>
            <div class="details">
              <div class="row" style="align-items: flex-start; margin-bottom: 6px;">
                <span class="row-k" style="padding-top: 3px;">Relay Actuation & Hardware Locks:</span>
                <div style="flex: 1; display: flex-direction: column; gap: 5px;">
                  ${[1, 2, 3].map(idx => {
                    const isOn = Boolean(m['p' + idx]);
                    const allowed = m['allowed_p' + idx];
                    const rLeft = (m.run_left && m.run_left[idx-1]) || 0;
                    const cLeft = (m.cd_left && m.cd_left[idx-1]) || 0;
                    
                    let lockTag = `<span class="tag tag-online" style="font-size:0.68rem;padding:2px 6px">⚡ READY</span>`;
                    if (rLeft > 0) {
                      lockTag = `<span class="tag tag-alert" style="font-size:0.68rem;padding:2px 6px;background:rgba(255,170,0,0.15);border-color:var(--amber);color:var(--amber)">⏳ RUN LOCK: ${rLeft}s</span>`;
                    } else if (cLeft > 0) {
                      lockTag = `<span class="tag tag-alert" style="font-size:0.68rem;padding:2px 6px;background:rgba(0,240,255,0.12);border-color:var(--cyan);color:var(--cyan)">❄️ COOLDOWN: ${cLeft}s</span>`;
                    }

                    return `
                      <div style="display:flex; justify-content:space-between; align-items:center; background:rgba(255,255,255,0.03); padding:4px 8px; border-radius:6px; border:1px solid rgba(255,255,255,0.05)">
                        <div style="display:flex; align-items:center; gap:8px">
                          <span style="font-weight:bold; font-family:var(--font-mono); color:#fff">P${idx}</span>
                          <span class="tag ${isOn ? 'tag-online' : 'tag-off'}" style="font-size:0.7rem; padding:1px 6px">${isOn ? 'ON' : 'OFF'}</span>
                          <span style="font-size:0.68rem; color:var(--muted)">${allowed !== undefined ? (allowed ? 'Auto:✓' : 'Auto:✗') : ''}</span>
                        </div>
                        ${lockTag}
                      </div>
                    `;
                  }).join('')}
                </div>
              </div>
              <div class="row"><span class="row-k">Environment:</span><span class="row-v">${m.temp_c !== undefined ? m.temp_c + ' °C / ' + m.humidity + ' %' : '--'}</span></div>
              <div class="row"><span class="row-k">Signal Quality:</span><span class="row-v">${n.last_rssi !== null ? n.last_rssi + ' dBm (SNR ' + n.last_snr + ' dB)' : 'No RF signal'}</span></div>
            </div>
          `;
        } else if (isPond) {
          const pr = pondSim.pump_running;
          const accord = (pondSim.tank_accord !== undefined) ? pondSim.tank_accord : true;
          const retryCnt = pondSim.accord_retry_count || 0;
          
          let accordBanner = '';
          if (accord) {
            accordBanner = `
              <div style="display:flex;align-items:center;gap:7px;padding:6px 10px;background:rgba(0,255,136,0.12);border:1px solid var(--green);border-radius:6px;color:var(--green);font-size:0.73rem;font-family:var(--font-mono);margin-bottom:8px">
                <span>🤝 COMMON ACCORD (Tank 0x02 & Pond in Mutual Agreement)</span>
              </div>
            `;
          } else {
            accordBanner = `
              <div style="display:flex;align-items:center;gap:7px;padding:6px 10px;background:rgba(255,170,0,0.15);border:1px solid var(--amber);border-radius:6px;color:var(--amber);font-size:0.73rem;font-family:var(--font-mono);margin-bottom:8px">
                <span class="pulse" style="background:var(--amber);box-shadow:0 0 8px var(--amber)"></span>
                <span>⚠️ AWAITING TANK ACCORD... (Handshake in progress [Retry #${retryCnt}])</span>
              </div>
            `;
          }

          let lowPondBanner = '';
          if (pondSim.pond_level_pct < 20.0) {
            lowPondBanner = `
              <div style="display:flex;align-items:center;gap:7px;padding:6px 10px;background:rgba(255,51,102,0.18);border:1px solid var(--red);border-radius:6px;color:#ff99aa;font-size:0.73rem;font-family:var(--font-mono);margin-bottom:8px">
                <span class="pulse" style="background:var(--red);box-shadow:0 0 8px var(--red)"></span>
                <span>🚨 LOW RESERVOIR SAFETY LOCK (&lt; 20%) — PUMP CUTOFF ACTIVE</span>
              </div>
            `;
          }

          html += `
            ${accordBanner}
            ${lowPondBanner}
            <div class="details">
              <div class="row"><span class="row-k">Inflow Pump State:</span><span class="tag ${pr ? 'tag-online' : 'tag-off'}">${pr ? 'RUNNING (PUMPING)' : 'IDLE (STOPPED)'}</span></div>
              <div class="row"><span class="row-k">Tank Accord:</span><span class="tag ${accord ? 'tag-online' : 'tag-alert'}">${accord ? 'IN SYNC' : 'AWAITING RESPONSE'}</span></div>
              <div class="row"><span class="row-k">Pumping Rate:</span><span class="row-v">${pondSim.flow_lpm || 60.0} L/min</span></div>
              <div class="row"><span class="row-k">Volume Delivered:</span><span class="row-v" style="color:var(--green);font-size:0.85rem">${pondSim.total_liters || 0.0} Liters</span></div>
              <div class="row"><span class="row-k">Pond Reservoir:</span><span class="row-v">${pondSim.pond_level_pct || 88}%</span></div>
              <div class="row"><span class="row-k">Solar Power Bank:</span><span class="row-v">${pondSim.battery_v || 12.8} V / ${pondSim.solar_w || 45} W</span></div>
              <div class="row"><span class="row-k">LoRa RF Beacons:</span><span class="tag ${pondSim.rf_tx_enabled ? 'tag-online' : 'tag-off'}">${pondSim.rf_tx_enabled ? 'TRANSMITTING DOWNLINKS' : 'OFF'}</span></div>
            </div>
            <div style="margin-top:8px;padding:8px;background:rgba(0,255,136,0.06);border-radius:6px;border:1px dashed var(--border-sim)">
              <div style="font-size:0.73rem;color:var(--green);margin-bottom:5px;font-family:var(--font-mono)">🧪 Pond Simulator Setup:</div>
              <div style="display:flex;flex-wrap:wrap;gap:4px;align-items:center;margin-bottom:4px">
                <span style="font-size:0.7rem;color:var(--muted)">Water:</span>
                <button class="btn-sim" style="padding:2px 6px;font-size:0.7rem" onclick="sendCmd('/pond sim level 0')">0%</button>
                <button class="btn-sim" style="padding:2px 6px;font-size:0.7rem" onclick="sendCmd('/pond sim level 33')">33%</button>
                <button class="btn-sim" style="padding:2px 6px;font-size:0.7rem" onclick="sendCmd('/pond sim level 66')">66%</button>
                <button class="btn-sim" style="padding:2px 6px;font-size:0.7rem" onclick="sendCmd('/pond sim level 100')">100%</button>
              </div>
              <div style="display:flex;flex-wrap:wrap;gap:4px;align-items:center">
                <span style="font-size:0.7rem;color:var(--muted)">Flow:</span>
                <button class="btn-sim" style="padding:2px 6px;font-size:0.7rem" onclick="sendCmd('/pond sim flow 30')">30 L/m</button>
                <button class="btn-sim" style="padding:2px 6px;font-size:0.7rem" onclick="sendCmd('/pond sim flow 60')">60 L/m</button>
                <button class="btn-sim" style="padding:2px 6px;font-size:0.7rem" onclick="sendCmd('/pond sim flow 100')">100 L/m</button>
              </div>
            </div>
            <div style="margin-top:10px;padding:9px;background:rgba(0,255,136,0.05);border-radius:8px;border:1px solid rgba(0,255,136,0.25)">
              <div style="font-size:0.74rem;font-weight:bold;color:var(--green);margin-bottom:7px;font-family:var(--font-mono);display:flex;justify-content:space-between;align-items:center">
                <span>📍 POND LOCAL STATION PANEL (0x03)</span>
                <span class="tag tag-sim" style="font-size:0.62rem">LOCAL OPERATOR</span>
              </div>
              <div style="display:flex;flex-wrap:wrap;gap:6px;margin-bottom:6px">
                <button class="btn-sim" style="flex:1;min-width:130px;padding:6px 8px;font-size:0.74rem;background:rgba(255,51,102,0.2);border-color:var(--red);color:#ff99aa" onclick="sendCmd('/pond local stop')">🛑 Stop Pump (Notify Tank)</button>
                <button class="btn-sim" style="flex:1;min-width:130px;padding:6px 8px;font-size:0.74rem;background:rgba(0,255,136,0.15);border-color:var(--green);color:var(--green)" onclick="sendCmd('/pond local start')">▶️ Start Pump (Notify Tank)</button>
              </div>
              <div style="display:flex;flex-wrap:wrap;gap:6px">
                <button class="btn-sim" style="flex:1;min-width:130px;padding:5px 6px;font-size:0.72rem" onclick="sendCmd('/pond sim tx')">📡 Send Status Beacon</button>
                <button class="btn-sim" style="flex:1;min-width:130px;padding:5px 6px;font-size:0.72rem" onclick="sendCmd('/pond sim toggle')">🔄 Toggle Sim Engine</button>
              </div>
            </div>
          `;
        } else if (isGateway) {
          html += `
            <div class="details">
              <div class="row"><span class="row-k">Gateway IP:</span><span class="row-v">${currentGwIp || d_gw_ip || 'Waiting...'}</span></div>
              <div class="row"><span class="row-k">Packets Rx / Tx:</span><span class="row-v">${m.rx_count || 0} / ${m.tx_count || 0}</span></div>
              <div class="row"><span class="row-k">TX Queue Depth:</span><span class="row-v">${m.q_depth || 0}/32</span></div>
              <div class="row"><span class="row-k">RF Radios:</span><span class="row-v">${m.rf0_mhz || '868.1'} MHz</span></div>
            </div>
          `;
        } else {
          // Generic Node Card
          html += `<div class="details">`;
          for (const [k, v] of Object.entries(m)) {
            html += `<div class="row"><span class="row-k">${k}:</span><span class="row-v">${v}</span></div>`;
          }
          if (Object.keys(m).length === 0) {
            html += `<div class="row" style="color:var(--muted)">No decoded telemetry yet</div>`;
          }
          html += `</div>`;
        }

        html += `</div>`; // Close main content

        // Actions toolbar
        html += `<div class="actions">`;
        if (isTank) {
          html += `
            <button onclick="sendCmd('/tank status')">Status</button>
            <button onclick="sendCmd('/tank mode toggle')">Mode</button>
            <button onclick="sendCmd('/tank p1 toggle')">P1</button>
            <button onclick="sendCmd('/tank p2 toggle')">P2</button>
            <button onclick="sendCmd('/tank p3 toggle')">P3</button>
            <button onclick="sendCmd('/tank pond toggle')">Pond Demand</button>
          `;
        } else if (isPond) {
          html += `
            <button class="btn-sim" onclick="sendCmd('/pond local stop')">🛑 Stop Pump</button>
            <button class="btn-sim" onclick="sendCmd('/pond local start')">▶️ Start Pump</button>
            <button class="btn-sim" onclick="sendCmd('/pond sim tx')">Beacon</button>
            <button class="btn-sim" onclick="sendCmd('/pond sim toggle')">Toggle Sim</button>
          `;
        } else {
          html += `<button onclick="sendCmd('/node ${n.id} status')">Inspect</button>`;
        }
        html += `</div></div>`; // Close card
      }

      container.innerHTML = html;
    }

    let d_gw_ip = '';
    setInterval(refreshData, 1200);
    refreshData();
  </script>
</body>
</html>
""".encode("utf-8")


# ─── HTTP Server & REST API Handler ───────────────────────────────────────────

class StationWebHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress console log noise

    def do_GET(self):
        if self.path == "/api/data":
            alarms = []
            tank = node_registry.nodes.get(NODE_TANK)
            if tank and "metrics" in tank:
                tm = tank["metrics"]
                if tm.get("auto_mode") and tm.get("water_level") == 0:
                    alarms.append({
                        "id": "tank_empty",
                        "level": "CRITICAL",
                        "title": "🚨 TANK EMPTY AUTO-LOCKOUT",
                        "msg": "Tank level is 0/3 (Empty) in AUTO mode. Outflow relays interlocked to prevent dry run."
                    })
                if tank.get("last_seen", 0) > 0 and (time.time() - tank["last_seen"]) > 60:
                    alarms.append({
                        "id": "lora_watchdog",
                        "level": "WARNING",
                        "title": "⚠️ LORA WATCHDOG SILENCE",
                        "msg": f"Tank Node (0x02) silence detected (> {int(time.time() - tank['last_seen'])}s without heartbeat)."
                    })
            if pond_sim.pond_level_pct < 20.0:
                alarms.append({
                    "id": "low_pond",
                    "level": "WARNING",
                    "title": "⚠️ POND RESERVOIR CRITICALLY LOW",
                    "msg": f"Pond reservoir is {pond_sim.pond_level_pct:.1f}% (< 20%). Safety cutoff prohibits inflow pump operation."
                })

            data = {
                "gateway_ip": gateway_ip or "Waiting...",
                "pkt_counter": pkt_counter,
                "nodes": node_registry.get_all(),
                "pond_sim": pond_sim.get_metrics(),
                "packets": packet_history[-35:],
                "history": telemetry_history[-60:],
                "alarms": alarms,
                "psram_info": {
                    "enabled": True,
                    "size_kb": 2048,
                    "chip": "ESP32-S3 WROOM-1 (AP_3v3)",
                    "mode": "QIO Q-SPI",
                    "buffer_capacity": 1440
                }
            }
            body = json.dumps(data).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/api/history":
            body = json.dumps(telemetry_history).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/api/nodes":
            nodes_data = node_registry.get_all()
            body = json.dumps(nodes_data).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(FLEET_HTML)))
        self.end_headers()
        self.wfile.write(FLEET_HTML)

    def do_POST(self):
        if self.path == "/api/cmd":
            try:
                content_len = int(self.headers.get("Content-Length", 0))
                post_body = self.rfile.read(content_len)
                data = json.loads(post_body.decode("utf-8"))
                cmd = data.get("cmd", "").strip()
                if cmd:
                    handle(cmd)
                    resp = json.dumps({"status": "ok", "msg": f"Dispatched: {cmd}"}).encode("utf-8")
                else:
                    resp = json.dumps({"status": "err", "msg": "Empty command"}).encode("utf-8")
            except Exception as e:
                resp = json.dumps({"status": "err", "msg": str(e)}).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
            return

def start_station_web_server(port=STATION_WEB_PORT):
    global active_web_port
    for p in range(port, port + 10):
        try:
            srv = HTTPServer(("0.0.0.0", p), StationWebHandler)
            t = threading.Thread(target=srv.serve_forever, daemon=True)
            t.start()
            active_web_port = p
            return p
        except OSError:
            continue
    return None


# ─── Background Simulation Ticker ─────────────────────────────────────────────

def simulation_ticker():
    """Runs periodic physics integration and RF beacons for simulated nodes, and ticks relay interlock countdowns."""
    last_hist_sample = 0.0
    while True:
        try:
            now = time.time()
            pond_sim.step()
            # Real-time 1-second ticker for Tank Node (0x02) relay interlock locks
            with node_registry.lock:
                tank = node_registry.nodes.get(NODE_TANK)
                if tank and "metrics" in tank:
                    m = tank["metrics"]
                    rl = m.get("run_left")
                    if rl:
                        for i in range(len(rl)):
                            if rl[i] > 0:
                                rl[i] -= 1
                    cl = m.get("cd_left")
                    if cl:
                        for i in range(len(cl)):
                            if cl[i] > 0:
                                cl[i] -= 1

            # In-memory PSRAM-style Telemetry Buffer (10-second snapshots, max 360 entries = 1 hour)
            if now - last_hist_sample >= 10.0:
                last_hist_sample = now
                with node_registry.lock:
                    tank = node_registry.nodes.get(NODE_TANK)
                    wl = 0
                    auto_m = False
                    p1_v = 0
                    p2_v = 0
                    p3_v = 0
                    temp = 0.0
                    hum = 0
                    if tank and "metrics" in tank:
                        tm = tank["metrics"]
                        wl = tm.get("water_level", 0)
                        auto_m = bool(tm.get("auto_mode", False))
                        p1_v = 1 if tm.get("p1") else 0
                        p2_v = 1 if tm.get("p2") else 0
                        p3_v = 1 if tm.get("p3") else 0
                        temp = float(tm.get("temp_c", 0.0))
                        hum = int(tm.get("humidity", 0))

                pm = pond_sim.get_metrics()
                tank_pct_map = [0, 33, 66, 100]
                telemetry_history.append({
                    "time": time.strftime("%H:%M:%S"),
                    "ts": int(now),
                    "tank_wl": wl,
                    "tank_pct": tank_pct_map[min(3, max(0, wl))],
                    "tank_auto": auto_m,
                    "p1": p1_v,
                    "p2": p2_v,
                    "p3": p3_v,
                    "pond_pump": 1 if pm["pump_running"] else 0,
                    "pond_pct": round(pm["pond_level_pct"], 1),
                    "pond_liters": round(pm["total_liters"], 1),
                    "temp_c": round(temp, 1),
                    "hum": hum
                })
                if len(telemetry_history) > 360:
                    telemetry_history.pop(0)

        except Exception as e:
            pass
        time.sleep(1.0)


# ─── Listener Thread ──────────────────────────────────────────────────────────

def listener_thread():
    global gateway_ip, pkt_counter
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except OSError:
            break

        gateway_ip   = addr[0]
        pkt_counter += 1
        ts           = time.strftime("%H:%M:%S")
        pkt_type     = data[0] if data else 0xFF
        
        log_lines = []

        if data and data[0:1] == b'{':
            log_lines.append(f"[{ts}] #{pkt_counter} RXPK  {gateway_ip}")
            log_lines.append(f"  Raw: {data.decode('utf-8', errors='replace')}")
            log_lines.append(parse_rxpk_json(data.decode("utf-8", errors="replace")))

        elif pkt_type == PKT_TYPE_HEARTBEAT:
            log_lines.append(f"[{ts}] #{pkt_counter} HB  {gateway_ip}")
            log_lines.append(parse_heartbeat(data))

        elif pkt_type == PKT_TYPE_BOOT:
            log_lines.append(f"[{ts}] #{pkt_counter} BOOT  {gateway_ip}")
            log_lines.append(parse_boot(data))
            log_lines.append(f"  Gateway locked at {gateway_ip} — ready for bidirectional multi-node RF.")

        else:
            log_lines.append(f"[{ts}] #{pkt_counter} UNKNOWN type=0x{pkt_type:02X} len={len(data)}  {data[:32].hex()}")

        safe_print("\n".join(log_lines))


# ─── Universal Command Handler ────────────────────────────────────────────────

def print_help():
    print("""
  Universal Multi-Node Station & Simulator Commands:
    /nodes                    List all registered & discovered nodes in the network
    /node <id> status         Inspect full metrics for any node ID (e.g. /node 2 status)
    /node <id> cmd <t> <a> <v>Send arbitrary LoRa command byteframe to node
    /send <tgt> <src> <typ> <hex> Transmit custom LoRa packet between any two nodes

  Pond Node Simulator (0x03):
    /pond status              Show simulated Pond pump state, volume, level, power
    /pond sim start|stop      Start / stop the simulated Pond pump manually
    /pond sim tx              Broadcast immediate LoRa RF MSG_STATUS beacon from 0x03
    /pond sim flow <L/min>    Configure simulated pumping rate (default: 60.0)
    /pond sim level <0-100>   Set simulated pond reservoir water level percentage
    /pond sim rf <on|off>     Enable/disable genuine LoRa RF downlinks from simulator

  Pond ➔ Tank Commands (Dispatched from Pond 0x03 to Tank 0x02):
    /pond to tank refill on|off|toggle  Command Tank Node refill demand switch from Pond
    /pond to tank mode auto|man|toggle  Command Tank Node operational mode from Pond
    /pond to tank stop                  Emergency Outflow Cutoff: turn OFF P1, P2, P3 from Pond
    /pond to tank p1|p2|p3 on|off|tog   Command specific outflow pump relay from Pond
    /pond to tank beacon                Dispatch direct status beacon to Tank Node

  Tank Node Commands (0x02):
    /tank status              Show latest cached telemetry with visual ASCII gauge
    /tank mode auto|man       Command Tank Node operational mode
    /tank p1 on|off|toggle    Command Outflow Pump 1
    /tank p2 on|off|toggle    Command Outflow Pump 2
    /tank p3 on|off|toggle    Command Outflow Pump 3
    /tank pond on|off|toggle  Command Remote Pond Pump demand virtual switch

  Station & Gateway:
    /gui or /web              Open Fleet Web Dashboard in your browser (http://localhost:8080)
    /status                   Show gateway heartbeat stats
    /custom [opts] [text]     Send custom RF JSON (freq, sf, bw, power, hex=...)
    /hex <hexstring>          Send raw bytes wrapped in JSON
    /help                     Show this help reference
    Ctrl+C                    Exit
""")

def handle(line: str):
    line = line.strip()
    if not line:
        return

    if line == "/help":
        print_help()
        return

    if line in ("/gui", "/web"):
        url = f"http://localhost:{active_web_port}"
        safe_print(f"  🌐 Launching Fleet Web Dashboard: {url}")
        try:
            webbrowser.open(url)
        except Exception as e:
            safe_print(f"  [!] Open browser manually: {url}")
        return

    # ── /nodes Command ────────────────────────────────────────────────────────
    if line == "/nodes":
        nodes = node_registry.get_all()
        safe_print(f"\n  ┌─ Registered LoRa Fleet Nodes ({len(nodes)} total) ───────────────────────")
        safe_print(f"  │ ID    Name                     Role                         Status    ")
        safe_print(f"  ├────── ──────────────────────── ──────────────────────────── ──────────")
        for nid, n in sorted(nodes.items()):
            sim_flag = " [SIM]" if n["is_simulated"] else ""
            status_str = f"{n['status']}{sim_flag}"
            safe_print(f"  │ {n['id_hex']:<5} {n['name'][:24]:<24} {n['role'][:28]:<28} {status_str:<10}")
        safe_print(f"  └────────────────────────────────────────────────────────────────────────\n")
        return

    # ── /node <id> Commands ───────────────────────────────────────────────────
    if line.startswith("/node "):
        parts = line[6:].strip().split()
        if not parts:
            safe_print("  Usage: /node <id_hex_or_dec> [status|cmd <target> <act> <val>]")
            return
        try:
            nid = int(parts[0], 0)
        except ValueError:
            safe_print("  [!] Invalid node ID. Use decimal (e.g. 2) or hex (e.g. 0x02)")
            return

        sub = parts[1].lower() if len(parts) > 1 else "status"
        if sub == "status":
            n = node_registry.get(nid)
            ago = int(time.time() - n["last_seen"]) if n["last_seen"] > 0 else -1
            seen_str = f"{ago}s ago" if ago >= 0 else "Never"
            safe_print(f"  ┌─ Node {n['id_hex']} ({n['name']}) ─────────────────────────")
            safe_print(f"  │ Type       : {n['type']} | Role: {n['role']}")
            safe_print(f"  │ Status     : {n['status']} (Seen: {seen_str}) | Packets: {n['pkt_count']}")
            if n["last_rssi"] is not None:
                safe_print(f"  │ Signal     : RSSI {n['last_rssi']} dBm | SNR {n['last_snr']} dB")
            safe_print(f"  │ Metrics    :")
            for k, v in n["metrics"].items():
                safe_print(f"  │   • {k:<15} : {v}")
            safe_print(f"  └────────────────────────────────────────────────────────────")
            return

        if sub == "cmd" and len(parts) >= 5:
            try:
                t_sub = int(parts[2], 0)
                act   = int(parts[3], 0)
                val   = int(parts[4], 0)
                send_node_cmd(NODE_GATEWAY, nid, t_sub, act, val)
                return
            except ValueError:
                safe_print("  [!] Invalid cmd arguments. Usage: /node <id> cmd <target_byte> <action_byte> <val_byte>")
                return

    # ── /pond Commands ────────────────────────────────────────────────────────
    if line.startswith("/pond"):
        m = pond_sim.get_metrics()
        parts = line.split()
        if len(parts) == 1 or parts[1] == "status":
            accord_str = "🤝 IN COMMON ACCORD (Tank 0x02 in sync)" if m.get("tank_accord") else f"⚠️ AWAITING ACCORD (Handshake Retrying [#{m.get('accord_retry_count',0)}])"
            safe_print(f"  ┌─ Pond Node (0x03) Autonomous Simulator Status ─────────")
            safe_print(f"  │ Simulator Active : {'YES' if m['enabled'] else 'DISABLED'}")
            safe_print(f"  │ Pump State       : {'RUNNING (PUMPING)' if m['pump_running'] else 'IDLE (STOPPED)'}")
            safe_print(f"  │ Tank Accord      : {accord_str}")
            safe_print(f"  │ Pumping Rate     : {m['flow_lpm']} L/min")
            safe_print(f"  │ Volume Delivered : {m['total_liters']} Liters (Current run: {m['cur_dur_s']}s, Total: {m['total_runtime_s']}s)")
            safe_print(f"  │ Pond Water Level : {m['pond_level_pct']}% Full")
            safe_print(f"  │ Power Bank       : {m['battery_v']} V ({m['solar_w']} W solar)")
            safe_print(f"  │ LoRa RF Beacons  : {'TRANSMITTING DOWNLINKS' if m['rf_tx_enabled'] else 'RF TX OFF'} (Interval: {m['tx_interval_s']}s)")
            safe_print(f"  └─────────────────────────────────────────────────────────")
            return

        if len(parts) >= 3 and parts[1] == "local":
            sub_cmd = parts[2].lower()
            if sub_cmd in ("start", "on", "1"):
                pond_sim.set_pump(True, "Local Station Start Button", notify_tank_first_hand=True)
            elif sub_cmd in ("stop", "off", "0"):
                pond_sim.set_pump(False, "Local Station Stop Button", notify_tank_first_hand=True)
            return

        if len(parts) >= 3 and parts[1] == "sim":
            sub_cmd = parts[2].lower()
            if sub_cmd in ("start", "on", "1"):
                pond_sim.set_pump(True, "CLI /pond sim start", notify_tank_first_hand=True)
            elif sub_cmd in ("stop", "off", "0"):
                pond_sim.set_pump(False, "CLI /pond sim stop", notify_tank_first_hand=True)
            elif sub_cmd == "tx":
                pond_sim.send_status_beacon(target_id=NODE_BROADCAST, reason="Manual CLI /pond sim tx")
            elif sub_cmd == "toggle":
                pond_sim.enabled = not pond_sim.enabled
                safe_print(f"  [Pond Sim] Simulator {'ENABLED' if pond_sim.enabled else 'DISABLED'}")
            elif sub_cmd == "rf" and len(parts) >= 4:
                pond_sim.rf_tx_enabled = parts[3].lower() in ("1", "on", "true")
                safe_print(f"  [Pond Sim] LoRa RF beacon downlinks {'ENABLED' if pond_sim.rf_tx_enabled else 'DISABLED'}")
            elif sub_cmd == "flow" and len(parts) >= 4:
                try:
                    pond_sim.flow_lpm = float(parts[3])
                    safe_print(f"  [Pond Sim] Pump flow rate set to {pond_sim.flow_lpm} L/min")
                except ValueError:
                    safe_print("  [!] Invalid flow rate")
            elif sub_cmd == "level" and len(parts) >= 4:
                try:
                    pond_sim.pond_level_pct = min(100.0, max(0.0, float(parts[3])))
                    safe_print(f"  [Pond Sim] Pond water level set to {pond_sim.pond_level_pct}%")
                except ValueError:
                    safe_print("  [!] Invalid level percent (0-100)")
            return

        # ── Cross-Node Synchronizations: Pond (0x03) ➔ Tank Node (0x02) ───────────
        if len(parts) >= 4 and parts[1] == "to" and parts[2] == "tank":
            action_word = parts[3].lower()
            if action_word == "sync" or action_word == "beacon":
                pond_sim.send_status_beacon(target_id=NODE_TANK, reason="Direct Pond->Tank Sync Beacon")
                return
            elif action_word in ("stop", "off"):
                pond_sim.set_pump(False, "Local Pond Stop Notification to Tank", notify_tank_first_hand=True)
                return
            elif action_word in ("start", "on"):
                pond_sim.set_pump(True, "Local Pond Start Notification to Tank", notify_tank_first_hand=True)
                return
            safe_print("  Usage: /pond to tank <sync|beacon|stop|start>")
            return

        if len(parts) >= 5 and parts[1] == "cmd":
            try:
                t_sub = int(parts[2], 0)
                act   = int(parts[3], 0)
                val   = int(parts[4], 0)
                send_node_cmd(NODE_POND, NODE_TANK, t_sub, act, val)
                return
            except ValueError:
                safe_print("  Usage: /pond cmd <target_byte> <action_byte> <val_byte>")
                return

    # ── /tank Commands ────────────────────────────────────────────────────────
    if line == "/tank status":
        tank = node_registry.get(NODE_TANK)
        m = tank.get("metrics", {})
        if not m:
            safe_print("  [!] No telemetry received from Tank Node (0x02) yet.")
            return
        ago = int(time.time() - tank.get("last_seen", time.time()))
        wl = m.get("water_level", 0)
        mode = "AUTO" if m.get("auto_mode") else "MANUAL"
        p1 = "ON" if m.get("p1") else "OFF"
        p2 = "ON" if m.get("p2") else "OFF"
        p3 = "ON" if m.get("p3") else "OFF"
        pond_demand = "DEMAND ACTIVE" if m.get("pond_demand") else "IDLE"
        temp = m.get("temp_c", 0.0)
        hum = m.get("humidity", 0)
        
        bars = [
            "[░░░░░░░░░░] EMPTY (0%)",
            "[███░░░░░░░] LOW   (33%)",
            "[███████░░░] MID   (66%)",
            "[██████████] FULL  (100%)"
        ]
        gauge = bars[min(wl, 3)]

        safe_print(f"  ┌─ Tank Node (0x02) Telemetry (seen {ago}s ago) ─────────")
        safe_print(f"  │ Water Level : {gauge}")
        safe_print(f"  │ System Mode : {mode}")
        safe_print(f"  │ Outflow P1  : {p1}")
        safe_print(f"  │ Outflow P2  : {p2}")
        safe_print(f"  │ Outflow P3  : {p3}")
        safe_print(f"  │ Pond Demand : {pond_demand}")
        safe_print(f"  │ Environment : {temp:.1f} °C, {hum}% RH")
        safe_print(f"  └─────────────────────────────────────────────────────────")
        return

    if line.startswith("/tank "):
        parts = line[6:].strip().split()
        if len(parts) >= 2:
            sub = parts[0].lower()
            val = parts[1].lower()
            if sub == "mode":
                act = 0x00 if val == "toggle" else 0x01
                v = 0x01 if val in ("auto", "1", "on") else 0x00
                send_node_cmd(NODE_GATEWAY, NODE_TANK, CMD_TARGET_MODE, act, v)
                return
            elif sub in ("p1", "p2", "p3"):
                p_mask = {"p1": CMD_TARGET_P1, "p2": CMD_TARGET_P2, "p3": CMD_TARGET_P3}[sub]
                act = 0x00 if val == "toggle" else 0x01
                v = 0x01 if val in ("on", "1") else 0x00
                send_node_cmd(NODE_GATEWAY, NODE_TANK, p_mask, act, v)
                return
            elif sub == "pond":
                act = 0x00 if val == "toggle" else 0x01
                v = 0x01 if val in ("on", "1") else 0x00
                send_node_cmd(NODE_GATEWAY, NODE_TANK, CMD_TARGET_POND, act, v)
                return
        safe_print("  Usage: /tank <mode|p1|p2|p3|pond> <on|off|toggle|auto|man>")
        return

    # ── Universal /send Command ───────────────────────────────────────────────
    if line.startswith("/send "):
        # /send <target_id> <sender_id> <msg_type> <hex_payload>
        parts = line[6:].strip().split()
        if len(parts) >= 4:
            try:
                tgt = int(parts[0], 0)
                src = int(parts[1], 0)
                mtype = int(parts[2], 0)
                raw_payload = bytes.fromhex(parts[3].replace(" ", ""))
                pkt = bytearray([0x6B, 0x5A, tgt, src, mtype]) + raw_payload
                tx_json = build_custom(bytes(pkt), freq=868_100_000, power=14, sf=9, bw=125, cr="4/5", invert=True)
                if send(tx_json):
                    safe_print(f"  [TX Frame] Sent {len(pkt)} bytes (0x{src:02X} → 0x{tgt:02X}, type=0x{mtype:02X})")
                    record_packet_history(f"TYPE_0x{mtype:02X}", f"0x{src:02X}", f"0x{tgt:02X}", "Custom injected packet", pkt.hex().upper())
                return
            except Exception as e:
                safe_print(f"  [!] Failed to build frame: {e}")
                return
        safe_print("  Usage: /send <target_id> <sender_id> <msg_type> <hex_payload>")
        return

    # ── Gateway Raw Commands ──────────────────────────────────────────────────
    if line == "/status":
        if not last_hb:
            safe_print("  No gateway heartbeat received yet.")
            return
        h, m, s = last_hb['uptime']//3600, (last_hb['uptime']%3600)//60, last_hb['uptime']%60
        sw = last_hb['sync_word']
        sync_str = "public(0x34)" if sw == 0x34 else f"private(0x{sw:02X})"
        safe_print(f"  uptime={h:02d}:{m:02d}:{s:02d}  rx={last_hb['rx']}  tx={last_hb['tx']}  "
                   f"crc_err={last_hb['crc_err']}  tx_err={last_hb['tx_err']}  "
                   f"q={last_hb['q_depth']}/{TX_QUEUE_SIZE}  q_drop={last_hb['q_drop']}  "
                   f"sync={sync_str}  rf0={last_hb['rf0_freq']/1e6:.3f}MHz  rf1={last_hb['rf1_freq']/1e6:.3f}MHz")
        return

    if line.startswith("/hex "):
        hexstr = line[5:].replace(" ", "")
        try:
            payload = bytes.fromhex(hexstr)
        except ValueError as e:
            safe_print(f"  [!] Bad hex: {e}")
            return
        pkt = build_default(payload)
        if send(pkt):
            safe_print(f"  [TX] raw hex JSON  {len(payload)} bytes → {hexstr}")
        return

    if line.startswith("/custom "):
        opts = dict(freq=868_100_000, sf=7, bw=125, power=14, cr="4/5", invert=1, rfch=0, modu="LORA")
        words = line[8:].split()
        text_words = []
        hex_payload = None
        for w in words:
            if "=" in w:
                k, v = w.split("=", 1)
                if k == "freq":
                    f_val = float(v)
                    opts["freq"] = int(f_val * 1e6) if f_val < 3000 else int(f_val)
                elif k == "sf":    opts["sf"]    = int(v)
                elif k == "bw":    opts["bw"]    = int(v)
                elif k == "power": opts["power"] = int(v)
                elif k == "cr":    opts["cr"]    = v
                elif k == "invert":opts["invert"]= int(v)
                elif k == "rfch":  opts["rfch"]  = int(v)
                elif k == "modu":  opts["modu"]  = v.upper()
                elif k == "hex":
                    try:
                        hex_payload = bytes.fromhex(v.replace(" ", ""))
                    except ValueError:
                        safe_print("  [!] Invalid hex string.")
                        return
            else:
                text_words.append(w)
        payload = hex_payload if hex_payload is not None else " ".join(text_words).encode()
        if not payload:
            safe_print("  [!] No payload text or hex provided.")
            return
        pkt = build_custom(payload, freq=opts["freq"], power=opts["power"],
                           sf=opts["sf"], bw=opts["bw"], cr=opts["cr"],
                           invert=bool(opts["invert"]), rfch=opts["rfch"], modu=opts["modu"])
        if send(pkt):
            safe_print(f"  [TX] custom JSON | SF{opts['sf']} | {opts['freq']/1e6:.4f} MHz | {len(payload)} bytes")
        return

    # Fallback to plain text TX
    if line.startswith("/tx "):
        line = line[4:]
    payload = line.encode()
    if len(payload) > 255:
        safe_print(f"  [!] Payload too long ({len(payload)} bytes, max 255)")
        return
    if send(build_default(payload)):
        safe_print(f"  [TX] default JSON  {len(payload)} bytes → \"{line}\"")


# ─── Main Startup ─────────────────────────────────────────────────────────────

def main():
    sock.bind(("0.0.0.0", LISTEN_PORT_UP))

    # Start LoRa listener thread
    t_listen = threading.Thread(target=listener_thread, daemon=True)
    t_listen.start()

    # Start simulation engine background ticker
    t_sim = threading.Thread(target=simulation_ticker, daemon=True)
    t_sim.start()

    # Start Web Server
    p = start_station_web_server(STATION_WEB_PORT)

    safe_print("=" * 68)
    safe_print("    SX1302 UNIVERSAL LORA FLEET COORDINATOR & POND SIMULATOR")
    safe_print("=" * 68)
    safe_print(f"  LoRa Socket  : listening on :{LISTEN_PORT_UP}  →  TX on :{SEND_PORT_DOWN}")
    if p:
        safe_print(f"  Fleet Web GUI: http://localhost:{p}  (type /gui to launch browser)")
    else:
        safe_print(f"  Fleet Web GUI: [!] Failed to bind ports {STATION_WEB_PORT}-{STATION_WEB_PORT+9}")
    safe_print(f"  Simulator    : Active Pond Node (0x03) [Rate: 60 L/min, LoRa RF TX: ON]")
    safe_print(f"  Console Help : type /help or /nodes for command reference")
    safe_print("=" * 68)
    safe_print("Waiting for gateway packets...\n")

    # Interactive prompt loop with fallback
    if HAS_PROMPT_TOOLKIT:
        try:
            session = PromptSession()
            with patch_stdout():
                while True:
                    try:
                        user_line = session.prompt("> ")
                        handle(user_line)
                    except KeyboardInterrupt:
                        safe_print("\nExiting.")
                        break
                    except EOFError:
                        break
            return
        except Exception:
            pass

    # Standard fallback console loop
    while True:
        try:
            user_line = input("> ")
            handle(user_line)
        except KeyboardInterrupt:
            safe_print("\nExiting.")
            break
        except EOFError:
            break

if __name__ == "__main__":
    main()