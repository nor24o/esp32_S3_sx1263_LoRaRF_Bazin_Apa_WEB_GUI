import socket
import struct
import threading
import time
import json
import base64
import sys
import os
import webbrowser
import sqlite3
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
DEFAULT_GATEWAY_IP = "192.168.1.29"
sock            = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
gateway_ip      = DEFAULT_GATEWAY_IP
pkt_counter     = 0
last_hb         = {}
active_web_port = STATION_WEB_PORT
packet_history  = []
telemetry_history = []


# ─── Universal Fleet SQLite Database ──────────────────────────────────────────
DB_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gateway_fleet.db")

class FleetDatabase:
    """
    Persistent SQLite database manager for Universal LoRa Fleet.
    Manages devices, dynamic RF modulation, inter-node hydraulic correlations, and packet history.
    Uses SQLite WAL mode with RLock for concurrent thread-safety across background workers.
    """
    def __init__(self, db_path=DB_FILE):
        self.db_path = db_path
        self.lock = threading.RLock()
        self.conn = sqlite3.connect(self.db_path, timeout=10.0, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        with self.lock:
            self.conn.execute("PRAGMA journal_mode=WAL;")
            self.conn.execute("PRAGMA synchronous=NORMAL;")
        self._init_db()

    def close(self):
        with self.lock:
            if self.conn:
                self.conn.close()
                self.conn = None

    def _init_db(self):
        with self.lock:
            cur = self.conn.cursor()
            cur.executescript("""
            CREATE TABLE IF NOT EXISTS devices (
                node_id INTEGER PRIMARY KEY,
                name TEXT NOT NULL,
                type TEXT NOT NULL,
                role TEXT,
                hardware TEXT DEFAULT 'ESP32-S3',
                first_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                pkt_count INTEGER DEFAULT 0,
                last_sf INTEGER DEFAULT 9,
                last_bw_khz INTEGER DEFAULT 125,
                last_cr TEXT DEFAULT '4/5',
                last_freq_hz INTEGER DEFAULT 868100000,
                last_rssi REAL,
                last_snr REAL,
                last_ldro INTEGER DEFAULT 0,
                status TEXT DEFAULT 'OFFLINE',
                is_simulated INTEGER DEFAULT 0,
                params_json TEXT DEFAULT '{}',
                metrics_json TEXT DEFAULT '{}'
            );

            CREATE TABLE IF NOT EXISTS node_correlations (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_node_id INTEGER NOT NULL,
                target_node_id INTEGER NOT NULL,
                relation_type TEXT NOT NULL,
                rule_desc TEXT NOT NULL,
                condition_expr TEXT,
                action_expr TEXT,
                active_accord INTEGER DEFAULT 1,
                trigger_count INTEGER DEFAULT 0,
                last_triggered TIMESTAMP,
                UNIQUE(source_node_id, target_node_id, relation_type)
            );

            CREATE TABLE IF NOT EXISTS telemetry_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                node_id INTEGER NOT NULL,
                water_level INTEGER,
                water_level_pct REAL,
                auto_mode INTEGER,
                p1 INTEGER,
                p2 INTEGER,
                p3 INTEGER,
                pond_pump INTEGER,
                temperature REAL,
                humidity REAL,
                battery_v REAL,
                solar_w REAL,
                sf INTEGER,
                bw_khz INTEGER,
                rssi REAL,
                snr REAL,
                raw_hex TEXT
            );

            CREATE TABLE IF NOT EXISTS packet_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                direction TEXT NOT NULL,
                sender_id INTEGER NOT NULL,
                target_id INTEGER NOT NULL,
                msg_type INTEGER NOT NULL,
                msg_type_str TEXT,
                summary TEXT,
                sf INTEGER,
                bw_khz INTEGER,
                freq_mhz REAL,
                payload_hex TEXT,
                rssi REAL,
                snr REAL,
                stat_str TEXT
            );
            """)

            # Seed default known network devices (Default SF11 / BW125 / LDRO=1 for 700m+ Long-Range)
            cur.execute("""
            INSERT OR IGNORE INTO devices (node_id, name, type, role, hardware, is_simulated, last_sf, last_bw_khz, last_ldro, status)
            VALUES (1, 'Gateway Coordinator', 'GATEWAY', 'SX1302 Concentrator & UDP Hub', 'SX1302 CoreCell', 0, 11, 125, 1, 'ONLINE');
            """)
            cur.execute("""
            INSERT OR IGNORE INTO devices (node_id, name, type, role, hardware, is_simulated, last_sf, last_bw_khz, last_ldro, status)
            VALUES (2, 'Tank Node (Bazin)', 'RESERVOIR_CTRL', 'Level Floats & 3x Outflow Relays', 'ESP32-S3 + SX1262', 0, 11, 125, 1, 'OFFLINE');
            """)
            cur.execute("""
            INSERT OR IGNORE INTO devices (node_id, name, type, role, hardware, is_simulated, last_sf, last_bw_khz, last_ldro, status)
            VALUES (3, 'Pond Node (Iaz)', 'INFLOW_PUMP', 'Remote Pond Charging Pump & Source Monitor', 'Virtual / RF Node', 1, 11, 125, 1, 'SIMULATED');
            """)

            # Seed default inter-node hydraulic correlations
            cur.execute("""
            INSERT OR IGNORE INTO node_correlations (source_node_id, target_node_id, relation_type, rule_desc, condition_expr, action_expr)
            VALUES (2, 3, 'HYDRAULIC_REFILL_DEMAND', 'Tank low level triggers remote pond pump refill', 'Tank.water_level <= 1 (<=33%)', 'Pond.pump = ON until Level >= 3 (100%)');
            """)
            cur.execute("""
            INSERT OR IGNORE INTO node_correlations (source_node_id, target_node_id, relation_type, rule_desc, condition_expr, action_expr)
            VALUES (3, 2, 'ACCORD_CONSENSUS_ACK', 'Pond pump status acknowledgment & failsafe status beacon', 'Pond pump state change', 'Tank aligns demand flag & confirms accord');
            """)
            cur.execute("""
            INSERT OR IGNORE INTO node_correlations (source_node_id, target_node_id, relation_type, rule_desc, condition_expr, action_expr)
            VALUES (2, 2, 'DRY_RUN_INTERLOCK', 'Tank empty emergency lockout: shuts off all outflow relays P1, P2, P3', 'Tank.water_level == 0 in AUTO', 'Force P1=0, P2=0, P3=0');
            """)
            cur.execute("""
            INSERT OR IGNORE INTO node_correlations (source_node_id, target_node_id, relation_type, rule_desc, condition_expr, action_expr)
            VALUES (1, 2, 'GATEWAY_KEEPALIVE', 'Gateway periodic 30s downlink heartbeat to Tank Node', 'Interval >= 30s', 'Send MSG_STATUS downlink to prevent node timeout');
            """)
            cur.execute("""
            INSERT OR IGNORE INTO node_correlations (source_node_id, target_node_id, relation_type, rule_desc, condition_expr, action_expr)
            VALUES (3, 3, 'POND_RESERVOIR_SAFETY', 'Pond reservoir low volume protection cutoff (< 20%)', 'Pond.pond_level_pct < 20.0%', 'Emergency pump cutoff & prohibit refill');
            """)
            self.conn.commit()

    def upsert_device(self, node_id, name, n_type, role, hardware='ESP32-S3', is_simulated=0):
        with self.lock:
            self.conn.execute("""
            INSERT INTO devices (node_id, name, type, role, hardware, is_simulated, last_seen)
            VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
            ON CONFLICT(node_id) DO UPDATE SET
                name=excluded.name,
                type=excluded.type,
                role=excluded.role,
                hardware=excluded.hardware,
                is_simulated=excluded.is_simulated,
                last_seen=CURRENT_TIMESTAMP
            """, (node_id, name, n_type, role, hardware, 1 if is_simulated else 0))
            self.conn.commit()

    def touch_device(self, node_id, sf=None, bw_khz=None, freq_hz=None, rssi=None, snr=None, cr=None, ldro=None, metrics=None):
        if node_id in (0x00, 0xFF) or node_id < 1 or node_id > 16:
            return
        with self.lock:
            row = self.conn.execute("SELECT node_id, last_sf, last_bw_khz FROM devices WHERE node_id = ?", (node_id,)).fetchone()
            if not row:
                name = f"Node 0x{node_id:02X}"
                n_type = "GENERIC_NODE"
                role = "Auto-discovered LoRa Field Node"
                self.conn.execute("""
                INSERT INTO devices (node_id, name, type, role, first_seen, last_seen, pkt_count, status)
                VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 1, 'ONLINE')
                """, (node_id, name, n_type, role))
                cur_sf = sf if sf is not None else 11
                cur_bw = bw_khz if bw_khz is not None else 125
            else:
                cur_sf = sf if sf is not None else (row["last_sf"] or 11)
                cur_bw = bw_khz if bw_khz is not None else (row["last_bw_khz"] or 125)
            
            updates = ["last_seen = CURRENT_TIMESTAMP", "pkt_count = pkt_count + 1", "status = 'ONLINE'"]
            params = []
            if sf is not None:
                updates.append("last_sf = ?")
                params.append(sf)
            if bw_khz is not None:
                updates.append("last_bw_khz = ?")
                params.append(bw_khz)
            if cr is not None and cr != "":
                updates.append("last_cr = ?")
                params.append(cr)
            if ldro is not None:
                updates.append("last_ldro = ?")
                params.append(1 if ldro else 0)
            elif sf is not None or bw_khz is not None:
                # Automatic LDRO Engine: T_sym = 2^SF / BW_kHz >= 16.0 ms
                t_sym_ms = (2 ** cur_sf) / float(max(1, cur_bw))
                ldro_flag = 1 if t_sym_ms >= 16.0 else 0
                updates.append("last_ldro = ?")
                params.append(ldro_flag)
            if freq_hz is not None:
                updates.append("last_freq_hz = ?")
                params.append(freq_hz)
            if rssi is not None:
                updates.append("last_rssi = ?")
                params.append(rssi)
            if snr is not None:
                updates.append("last_snr = ?")
                params.append(snr)
            if metrics is not None:
                updates.append("metrics_json = ?")
                params.append(json.dumps(metrics))

            params.append(node_id)
            sql = f"UPDATE devices SET {', '.join(updates)} WHERE node_id = ?"
            self.conn.execute(sql, params)
            self.conn.commit()

    def get_target_rf(self, node_id):
        """Returns detected RF modulation parameters for a node, or default SF11/BW125/LDRO."""
        with self.lock:
            row = self.conn.execute("SELECT last_sf, last_bw_khz, last_freq_hz, last_cr, last_ldro FROM devices WHERE node_id = ?", (node_id,)).fetchone()
            if row:
                sf_val = row["last_sf"] or 11
                bw_val = row["last_bw_khz"] or 125
                ldro_val = row["last_ldro"] if row["last_ldro"] is not None else (1 if ((2 ** sf_val) / float(max(1, bw_val))) >= 16.0 else 0)
                return {
                    "sf": sf_val,
                    "bw_khz": bw_val,
                    "freq_hz": row["last_freq_hz"] or 868100000,
                    "cr": row["last_cr"] or "4/5",
                    "ldro": int(ldro_val)
                }
            return {"sf": 11, "bw_khz": 125, "freq_hz": 868100000, "cr": "4/5", "ldro": 1}

    def record_telemetry(self, node_id, water_level=None, auto_mode=None, p1=None, p2=None, p3=None,
                         pond_pump=None, temp_c=None, humidity=None, battery_v=None, solar_w=None,
                         sf=None, bw_khz=None, rssi=None, snr=None, raw_hex=None):
        wl_pct = None
        if water_level is not None:
            wl_pct = [0.0, 33.3, 66.6, 100.0][min(3, max(0, water_level))]
        with self.lock:
            self.conn.execute("""
            INSERT INTO telemetry_history (
                node_id, water_level, water_level_pct, auto_mode, p1, p2, p3,
                pond_pump, temperature, humidity, battery_v, solar_w,
                sf, bw_khz, rssi, snr, raw_hex
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (
                node_id, water_level, wl_pct,
                1 if auto_mode else (0 if auto_mode is not None else None),
                1 if p1 else (0 if p1 is not None else None),
                1 if p2 else (0 if p2 is not None else None),
                1 if p3 else (0 if p3 is not None else None),
                1 if pond_pump else (0 if pond_pump is not None else None),
                temp_c, humidity, battery_v, solar_w,
                sf, bw_khz, rssi, snr, raw_hex
            ))
            self.conn.commit()

    def log_packet(self, direction, sender_id, target_id, msg_type, msg_type_str, summary,
                   sf=None, bw_khz=None, freq_mhz=None, payload_hex=None, rssi=None, snr=None, stat_str=None):
        with self.lock:
            self.conn.execute("""
            INSERT INTO packet_log (
                direction, sender_id, target_id, msg_type, msg_type_str,
                summary, sf, bw_khz, freq_mhz, payload_hex, rssi, snr, stat_str
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (
                direction, sender_id, target_id, msg_type, msg_type_str,
                summary, sf, bw_khz, freq_mhz, payload_hex, rssi, snr, stat_str
            ))
            self.conn.commit()

    def trigger_correlation(self, source_id, target_id, relation_type):
        with self.lock:
            self.conn.execute("""
            UPDATE node_correlations
            SET trigger_count = trigger_count + 1,
                last_triggered = CURRENT_TIMESTAMP
            WHERE source_node_id = ? AND target_node_id = ? AND relation_type = ?
            """, (source_id, target_id, relation_type))
            self.conn.commit()

    def get_devices(self):
        with self.lock:
            rows = self.conn.execute("SELECT * FROM devices ORDER BY node_id ASC").fetchall()
            return [dict(r) for r in rows]

    def get_correlations(self):
        with self.lock:
            rows = self.conn.execute("SELECT * FROM node_correlations ORDER BY id ASC").fetchall()
            return [dict(r) for r in rows]

    def get_recent_telemetry(self, limit=60, node_id=None):
        with self.lock:
            if node_id:
                rows = self.conn.execute("SELECT * FROM telemetry_history WHERE node_id = ? ORDER BY id DESC LIMIT ?", (node_id, limit)).fetchall()
            else:
                rows = self.conn.execute("SELECT * FROM telemetry_history ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
            return [dict(r) for r in reversed(rows)]

    def get_recent_packets(self, limit=100):
        with self.lock:
            rows = self.conn.execute("SELECT * FROM packet_log ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
            return [dict(r) for r in rows]

    def get_db_stats(self):
        with self.lock:
            dev_cnt = self.conn.execute("SELECT COUNT(*) FROM devices").fetchone()[0]
            corr_cnt = self.conn.execute("SELECT COUNT(*) FROM node_correlations").fetchone()[0]
            tele_cnt = self.conn.execute("SELECT COUNT(*) FROM telemetry_history").fetchone()[0]
            pkt_cnt = self.conn.execute("SELECT COUNT(*) FROM packet_log").fetchone()[0]
            size_bytes = os.path.getsize(self.db_path) if os.path.exists(self.db_path) else 0
            return {
                "db_file": os.path.basename(self.db_path),
                "size_kb": round(size_bytes / 1024, 1),
                "device_count": dev_cnt,
                "correlation_count": corr_cnt,
                "telemetry_count": tele_cnt,
                "packet_count": pkt_cnt,
                "wal_mode": True
            }

fleet_db = FleetDatabase()


# ─── Universal Node Registry ───────────────────────────────────────────────────

class NodeRegistry:
    """
    Universal registry for all devices in the LoRa RF ecosystem.
    Tracks live metrics, online/offline health, hardware roles, and simulation status.
    Synchronizes with SQLite fleet database for persistence across reboots.
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
                    "last_sf": 11,
                    "last_bw_khz": 125,
                    "last_freq_hz": 868100000,
                    "last_cr": "4/5",
                    "last_ldro": 1,
                    "metrics": {},
                    "prev_metrics": {}
                }
                fleet_db.upsert_device(node_id, name, n_type, role, is_simulated=is_simulated)
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
                    "last_sf": 11,
                    "last_bw_khz": 125,
                    "last_freq_hz": 868100000,
                    "last_cr": "4/5",
                    "last_ldro": 1,
                    "metrics": {},
                    "prev_metrics": {}
                }
            if node_id not in self.nodes:
                # Strictly limit auto-discovery to valid node addresses (1 <= node_id <= 16)
                if 1 <= node_id <= 16:
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
                        "last_sf": 11,
                        "last_bw_khz": 125,
                        "last_freq_hz": 868100000,
                        "last_cr": "4/5",
                        "last_ldro": 1,
                        "metrics": {},
                        "prev_metrics": {}
                    }
                    fleet_db.upsert_device(node_id, f"Field Node 0x{node_id:02X}", "GENERIC_NODE", "Auto-discovered LoRa Field Node")
                else:
                    return {
                        "id": node_id,
                        "id_hex": f"0x{node_id:02X}",
                        "name": f"Invalid Node 0x{node_id:02X}",
                        "type": "CORRUPTED",
                        "role": "Out-of-range Address",
                        "status": "INVALID",
                        "is_simulated": False,
                        "last_seen": 0,
                        "pkt_count": 0,
                        "last_rssi": None,
                        "last_snr": None,
                        "last_sf": 11,
                        "last_bw_khz": 125,
                        "last_freq_hz": 868100000,
                        "last_cr": "4/5",
                        "last_ldro": 1,
                        "metrics": {},
                        "prev_metrics": {}
                    }
            return self.nodes[node_id]

    def touch(self, node_id: int, rssi: float = None, snr: float = None, sf: int = None,
              bw_khz: int = None, freq_hz: int = None, cr: str = None, ldro: int = None):
        if node_id in (0x00, 0xFF) or node_id < 1 or node_id > 16:
            return
        with self.lock:
            node = self.get(node_id)
            if node.get("status") == "INVALID":
                return
            node["last_seen"] = time.time()
            node["pkt_count"] += 1
            if not node["is_simulated"]:
                node["status"] = "ONLINE"
            if rssi is not None:
                node["last_rssi"] = rssi
            if snr is not None:
                node["last_snr"] = snr
            if sf is not None:
                node["last_sf"] = sf
            if bw_khz is not None:
                node["last_bw_khz"] = bw_khz
            if freq_hz is not None:
                node["last_freq_hz"] = freq_hz
            if cr is not None and cr != "":
                node["last_cr"] = cr
            cur_sf = node.get("last_sf", 11)
            cur_bw = node.get("last_bw_khz", 125)
            if ldro is not None:
                node["last_ldro"] = 1 if ldro else 0
            else:
                node["last_ldro"] = 1 if ((2 ** cur_sf) / float(max(1, cur_bw))) >= 16.0 else 0

            # Mirror RF parameters to simulated Pond Node when Tank Node RF parameters change
            if node_id == NODE_TANK and NODE_POND in self.nodes:
                pond_node = self.nodes[NODE_POND]
                if sf is not None:
                    pond_node["last_sf"] = sf
                if bw_khz is not None:
                    pond_node["last_bw_khz"] = bw_khz
                if freq_hz is not None:
                    pond_node["last_freq_hz"] = freq_hz
                if cr is not None and cr != "":
                    pond_node["last_cr"] = cr
                pond_node["last_ldro"] = node["last_ldro"]

        # Sync to persistent SQLite DB
        fleet_db.touch_device(node_id, sf=sf, bw_khz=bw_khz, freq_hz=freq_hz, rssi=rssi, snr=snr, cr=cr, ldro=ldro)
        # If Tank Node updated SF/BW/CR/LDRO, align simulated Pond Node in DB to match network RF
        if node_id == NODE_TANK and any(x is not None for x in (sf, bw_khz, freq_hz, cr, ldro)):
            fleet_db.touch_device(NODE_POND, sf=sf, bw_khz=bw_khz, freq_hz=freq_hz, cr=cr, ldro=node["last_ldro"])

    def update_metrics(self, node_id: int, metrics: dict):
        if node_id in (0x00, 0xFF) or node_id < 1 or node_id > 16:
            return
        with self.lock:
            node = self.get(node_id)
            if node.get("status") == "INVALID":
                return
            node["prev_metrics"] = dict(node["metrics"])
            node["metrics"].update(metrics)
            node["last_seen"] = time.time()
            if not node["is_simulated"]:
                node["status"] = "ONLINE"

    def prune_ghost_nodes(self):
        with self.lock:
            to_remove = [nid for nid in list(self.nodes.keys()) if nid not in (NODE_GATEWAY, NODE_TANK, NODE_POND)]
            for nid in to_remove:
                del self.nodes[nid]
            return len(to_remove)

    def get_all(self):
        with self.lock:
            now = time.time()
            res = {}
            for nid, node in self.nodes.items():
                copy_node = dict(node)
                # Compute online status based on dual-rate timeout if physical node
                if not copy_node["is_simulated"] and nid != NODE_GATEWAY:
                    burst_active = copy_node.get("metrics", {}).get("burst_mode", False)
                    pumps_active = any(copy_node.get("metrics", {}).get(p, False) for p in ("p1", "p2", "p3", "pond_demand"))
                    # If burst mode or pumps are active: allow up to 180s (3m) before flagging timeout
                    # In Silent Idle Mode: allow up to 1900s (31m)
                    timeout_s = 180 if (burst_active or pumps_active) else 1900
                    if copy_node["last_seen"] == 0:
                        copy_node["status"] = "OFFLINE"
                    elif (now - copy_node["last_seen"]) > timeout_s:
                        copy_node["status"] = "TIMEOUT"
                    else:
                        copy_node["status"] = "ONLINE"
                elif nid == NODE_GATEWAY:
                    copy_node["status"] = "ONLINE" if gateway_ip else "WAITING"
                res[nid] = copy_node
            return res

node_registry = NodeRegistry()


# ─── Dynamic LoRa Time-on-Air (ToA) & Turnaround Guard Calculator ─────────────

def calc_lora_toa_s(payload_len: int, sf: int, bw_khz: int = 125, cr_str: str = "4/5", ldro: int = None) -> float:
    """Computes LoRa packet Time-on-Air (seconds) and automatic LDRO (Tsym >= 16.0 ms)."""
    sf = max(6, min(12, int(sf or 11)))
    bw_khz = max(7, int(bw_khz or 125))
    cr_denom = 5
    if isinstance(cr_str, str) and "/" in cr_str:
        try:
            cr_denom = max(5, min(8, int(cr_str.split("/")[1])))
        except Exception:
            cr_denom = 5
    t_sym_ms = (2 ** sf) / float(bw_khz)
    de = (1 if ldro else 0) if ldro is not None else (1 if t_sym_ms >= 16.0 else 0)
    t_preamble_ms = (8.0 + 4.25) * t_sym_ms
    num = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0
    den = 4.0 * (sf - 2.0 * de)
    payload_symb = 8.0
    if den > 0:
        import math
        c = math.ceil(num / den)
        if c > 0:
            payload_symb += c * cr_denom
    return (t_preamble_ms + payload_symb * t_sym_ms) / 1000.0

def get_turnaround_guard_s(node_id: int = NODE_TANK, payload_len: int = 17) -> float:
    """Returns dynamic half-duplex turnaround guard window (1.2s - 1.8s+ for SF11/SF12)."""
    rf = fleet_db.get_target_rf(node_id)
    sf = rf.get("sf", 11)
    bw = rf.get("bw_khz", 125)
    cr = rf.get("cr", "4/5")
    ldro = rf.get("ldro", None)
    toa_s = calc_lora_toa_s(payload_len, sf, bw, cr, ldro)
    guard_s = toa_s * 1.65 + 0.25
    if sf >= 12 and guard_s < 1.85:
        guard_s = 1.85
    elif sf >= 11 and guard_s < 1.25:
        guard_s = 1.25
    return round(guard_s, 2)


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
        self.tx_interval_s = 120        # Periodic status broadcast interval (event-driven, low-chatter)
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
        self.local_operator_stop = False     # Explicit stop by operator at the pond station
        self.local_operator_override = False # Explicit manual run by operator at pond station

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

            # First-hand notification directly to Tank Node over LoRa RF (single snappy MSG_CMD frame)
            if notify_tank_first_hand and self.rf_tx_enabled and self.enabled:
                safe_print(f"  🤝 [POND -> TANK FIRST-HAND] Dispatched local pump state={('ON' if running else 'OFF')} to Tank (0x02) | Awaiting Accord...")
                self.last_tx_time = now
                def _dispatch_first_hand(run_state, reason_text):
                    # Send 1 direct command packet to align Tank's demand switch (Tank replies with 1 MSG_STATUS)
                    send_node_cmd(NODE_POND, NODE_TANK, CMD_TARGET_POND, CMD_ACTION_SET, 1 if run_state else 0)

                threading.Thread(target=_dispatch_first_hand, args=(running, reason), daemon=True).start()
                self.last_accord_notify = now

    def check_tank_accord(self, tank_pond_flag: bool):
        """
        Consensus Accord Handshake Protocol:
        Verifies whether Tank Node (0x02) and Pond Node (0x03) are in mutual agreement.
        - When Tank requests water refill (tank_pond_flag=True):
            * If Pond is healthy and not under local operator stop, Pond starts the pump!
            * If Pond was stopped by operator at the pond station, or pond level is low (<20%),
              Pond re-notifies Tank over LoRa up to 3 times to cancel demand.
        - When Tank refill is satisfied (tank_pond_flag=False):
            * If Pond pump was running for Tank refill (and not on local override), Pond stops the pump.
            * Common accord is confirmed and status beacon acknowledged.
        """
        with self.lock:
            if not self.enabled:
                return

            if tank_pond_flag:
                # Case 1: Tank demands water refill
                if not self.pump_running:
                    if self.local_operator_stop:
                        # Pond was stopped by operator locally at the pond station
                        self.tank_accord = False
                        if self.accord_retry_count < 3:
                            self.accord_retry_count += 1
                            safe_print(f"\n  ⚠️ [ACCORD HANDSHAKE #{self.accord_retry_count}/3] Tank demands refill, but Pond was stopped by local operator!")
                            safe_print(f"  🔄 [ACCORD NOTIFICATION] Informing Tank Node (0x02) that Pond is locally stopped...")
                            if self.rf_tx_enabled:
                                def _delayed_local_stop():
                                    time.sleep(0.18)
                                    send_node_cmd(NODE_POND, NODE_TANK, CMD_TARGET_POND, CMD_ACTION_SET, 0)
                                threading.Thread(target=_delayed_local_stop, daemon=True).start()
                            self.last_accord_notify = time.time()
                        else:
                            safe_print(f"  ⏸️ [ACCORD PAUSED] Tank demands refill != Pond local stop. Max retries (3) reached.")
                    elif self.pond_level_pct < 20.0:
                        # Low reservoir cutoff
                        self.tank_accord = False
                        if self.accord_retry_count < 3:
                            self.accord_retry_count += 1
                            safe_print(f"\n  ⚠️ [ACCORD HANDSHAKE #{self.accord_retry_count}/3] Tank demands refill, but Pond water level is critically low ({self.pond_level_pct:.1f}% < 20%)!")
                            if self.rf_tx_enabled:
                                def _delayed_low_cutoff():
                                    time.sleep(0.18)
                                    send_node_cmd(NODE_POND, NODE_TANK, CMD_TARGET_POND, CMD_ACTION_SET, 0)
                                threading.Thread(target=_delayed_low_cutoff, daemon=True).start()
                            self.last_accord_notify = time.time()
                    else:
                        # Normal refill demand from Tank -> START PUMP!
                        safe_print(f"\n  🌊 [ACCORD AUTO-START] Tank Node (0x02) requested refill -> Energizing Pond Pump...")
                        self.set_pump(True, reason="Tank refill demand auto-accord", notify_tank_first_hand=False)
                        self.tank_accord = True
                        self.accord_retry_count = 0
                        if self.rf_tx_enabled:
                            def _delayed_accord_ack():
                                time.sleep(0.18)
                                self.send_status_beacon(target_id=NODE_TANK, reason="Tank refill accord ack")
                            threading.Thread(target=_delayed_accord_ack, daemon=True).start()
                        safe_print(f"  🤝 [COMMON ACCORD REACHED] Tank Node (0x02) & Pond Node (0x03) in full agreement! (Pond Pump: RUNNING)\n")
                else:
                    # Tank demands refill and Pond is already running
                    if not self.tank_accord:
                        self.tank_accord = True
                        self.accord_retry_count = 0
                        safe_print(f"\n  🤝 [COMMON ACCORD REACHED] Tank Node (0x02) & Pond Node (0x03) in full agreement! (Pond Pump: RUNNING)\n")
                    self.local_operator_stop = False
            else:
                # Case 2: Tank is NOT demanding refill (tank_pond_flag == False)
                if self.pump_running:
                    now = time.time()
                    if self.local_operator_override:
                        # Running under manual local operator override at Pond
                        if not self.tank_accord:
                            self.tank_accord = True
                            self.accord_retry_count = 0
                            safe_print(f"\n  ℹ️ [LOCAL OVERRIDE] Pond pump running by local operator command while Tank demand is idle.\n")
                    elif self.pump_started_at and (now - self.pump_started_at < 6.0):
                        # Grace window: pump was energized < 6s ago, do not immediately shut down on in-flight telemetry
                        safe_print(f"\n  ⏳ [ACCORD GRACE] Pond pump started {now - self.pump_started_at:.1f}s ago - holding state for Tank sync...\n")
                    else:
                        # Tank refill satisfied -> STOP PUMP!
                        safe_print(f"\n  🛑 [ACCORD AUTO-STOP] Tank Node (0x02) refill satisfied -> Stopping Pond Pump...")
                        self.set_pump(False, reason="Tank refill satisfied auto-accord", notify_tank_first_hand=False)
                        self.tank_accord = True
                        self.accord_retry_count = 0
                        if self.rf_tx_enabled:
                            def _delayed_stop_ack():
                                time.sleep(0.18)
                                self.send_status_beacon(target_id=NODE_TANK, reason="Tank stop accord ack")
                            threading.Thread(target=_delayed_stop_ack, daemon=True).start()
                        safe_print(f"  🤝 [COMMON ACCORD REACHED] Tank Node (0x02) & Pond Node (0x03) in full agreement! (Pond Pump: STOPPED)\n")
                else:
                    # Both agree pump should be stopped
                    if not self.tank_accord:
                        self.tank_accord = True
                        self.accord_retry_count = 0
                        safe_print(f"\n  🤝 [COMMON ACCORD REACHED] Tank Node (0x02) & Pond Node (0x03) in full agreement! (Pond Pump: STOPPED)\n")
                    self.local_operator_stop = False

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
                "local_operator_stop": self.local_operator_stop,
                "local_operator_override": self.local_operator_override,
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
        Payload is 17 bytes (5 bytes header + 12 bytes StatusPayload).
        """
        if not self.enabled:
            return False

        with self.lock:
            # Construct 17-byte LoRaPacket Status Frame:
            # [0..1] Magic (6B 5A)
            # [2]    Target ID (0x01, 0x02, or 0xFF)
            # [3]    Sender ID (0x03 - Pond Node)
            # [4]    MsgType   (0x01 - MSG_STATUS)
            # [5]    Flags     (Bit0: Auto=1, Bit4: Pond Pump running)
            # [6]    Water     (0-3 scaled from pond level %)
            # [7..8] Temp      (int16 little endian = temp_c * 10)
            # [9]    Humidity  (level % 0-100)
            # [10]   Reserved  (battery deci-volts, e.g. 128 = 12.8V)
            # [11..13] run_left[3] = 0
            # [14..16] cd_left[3]  = 0
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
            payload.extend([0, 0, 0, 0, 0, 0]) # 17 bytes total

            self.last_tx_time = time.time()

        # Dynamic RF Modulation Adaptation based on target device profile in DB
        rf_target_id = target_id if target_id not in (0x00, 0xFF) else NODE_TANK
        rf = fleet_db.get_target_rf(rf_target_id)
        target_sf = rf.get("sf", 11)
        target_bw = rf.get("bw_khz", 125)
        target_freq = rf.get("freq_hz", 868_100_000)
        target_cr = rf.get("cr", "4/5")
        target_ldro = rf.get("ldro", 1)

        tx_json = build_custom(bytes(payload), freq=target_freq, power=22, sf=target_sf, bw=target_bw, cr=target_cr, ldro=target_ldro, invert=True)
        success = send(tx_json)
        if success:
            ldro_str = " [LDRO]" if target_ldro else ""
            toa_ms = int(calc_lora_toa_s(len(payload), target_sf, target_bw, target_cr, target_ldro) * 1000)
            safe_print(f"  📡 [POND SIM -> RF TX | SF{target_sf}BW{target_bw}{ldro_str} | ToA:{toa_ms}ms] Broadcasted MSG_STATUS via LoRa ({reason}) | Pump={'ON' if self.pump_running else 'OFF'} Level={self.pond_level_pct}%")
            # Record in packet history
            record_packet_history("MSG_STATUS (SIM)", f"0x{NODE_POND:02X}", f"0x{target_id:02X}",
                                  f"Pond Sim Beacon (SF{target_sf}): Pump={'ON' if self.pump_running else 'OFF'} Lvl={self.pond_level_pct}% Batt={self.battery_v}V",
                                  bytes(payload).hex().upper())
            fleet_db.log_packet("DOWNLINK", NODE_POND, target_id, MSG_STATUS, "MSG_STATUS (SIM)",
                                f"Pond Beacon: Pump={'ON' if self.pump_running else 'OFF'} Level={self.pond_level_pct}%",
                                sf=target_sf, bw_khz=target_bw, freq_mhz=target_freq/1e6,
                                payload_hex=bytes(payload).hex().upper(), stat_str="TX")
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
    rf = fleet_db.get_target_rf(NODE_TANK)
    return build_custom(
        payload,
        freq=rf.get("freq_hz", 868_100_000),
        power=22,
        sf=rf.get("sf", 11),
        bw=rf.get("bw_khz", 125),
        cr=rf.get("cr", "4/5"),
        ldro=rf.get("ldro", 1),
        invert=True
    )

def build_custom(payload: bytes, freq=868_100_000, power=22, sf=11,
                 bw=125, cr="4/5", ldro=None, invert=True, no_crc=False,
                 imme=True, rfch=0, modu="LORA") -> bytes:
    freq_mhz = freq / 1e6
    data_b64 = base64.b64encode(payload).decode('utf-8')
    datr_str = f"SF{sf}BW{bw}" if modu == "LORA" else str(sf)
    if ldro is None:
        ldro = 1 if ((2 ** int(sf)) / float(max(1, int(bw)))) >= 16.0 else 0
    pkt = {
        "imme": imme,
        "freq": round(freq_mhz, 4),
        "rfch": rfch,
        "powe": power,
        "modu": modu,
        "datr": datr_str,
        "codr": cr,
        "ldro": bool(ldro),
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
    Dynamically targets the exact spreading factor, bandwidth, coding rate, and LDRO detected for target_id.
    """
    rf = fleet_db.get_target_rf(target_id if target_id not in (0x00, 0xFF) else NODE_TANK)
    target_sf = rf.get("sf", 11)
    target_bw = rf.get("bw_khz", 125)
    target_freq = rf.get("freq_hz", 868_100_000)
    target_cr = rf.get("cr", "4/5")
    target_ldro = rf.get("ldro", 1)

    pkt = bytearray([0x6B, 0x5A, target_id, sender_id, MSG_CMD, target_byte, action_byte, val_byte])
    tx_json = build_custom(bytes(pkt), freq=target_freq, power=22, sf=target_sf, bw=target_bw, cr=target_cr, ldro=target_ldro, invert=True)
    if send(tx_json):
        t_name = {0x01: "MODE", 0x02: "P1", 0x04: "P2", 0x08: "P3", 0x10: "POND_PUMP"}.get(target_byte, f"0x{target_byte:02X}")
        a_name = "TOGGLE" if action_byte == 0 else ("SET ON" if val_byte == 1 else "SET OFF")
        ldro_str = " [LDRO]" if target_ldro else ""
        toa_ms = int(calc_lora_toa_s(len(pkt), target_sf, target_bw, target_cr, target_ldro) * 1000)
        safe_print(f"  [TX -> 0x{target_id:02X} | SF{target_sf}BW{target_bw}{ldro_str} | ToA:{toa_ms}ms] Cmd from 0x{sender_id:02X}: {t_name} -> {a_name}")
        record_packet_history("MSG_CMD (TX)", f"0x{sender_id:02X}", f"0x{target_id:02X}",
                              f"Cmd: {t_name} -> {a_name} (SF{target_sf}{ldro_str})", bytes(pkt).hex().upper())
        fleet_db.log_packet("DOWNLINK", sender_id, target_id, MSG_CMD, "MSG_CMD",
                            f"Cmd: {t_name} -> {a_name}", sf=target_sf, bw_khz=target_bw, freq_mhz=target_freq/1e6,
                            payload_hex=bytes(pkt).hex().upper(), stat_str="TX")
        return True
    return False


# ─── Universal Payload Decoders & Parsers ─────────────────────────────────────

def decode_device_payload(payload_bytes: bytes, rssi: float = None, snr: float = None,
                          sf: int = 11, bw_khz: int = 125, freq_hz: int = 868100000,
                          codr: str = "4/5", stat_str: str = "CRC_OK"):
    if len(payload_bytes) < 5:
        return None
        
    magic = payload_bytes[0:2]
    if magic != b'\x6B\x5A':
        return None

    target_id = payload_bytes[2]
    sender_id = payload_bytes[3]
    msg_type  = payload_bytes[4]

    # Validate sender_id and target_id boundaries (IDs must be 1..16 or broadcast 0xFF)
    if sender_id < 1 or sender_id > 16:
        return None
    if target_id < 1 or (target_id > 16 and target_id != 0xFF):
        return None
    if msg_type not in (MSG_STATUS, MSG_CMD, 0x03):
        return None

    # Detect reported LDRO from Tank StatusPayload.reserved (bit 4 = 0x10) if available
    reported_ldro = None
    if len(payload_bytes) >= 11 and sender_id == NODE_TANK:
        reported_ldro = 1 if (payload_bytes[10] & 0x10) else 0

    # Touch both nodes in the registry (filter broadcast/null) with detected RF modulation parameters
    if sender_id not in (0x00, 0xFF):
        node_registry.touch(sender_id, rssi=rssi, snr=snr, sf=sf, bw_khz=bw_khz,
                            freq_hz=freq_hz, cr=codr or "4/5", ldro=reported_ldro)
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
        burst_mode = bool(reserved & 0x08) if sender_id == NODE_TANK else None
        ldro_active = bool(reserved & 0x10) if sender_id == NODE_TANK else bool(((2 ** sf) / float(max(1, bw_khz))) >= 16.0)
        pond_ack_rx = bool(reserved & 0x20) if sender_id == NODE_TANK else None

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
            if sender_id == NODE_TANK and prev.get("burst_mode") is not None and prev.get("burst_mode") != burst_mode:
                deltas.append(f"TelemRate: {'BURST' if prev.get('burst_mode') else 'IDLE'} -> {'BURST' if burst_mode else 'IDLE'}")

        # Update node registry
        metrics_dict = {
            "water_level": water,
            "auto_mode": auto_mode,
            "p1": p1, "p2": p2, "p3": p3,
            "pond_demand": pond_flag,
            "temp_c": round(temp_raw / 10.0, 1),
            "humidity": humidity,
            "flags": flags,
            "ldro_active": ldro_active
        }
        if sender_id == NODE_TANK:
            metrics_dict["allowed_p1"] = allowed_p1
            metrics_dict["allowed_p2"] = allowed_p2
            metrics_dict["allowed_p3"] = allowed_p3
            metrics_dict["burst_mode"] = burst_mode
            metrics_dict["pond_ack"] = pond_ack_rx
            metrics_dict["run_left"] = run_left
            metrics_dict["cd_left"]  = cd_left
        node_registry.update_metrics(sender_id, metrics_dict)

        # Consensus Accord Protocol Verification between Tank (0x02) and Pond (0x03)
        if sender_id == NODE_TANK:
            pond_sim.check_tank_accord(tank_pond_flag=pond_flag)
            # If Tank reports demand=pond_flag, Pond is already in sync, but Tank hasn't yet seen Pond's ACK (pond_ack_rx != pond_sim.pump_running), send 1 snappy ACK
            if pond_ack_rx is not None and (pond_ack_rx != pond_sim.pump_running) and (pond_flag == pond_sim.pump_running):
                if pond_sim.rf_tx_enabled and (time.time() - pond_sim.last_tx_time > 1.5):
                    def _sync_ack():
                        time.sleep(0.18)
                        pond_sim.send_status_beacon(target_id=NODE_TANK, reason="Sync Tank Pond-ACK bit")
                    threading.Thread(target=_sync_ack, daemon=True).start()

        rate_tag = ("BURST" if burst_mode else "IDLE") if sender_id == NODE_TANK else "SIM"
        ldro_tag = "LDRO" if ldro_active else "NO-LDRO"
        record_packet_history("MSG_STATUS", f"0x{sender_id:02X}", f"0x{target_id:02X}",
                              f"Lvl={water}/3 Auto={int(auto_mode)} Relays=[{int(p1)},{int(p2)},{int(p3)}] [{rate_tag}|{ldro_tag}] {temp_raw/10.0:.1f}°C (SF{sf})",
                              payload_bytes.hex().upper())

        # Persist packet and telemetry into SQLite Database
        summary_str = f"Lvl={water}/3 Auto={int(auto_mode)} Relays=[{int(p1)},{int(p2)},{int(p3)}] [{rate_tag}|{ldro_tag}] {temp_raw/10.0:.1f}°C"
        fleet_db.log_packet("UPLINK", sender_id, target_id, MSG_STATUS, "MSG_STATUS",
                            summary_str, sf=sf, bw_khz=bw_khz, freq_mhz=freq_hz/1e6,
                            payload_hex=payload_bytes.hex().upper(), rssi=rssi, snr=snr, stat_str=stat_str)

        if sender_id == NODE_TANK:
            fleet_db.record_telemetry(NODE_TANK, water_level=water, auto_mode=auto_mode,
                                     p1=p1, p2=p2, p3=p3, pond_pump=pond_flag,
                                     temp_c=temp_raw/10.0, humidity=humidity,
                                     sf=sf, bw_khz=bw_khz, rssi=rssi, snr=snr,
                                     raw_hex=payload_bytes.hex().upper())
            # Check and record active inter-node hydraulic correlations
            if water == 0 and auto_mode:
                fleet_db.trigger_correlation(NODE_TANK, NODE_TANK, "DRY_RUN_INTERLOCK")
            if pond_flag:
                fleet_db.trigger_correlation(NODE_TANK, NODE_POND, "HYDRAULIC_REFILL_DEMAND")

        extra_info = f" (Mask:[{int(allowed_p1)},{int(allowed_p2)},{int(allowed_p3)}] Rate:{'BURST' if burst_mode else 'IDLE'} LDRO:{'ON' if ldro_active else 'OFF'})" if sender_id == NODE_TANK else ""
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
                              f"Target={target_str} Act={action_str} Val={value_str} (SF{sf})",
                              payload_bytes.hex().upper())

        fleet_db.log_packet("UPLINK", sender_id, target_id, MSG_CMD, "MSG_CMD",
                            f"Target={target_str} Act={action_str} Val={value_str}",
                            sf=sf, bw_khz=bw_khz, freq_mhz=freq_hz/1e6,
                            payload_hex=payload_bytes.hex().upper(), rssi=rssi, snr=snr, stat_str=stat_str)

        if (target & 0x10):
            fleet_db.trigger_correlation(sender_id, NODE_POND, "HYDRAULIC_REFILL_DEMAND")

        result.extend([
            f"  │   Type       = MSG_CMD (8 bytes)",
            f"  │   Target     = {target_str}",
            f"  │   Action     = {action_str}",
            f"  │   Value      = {value_str}"
        ])

        # Active Interception by Pond Node Simulator (NODE_POND = 0x03)
        if (target_id in (NODE_POND, NODE_BROADCAST)) and (target & 0x10):
            turn_on = bool(value == 1) if action == 0x01 else not pond_sim.pump_running
            if turn_on:
                pond_sim.local_operator_stop = False
            # Actuate pump state on simulated pond node
            pond_sim.set_pump(turn_on, reason=f"LoRa CMD from 0x{sender_id:02X}", notify_tank_first_hand=False)
            pond_sim.tank_accord = True
            pond_sim.accord_retry_count = 0
            # Broadcast confirmation status beacon to Tank Node after fast 180ms turnaround
            def _delayed_cmd_ack():
                time.sleep(0.18)
                pond_sim.send_status_beacon(target_id=NODE_TANK, reason="Ack Tank Refill CMD")
            threading.Thread(target=_delayed_cmd_ack, daemon=True).start()
    else:
        result.append(f"  │   Generic / Custom Payload ({len(payload_bytes)} bytes)")
        record_packet_history(f"MSG_0x{msg_type:02X}", f"0x{sender_id:02X}", f"0x{target_id:02X}",
                              f"Raw {len(payload_bytes)} bytes (SF{sf})", payload_bytes.hex().upper())
        fleet_db.log_packet("UPLINK", sender_id, target_id, msg_type, f"MSG_0x{msg_type:02X}",
                            f"Raw {len(payload_bytes)} bytes", sf=sf, bw_khz=bw_khz, freq_mhz=freq_hz/1e6,
                            payload_hex=payload_bytes.hex().upper(), rssi=rssi, snr=snr, stat_str=stat_str)

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

            # Parse Spreading Factor and Bandwidth dynamically from datr string (e.g. 'SF11BW125')
            sf = 11
            bw_khz = 125
            if datr and datr.startswith("SF"):
                try:
                    parts = datr[2:].split("BW")
                    sf = int(parts[0])
                    bw_khz = int(parts[1]) if len(parts) > 1 else 125
                except Exception:
                    pass
            freq_hz = int(freq * 1e6)

            try:
                payload_bytes = base64.b64decode(data_b64 + "==")
                payload_disp_HEX = " ".join(f"{b:02X}" for b in payload_bytes)
                if stat != 1:
                    # Packet has CRC error or missing CRC — strictly discard to prevent ghost node injection!
                    payload_disp = f"⚠️ [CRC ERROR ({stat_str})] {payload_disp_HEX}"
                else:
                    device_decode = decode_device_payload(payload_bytes, rssi=rssi_s, snr=lsnr,
                                                          sf=sf, bw_khz=bw_khz, freq_hz=freq_hz,
                                                          codr=codr or "4/5", stat_str=stat_str)
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

FLEET_HTML_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fleet_scada.html")

def get_fleet_html() -> bytes:
    if os.path.exists(FLEET_HTML_FILE):
        try:
            with open(FLEET_HTML_FILE, "rb") as f:
                return f.read()
        except Exception as e:
            safe_print(f"  [!] Error reading fleet_scada.html: {e}")
    return b"""<!DOCTYPE html><html><body style='background:#050811;color:#fff;font-family:sans-serif;padding:30px;'><h2>SX1302 LoRa SCADA Deck</h2><p>fleet_scada.html missing or loading...</p></body></html>"""


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
                pumps_active = any(tm.get(p, False) for p in ("p1", "p2", "p3", "pond_demand"))
                watchdog_limit_s = 180 if (tm.get("burst_mode", False) or pumps_active) else 1900
                if tank.get("last_seen", 0) > 0 and (time.time() - tank["last_seen"]) > watchdog_limit_s:
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
                "db_stats": fleet_db.get_db_stats(),
                "correlations": fleet_db.get_correlations(),
                "db_devices": fleet_db.get_devices(),
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

        # ── Universal SQLite Fleet Database Endpoints ─────────────────────────
        if self.path == "/api/db/devices":
            devs = fleet_db.get_devices()
            body = json.dumps(devs).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/api/db/correlations":
            corrs = fleet_db.get_correlations()
            body = json.dumps(corrs).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path.startswith("/api/db/history"):
            hist = fleet_db.get_recent_telemetry(limit=100)
            body = json.dumps(hist).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path.startswith("/api/db/packets"):
            pkts = fleet_db.get_recent_packets(limit=100)
            body = json.dumps(pkts).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/api/db/stats":
            stats = fleet_db.get_db_stats()
            body = json.dumps(stats).encode("utf-8")
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

        html_bytes = get_fleet_html()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(html_bytes)))
        self.end_headers()
        self.wfile.write(html_bytes)

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
    last_gw_keepalive = 0.0
    while True:
        try:
            now = time.time()
            pond_sim.step()

            # Gateway -> Tank Watchdog Keepalive (Every 45s if no downlink was sent in the last 30s)
            # Adapts automatically to Tank Node's detected Spreading Factor, Bandwidth, CR & LDRO
            if gateway_ip and (now - last_gw_keepalive >= 45.0) and (now - pond_sim.last_tx_time >= 30.0):
                last_gw_keepalive = now
                rf = fleet_db.get_target_rf(NODE_TANK)
                target_sf = rf.get("sf", 11)
                target_bw = rf.get("bw_khz", 125)
                target_freq = rf.get("freq_hz", 868_100_000)
                target_cr = rf.get("cr", "4/5")
                target_ldro = rf.get("ldro", 1)
                gw_pkt = bytearray([0x6B, 0x5A, NODE_TANK, NODE_GATEWAY, MSG_STATUS, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0])
                tx_json = build_custom(bytes(gw_pkt), freq=target_freq, power=22, sf=target_sf, bw=target_bw, cr=target_cr, ldro=target_ldro, invert=True)
                send(tx_json)
                fleet_db.trigger_correlation(NODE_GATEWAY, NODE_TANK, "GATEWAY_KEEPALIVE")

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
    /nodes [prune]            List all registered nodes, or prune ghost/corrupted node IDs
    /devices                  Display full table of registered devices in SQLite database
    /correlations             List active inter-node hydraulic rules, consensus & triggers
    /db [stats]               Show SQLite database status, size, and record counts
    /sf <node_id> [sf_val]    Inspect or override target downlink Spreading Factor (7-12)
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
        url = f"http://192.168.1.8:{active_web_port}"
        safe_print(f"  🌐 Launching Fleet Web Dashboard: {url}")
        try:
            webbrowser.open(url)
        except Exception as e:
            safe_print(f"  [!] Open browser manually: {url}")
        return

    # ── Database & Fleet Device Registry Commands ─────────────────────────────
    if line in ("/db", "/db status", "/db stats"):
        stats = fleet_db.get_db_stats()
        safe_print("\n  ┌─ SQLite Universal Fleet Database Status ───────────────")
        safe_print(f"  │ Database File     : {stats['db_file']} ({stats['size_kb']} KB)")
        safe_print(f"  │ WAL Journal Mode  : ACTIVE (Thread-Safe Concurrent)")
        safe_print(f"  │ Registered Devices: {stats['device_count']}")
        safe_print(f"  │ Inter-Node Rules  : {stats['correlation_count']}")
        safe_print(f"  │ Telemetry Logs    : {stats['telemetry_count']} records")
        safe_print(f"  │ LoRa Packet Logs  : {stats['packet_count']} frames")
        safe_print("  └────────────────────────────────────────────────────────\n")
        return

    if line == "/devices":
        devs = fleet_db.get_devices()
        safe_print(f"\n  ┌─ Fleet Devices Database ({len(devs)} registered) ───────────────────────────────────────────────────")
        safe_print("  │ ID   Name                     Type            RF Mod          Signal       Pkts   Status    ")
        safe_print("  ├────  ───────────────────────  ──────────────  ──────────────  ───────────  ─────  ──────────")
        for d in devs:
            nid_hex = f"0x{d['node_id']:02X}"
            rf_str = f"SF{d['last_sf']}BW{d['last_bw_khz']}" + (" [LDRO]" if d['last_ldro'] else "")
            sig_str = f"{d['last_rssi'] or 0:.0f}dBm/{d['last_snr'] or 0:.0f}dB" if d['last_rssi'] is not None else "--/--"
            sim_str = " [SIM]" if d['is_simulated'] else ""
            stat_str = f"{d['status']}{sim_str}"
            safe_print(f"  │ {nid_hex:<4} {d['name'][:23]:<23}  {d['type'][:14]:<14}  {rf_str:<14}  {sig_str:<11}  {d['pkt_count']:<5}  {stat_str:<10}")
        safe_print("  └──────────────────────────────────────────────────────────────────────────────────────────────\n")
        return

    if line == "/correlations":
        corrs = fleet_db.get_correlations()
        safe_print(f"\n  ┌─ Inter-Node Hydraulic Correlations & Rules Matrix ({len(corrs)} active) ──────────────────────────")
        safe_print("  │ Link          Relation Type             Triggers  Last Triggered       Rule Description")
        safe_print("  ├─────────────  ────────────────────────  ────────  ───────────────────  ─────────────────────────────────")
        for c in corrs:
            link = f"0x{c['source_node_id']:02X} -> 0x{c['target_node_id']:02X}"
            last_t = c['last_triggered'] or "Never"
            safe_print(f"  │ {link:<13}  {c['relation_type'][:24]:<24}  {c['trigger_count']:<8}  {last_t[:19]:<19}  {c['rule_desc']}")
        safe_print("  └────────────────────────────────────────────────────────────────────────────────────────────────────\n")
        return

    if line.startswith("/sf"):
        parts = line.split()
        if len(parts) >= 2:
            try:
                nid = int(parts[1], 0)
                if len(parts) >= 3:
                    new_sf = int(parts[2])
                    if new_sf < 6 or new_sf > 12:
                        safe_print("  [!] Spreading factor must be between 6 and 12.")
                        return
                    node_registry.touch(nid, sf=new_sf)
                    fleet_db.touch_device(nid, sf=new_sf)
                    if nid == NODE_TANK:
                        node_registry.touch(NODE_POND, sf=new_sf)
                        fleet_db.touch_device(NODE_POND, sf=new_sf)
                    safe_print(f"  🎯 [SF OVERRIDE] Target node 0x{nid:02X} downlink SF set to SF{new_sf}!")
                rf = fleet_db.get_target_rf(nid)
                toa_ms = int(calc_lora_toa_s(17, rf['sf'], rf['bw_khz'], rf['cr'], rf['ldro']) * 1000)
                guard_s = get_turnaround_guard_s(nid, 17)
                safe_print(f"  📡 Node 0x{nid:02X} Target RF: SF{rf['sf']} BW{rf['bw_khz']} kHz CR{rf['cr']} Freq {rf['freq_hz']/1e6:.3f} MHz LDRO={'ACTIVE' if rf['ldro'] else 'OFF'} | ToA: {toa_ms}ms (Guard: {guard_s}s)")
                return
            except Exception as e:
                safe_print(f"  [!] Invalid /sf syntax: {e}")
                return
        safe_print("  Usage: /sf <node_id> [sf_value (6-12)]")
        return

    # ── /nodes Command ────────────────────────────────────────────────────────
    if line.startswith("/nodes"):
        parts = line.split()
        if len(parts) > 1 and parts[1].lower() in ("prune", "clear", "clean"):
            count = node_registry.prune_ghost_nodes()
            safe_print(f"\n  🧹 [PRUNED GHOST NODES] Removed {count} ghost/unregistered node(s) from registry.")
            return
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
                pond_sim.local_operator_stop = False
                pond_sim.local_operator_override = True
                pond_sim.set_pump(True, "Local Station Start Button", notify_tank_first_hand=True)
            elif sub_cmd in ("stop", "off", "0"):
                pond_sim.local_operator_stop = True
                pond_sim.local_operator_override = False
                pond_sim.set_pump(False, "Local Station Stop Button", notify_tank_first_hand=True)
            return

        if len(parts) >= 3 and parts[1] == "sim":
            sub_cmd = parts[2].lower()
            if sub_cmd in ("start", "on", "1"):
                pond_sim.local_operator_stop = False
                pond_sim.local_operator_override = True
                pond_sim.set_pump(True, "CLI /pond sim start", notify_tank_first_hand=True)
            elif sub_cmd in ("stop", "off", "0"):
                pond_sim.local_operator_stop = True
                pond_sim.local_operator_override = False
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
        telem_rate = "ACTIVE BURST" if m.get("burst_mode") else "SILENT IDLE"
        ldro_stat = "ACTIVE (ON)" if m.get("ldro_active") else "OFF"
        rf_info = fleet_db.get_target_rf(NODE_TANK)
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
        safe_print(f"  │ System Mode : {mode} | Telem Rate: {telem_rate}")
        safe_print(f"  │ RF Link     : SF{rf_info['sf']} BW{rf_info['bw_khz']}kHz CR{rf_info['cr']} | LDRO: {ldro_stat}")
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
                rf = fleet_db.get_target_rf(tgt if tgt not in (0x00, 0xFF) else NODE_TANK)
                target_sf = rf.get("sf", 9)
                target_bw = rf.get("bw_khz", 125)
                target_freq = rf.get("freq_hz", 868_100_000)
                tx_json = build_custom(bytes(pkt), freq=target_freq, power=14, sf=target_sf, bw=target_bw, cr="4/5", invert=True)
                if send(tx_json):
                    ldro_str = " [LDRO]" if rf.get("ldro") else ""
                    safe_print(f"  [TX Frame | SF{target_sf}BW{target_bw}{ldro_str}] Sent {len(pkt)} bytes (0x{src:02X} → 0x{tgt:02X}, type=0x{mtype:02X})")
                    record_packet_history(f"TYPE_0x{mtype:02X}", f"0x{src:02X}", f"0x{tgt:02X}", f"Custom packet (SF{target_sf})", pkt.hex().upper())
                    fleet_db.log_packet("DOWNLINK", src, tgt, mtype, f"TYPE_0x{mtype:02X}", "Custom injected packet",
                                        sf=target_sf, bw_khz=target_bw, freq_mhz=target_freq/1e6, payload_hex=pkt.hex().upper(), stat_str="TX")
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