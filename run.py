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
        ctypes.windll.kernel32.SetConsoleTitleW("SX1302 LoRa Gateway Station [python run.py]")
    except Exception:
        pass

# Import prompt_toolkit components for advanced thread-safe terminal handling
from prompt_toolkit import PromptSession
from prompt_toolkit.patch_stdout import patch_stdout

# --- CONFIGURATION ---
LISTEN_PORT_UP   = 5555   # gateway -> PC
SEND_PORT_DOWN   = 5556   # PC -> gateway
TX_QUEUE_SIZE    = 32     # must match raw_lora_app.c
STATION_WEB_PORT = 8080   # Embedded Web Station GUI
# ---------------------

packet_history = []
prev_tank_status = {}
active_web_port = STATION_WEB_PORT


PKT_TYPE_HEARTBEAT = 0x02
PKT_TYPE_BOOT      = 0x03

HB_FMT   = ">BQIIIIIIBBII"
BOOT_FMT = ">BQ32s"

HB_SIZE     = struct.calcsize(HB_FMT)    
BOOT_SIZE   = struct.calcsize(BOOT_FMT)  

sock        = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
gateway_ip  = None
pkt_counter = 0
last_hb     = {}

# Tank Node & Pond Simulator State
last_tank_status = {}
pond_sim = {
    "enabled": True,
    "pump_running": False,
    "pump_started_at": None,
    "total_runtime_s": 0,
    "sim_flow_lpm": 60.0  # 60 Liters per minute nominal pump flow
}

# ── Station Web GUI (http://localhost:8080) ───────────────────────────────────

STATION_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SX1302 Gateway Station // SCADA Deck</title>
  <style>
    :root {
      --bg: #070b14;
      --card-bg: rgba(13, 20, 36, 0.85);
      --border: rgba(0, 240, 255, 0.2);
      --cyan: #00f0ff;
      --green: #00ff88;
      --amber: #ffaa00;
      --red: #ff3366;
      --text: #e2e8f0;
      --muted: #64748b;
      --font-mono: 'Consolas', 'Fira Code', monospace;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      padding: 24px;
      min-height: 100vh;
      background-image: radial-gradient(circle at 10% 20%, rgba(0, 240, 255, 0.05) 0%, transparent 40%),
                        radial-gradient(circle at 90% 80%, rgba(0, 255, 136, 0.04) 0%, transparent 40%);
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px 24px;
      margin-bottom: 20px;
      box-shadow: 0 8px 32px rgba(0,0,0,0.5);
    }
    .brand h1 {
      font-family: var(--font-mono);
      font-size: 1.25rem;
      letter-spacing: 0.08em;
      color: #fff;
    }
    .brand p { font-size: 0.75rem; color: var(--muted); margin-top: 3px; }
    .badges { display: flex; gap: 10px; align-items: center; }
    .badge {
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.1);
      padding: 6px 12px;
      border-radius: 6px;
      font-size: 0.75rem;
      font-family: var(--font-mono);
      color: var(--cyan);
    }
    .pulse {
      display: inline-block; width: 8px; height: 8px; border-radius: 50%;
      background: var(--green); box-shadow: 0 0 8px var(--green); margin-right: 6px;
      animation: pulse 1.2s infinite alternate;
    }
    @keyframes pulse { from { opacity: 0.4; } to { opacity: 1; } }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
      gap: 20px;
      margin-bottom: 20px;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 20px;
      box-shadow: 0 8px 24px rgba(0,0,0,0.4);
    }
    .card-title {
      font-family: var(--font-mono);
      font-size: 0.85rem;
      color: var(--cyan);
      margin-bottom: 16px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      border-bottom: 1px solid rgba(255, 255, 255, 0.08);
      padding-bottom: 8px;
    }
    .tank-box {
      display: flex;
      gap: 20px;
      align-items: center;
      margin-bottom: 16px;
    }
    .tank-vessel {
      width: 100px;
      height: 150px;
      border: 3px solid #334155;
      border-radius: 8px;
      background: rgba(0, 0, 0, 0.5);
      position: relative;
      overflow: hidden;
      box-shadow: inset 0 0 20px rgba(0,0,0,0.8);
    }
    .water-fill {
      position: absolute;
      bottom: 0; left: 0; right: 0;
      background: linear-gradient(180deg, rgba(0, 240, 255, 0.7) 0%, rgba(0, 100, 255, 0.85) 100%);
      transition: height 0.6s cubic-bezier(0.4, 0, 0.2, 1);
    }
    .tank-details { flex: 1; display: flex; flex-direction: column; gap: 8px; }
    .row { display: flex; justify-content: space-between; font-size: 0.78rem; font-family: var(--font-mono); }
    .row-k { color: var(--muted); }
    .row-v { font-weight: 700; }
    .tag {
      padding: 2px 7px;
      border-radius: 4px;
      font-size: 0.68rem;
      font-weight: 700;
      font-family: var(--font-mono);
    }
    .tag-auto { background: rgba(0, 255, 136, 0.2); color: var(--green); border: 1px solid rgba(0, 255, 136, 0.4); }
    .tag-man { background: rgba(255, 170, 0, 0.2); color: var(--amber); border: 1px solid rgba(255, 170, 0, 0.4); }
    .tag-on { background: rgba(0, 255, 136, 0.2); color: var(--green); }
    .tag-off { background: rgba(255, 255, 255, 0.05); color: var(--muted); }
    .actions { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 14px; }
    button {
      background: rgba(0, 240, 255, 0.08);
      border: 1px solid var(--border);
      color: #fff;
      padding: 7px 12px;
      border-radius: 6px;
      font-family: var(--font-mono);
      font-size: 0.72rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s;
    }
    button:hover { background: rgba(0, 240, 255, 0.2); border-color: var(--cyan); transform: translateY(-1px); }
    .packet-box {
      max-height: 250px;
      overflow-y: auto;
      border: 1px solid rgba(255, 255, 255, 0.08);
      border-radius: 6px;
    }
    table { width: 100%; border-collapse: collapse; font-family: var(--font-mono); font-size: 0.72rem; }
    th { background: rgba(255, 255, 255, 0.05); padding: 8px 10px; text-align: left; color: var(--muted); position: sticky; top: 0; }
    td { padding: 6px 10px; border-bottom: 1px solid rgba(255, 255, 255, 0.04); }
    tr:hover { background: rgba(255, 255, 255, 0.03); }
    .toast {
      position: fixed; bottom: 20px; right: 20px;
      background: rgba(13, 20, 36, 0.95); border-left: 4px solid var(--cyan);
      padding: 10px 16px; border-radius: 6px; font-family: var(--font-mono); font-size: 0.75rem;
      box-shadow: 0 4px 16px rgba(0,0,0,0.5); display: none;
    }
  </style>
</head>
<body>
  <div class="header">
    <div class="brand">
      <h1>SX1302 GATEWAY STATION <span style="font-size:0.75rem;color:var(--cyan)">// SCADA DECK</span></h1>
      <p class="subtitle">UDP Telemetry Hub (:5555 / :5556) & Autonomous Field Simulator</p>
    </div>
    <div class="badges">
      <div class="badge"><span class="pulse"></span><span id="gwIp">Gateway: Waiting...</span></div>
      <div class="badge">Packets: <span id="pktCount">0</span></div>
      <button onclick="refreshData()" title="Refresh Telemetry">🔄</button>
    </div>
  </div>

  <div class="grid">
    <!-- Card 1: Tank Node (0x02) Live Telemetry -->
    <div class="card">
      <div class="card-title">
        <span>💧 TANK NODE TELEMETRY (0x02)</span>
        <span id="tankLastSeen" style="font-size:0.7rem;color:var(--muted)">No Data</span>
      </div>
      <div class="tank-box">
        <div class="tank-vessel">
          <div class="water-fill" id="waterFill" style="height: 0%;"></div>
        </div>
        <div class="tank-details">
          <div class="row"><span class="row-k">Water Level:</span><span class="row-v" id="tankLevel">--</span></div>
          <div class="row"><span class="row-k">System Mode:</span><span id="tankMode" class="tag tag-man">MANUAL</span></div>
          <div class="row"><span class="row-k">Outflow P1:</span><span id="tankP1" class="tag tag-off">OFF</span></div>
          <div class="row"><span class="row-k">Outflow P2:</span><span id="tankP2" class="tag tag-off">OFF</span></div>
          <div class="row"><span class="row-k">Outflow P3:</span><span id="tankP3" class="tag tag-off">OFF</span></div>
          <div class="row"><span class="row-k">Pond Demand:</span><span id="tankPond" class="tag tag-off">IDLE</span></div>
          <div class="row"><span class="row-k">Environment:</span><span class="row-v" id="tankEnv">-- °C / -- %</span></div>
        </div>
      </div>
      <div class="actions">
        <button onclick="sendCmd('/tank status')">Status</button>
        <button onclick="sendCmd('/tank mode toggle')">Toggle Mode</button>
        <button onclick="sendCmd('/tank p1 toggle')">Toggle P1</button>
        <button onclick="sendCmd('/tank p2 toggle')">Toggle P2</button>
        <button onclick="sendCmd('/tank p3 toggle')">Toggle P3</button>
        <button onclick="sendCmd('/tank pond toggle')">Toggle Pond</button>
      </div>
    </div>

    <!-- Card 2: Autonomous Pond Simulator (0x03) -->
    <div class="card">
      <div class="card-title">
        <span>🌊 POND NODE SIMULATOR (0x03)</span>
        <span id="pondSimState" class="tag tag-auto">SIMULATOR ACTIVE</span>
      </div>
      <div class="tank-details" style="gap:12px;">
        <div class="row"><span class="row-k">Pump State:</span><span id="pondPumpState" class="tag tag-off">IDLE (STOPPED)</span></div>
        <div class="row"><span class="row-k">Pumping Rate:</span><span class="row-v" id="pondFlow">60.0 L/min</span></div>
        <div class="row"><span class="row-k">Current Duration:</span><span class="row-v" id="pondCurDur">0.0s</span></div>
        <div class="row"><span class="row-k">Total Operating:</span><span class="row-v" id="pondTotDur">0.0s</span></div>
        <div class="row"><span class="row-k">Volume Delivered:</span><span class="row-v" id="pondDelivered" style="color:var(--cyan);font-size:0.9rem;">0.0 Liters</span></div>
      </div>
      <div class="actions" style="margin-top:20px;">
        <button onclick="sendCmd('/pond sim on')">Enable Sim</button>
        <button onclick="sendCmd('/pond sim off')">Disable Sim</button>
        <button onclick="sendCmd('/pond status')">Sim Status</button>
      </div>
    </div>
  </div>

  <!-- Card 3: Live Decoded Packet Stream -->
  <div class="card">
    <div class="card-title">
      <span>📡 LIVE DECODED LORA PACKETS</span>
      <span style="font-size:0.7rem;color:var(--muted)">Auto-updating (1s)</span>
    </div>
    <div class="packet-box">
      <table>
        <thead>
          <tr>
            <th>Time</th>
            <th>Type</th>
            <th>Sender → Target</th>
            <th>Summary</th>
            <th>Raw Hex</th>
          </tr>
        </thead>
        <tbody id="pktBody">
          <tr><td colspan="5" style="text-align:center;color:var(--muted)">Waiting for incoming packets...</td></tr>
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
        showToast(d.msg || 'Command sent');
        refreshData();
      })
      .catch(e => showToast('Error sending command'));
    }

    function refreshData() {
      fetch('/api/data')
        .then(r => r.json())
        .then(d => {
          if (d.gateway_ip) document.getElementById('gwIp').innerText = 'Gateway: ' + d.gateway_ip;
          if (d.pkt_counter !== undefined) document.getElementById('pktCount').innerText = '#' + d.pkt_counter;

          // Tank Node
          if (d.tank && d.tank.last_seen) {
            const t = d.tank;
            const wl = t.water_level || 0;
            const heights = [0, 33, 66, 100];
            const labels = ['0/3 (EMPTY)', '1/3 (LOW 33%)', '2/3 (MID 66%)', '3/3 (FULL 100%)'];
            document.getElementById('waterFill').style.height = (heights[wl] || 0) + '%';
            document.getElementById('tankLevel').innerText = labels[wl] || (wl + '/3');
            
            const isAuto = Boolean(t.auto_mode);
            const mBadge = document.getElementById('tankMode');
            mBadge.innerText = isAuto ? 'AUTOMATIC' : 'MANUAL';
            mBadge.className = isAuto ? 'tag tag-auto' : 'tag tag-man';

            ['p1', 'p2', 'p3'].forEach(p => {
              const el = document.getElementById('tank' + p.toUpperCase());
              const on = Boolean(t[p]);
              el.innerText = on ? 'ON' : 'OFF';
              el.className = on ? 'tag tag-on' : 'tag tag-off';
            });

            const pnd = document.getElementById('tankPond');
            const pndOn = Boolean(t.pond_demand);
            pnd.innerText = pndOn ? 'DEMAND ACTIVE' : 'IDLE';
            pnd.className = pndOn ? 'tag tag-auto' : 'tag tag-off';

            if (t.temp_c !== undefined && t.humidity !== undefined) {
              document.getElementById('tankEnv').innerText = t.temp_c.toFixed(1) + ' °C / ' + t.humidity + ' %';
            }
            const ago = Math.round((Date.now() / 1000) - t.last_seen);
            document.getElementById('tankLastSeen').innerText = 'seen ' + ago + 's ago';
          }

          // Pond Sim
          if (d.pond_sim) {
            const p = d.pond_sim;
            const simEl = document.getElementById('pondSimState');
            simEl.innerText = p.enabled ? 'SIMULATOR ACTIVE' : 'SIMULATOR DISABLED';
            simEl.className = p.enabled ? 'tag tag-auto' : 'tag tag-off';

            const pumpEl = document.getElementById('pondPumpState');
            pumpEl.innerText = p.pump_running ? 'RUNNING (PUMPING)' : 'IDLE (STOPPED)';
            pumpEl.className = p.pump_running ? 'tag tag-auto' : 'tag tag-off';

            document.getElementById('pondFlow').innerText = p.flow_lpm + ' L/min';
            document.getElementById('pondCurDur').innerText = p.cur_dur + 's';
            document.getElementById('pondTotDur').innerText = p.tot_dur + 's';
            document.getElementById('pondDelivered').innerText = p.tot_liters + ' Liters';
          }

          // Packets
          if (d.packets && d.packets.length > 0) {
            const tbody = document.getElementById('pktBody');
            tbody.innerHTML = d.packets.slice().reverse().map(p => `
              <tr>
                <td>${p.time}</td>
                <td><span class="tag ${p.type === 'MSG_STATUS' ? 'tag-auto' : 'tag-cmd'}">${p.type}</span></td>
                <td>${p.sender} → ${p.target}</td>
                <td>${p.summary}</td>
                <td style="color:var(--muted)">${p.hex}</td>
              </tr>
            `).join('');
          }
        })
        .catch(e => console.warn('Fetch error:', e));
    }

    setInterval(refreshData, 1200);
    refreshData();
  </script>
</body>
</html>
""".encode("utf-8")

class StationWebHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress console log spam from web requests

    def do_GET(self):
        if self.path == "/api/data":
            now = time.time()
            running = pond_sim["pump_running"]
            cur_dur = (now - pond_sim["pump_started_at"]) if (running and pond_sim["pump_started_at"]) else 0
            tot_dur = pond_sim["total_runtime_s"] + cur_dur
            tot_liters = (tot_dur / 60.0) * pond_sim["sim_flow_lpm"]

            data = {
                "gateway_ip": gateway_ip or "Waiting...",
                "pkt_counter": pkt_counter,
                "tank": last_tank_status,
                "pond_sim": {
                    "enabled": pond_sim["enabled"],
                    "pump_running": running,
                    "cur_dur": round(cur_dur, 1),
                    "tot_dur": round(tot_dur, 1),
                    "tot_liters": round(tot_liters, 1),
                    "flow_lpm": pond_sim["sim_flow_lpm"]
                },
                "packets": packet_history[-30:]
            }
            body = json.dumps(data).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(STATION_HTML)))
        self.end_headers()
        self.wfile.write(STATION_HTML)

    def do_POST(self):
        if self.path == "/api/cmd":
            try:
                content_len = int(self.headers.get("Content-Length", 0))
                post_body = self.rfile.read(content_len)
                data = json.loads(post_body.decode("utf-8"))
                cmd = data.get("cmd", "").strip()
                if cmd:
                    handle(cmd)
                    resp = json.dumps({"status": "ok", "msg": f"Executed: {cmd}"}).encode("utf-8")
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


# ── Thread-Safe Terminal Printing ──────────────────────────────────────────────

def safe_print(msg: str):
    """
    Prints logs safely. 
    patch_stdout will automatically intercept this call, lift your active input line,
    print this message, and restore your input line seamlessly underneath.
    """
    print(msg)

# ── Packet parsers ────────────────────────────────────────────────────────────

def decode_device_payload(payload_bytes: bytes):
    if len(payload_bytes) < 5:
        return None
        
    # Extract & Verify Magic Bytes (6B 5A)
    magic = payload_bytes[0:2]
    if magic != b'\x6B\x5A':
        return None

    # Parse Frame Header
    target_id   = payload_bytes[2]
    sender_id   = payload_bytes[3]
    msg_type    = payload_bytes[4]

    # Initialize the tracking array
    result = [
        f"  │   [Device Frame] MsgType: 0x{msg_type:02X} | Sender: 0x{sender_id:02X} → Target: 0x{target_id:02X}"
    ]
        
    if len(payload_bytes) == 11:  # MSG_STATUS (TX from device)
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

        # Delta detection against previous status
        deltas = []
        if prev_tank_status:
            if prev_tank_status.get("water_level") != water:
                wl_names = ["0/3 (EMPTY)", "1/3 (LOW)", "2/3 (MID)", "3/3 (FULL)"]
                old_w = wl_names[min(prev_tank_status.get("water_level", 0), 3)]
                new_w = wl_names[min(water, 3)]
                deltas.append(f"Water: {old_w} -> {new_w}")
            if prev_tank_status.get("auto_mode") != auto_mode:
                deltas.append(f"Mode: {'AUTO' if prev_tank_status.get('auto_mode') else 'MANUAL'} -> {'AUTO' if auto_mode else 'MANUAL'}")
            if prev_tank_status.get("p1") != p1:
                deltas.append(f"P1: {'ON' if prev_tank_status.get('p1') else 'OFF'} -> {'ON' if p1 else 'OFF'}")
            if prev_tank_status.get("p2") != p2:
                deltas.append(f"P2: {'ON' if prev_tank_status.get('p2') else 'OFF'} -> {'ON' if p2 else 'OFF'}")
            if prev_tank_status.get("p3") != p3:
                deltas.append(f"P3: {'ON' if prev_tank_status.get('p3') else 'OFF'} -> {'ON' if p3 else 'OFF'}")
            if prev_tank_status.get("pond_demand") != pond_flag:
                deltas.append(f"PondDemand: {'ON' if prev_tank_status.get('pond_demand') else 'OFF'} -> {'ON' if pond_flag else 'OFF'}")

        # Cache live status for console and GUI
        last_tank_status.update({
            "water_level": water,
            "auto_mode": auto_mode,
            "p1": p1, "p2": p2, "p3": p3,
            "pond_demand": pond_flag,
            "temp_c": temp_raw / 10.0,
            "humidity": humidity,
            "last_seen": time.time()
        })
        prev_tank_status.update(last_tank_status)

        # Record to packet history for Station Web GUI
        packet_history.append({
            "time": time.strftime("%H:%M:%S"),
            "type": "MSG_STATUS",
            "sender": f"0x{sender_id:02X}",
            "target": f"0x{target_id:02X}",
            "summary": f"Lvl={water}/3 Auto={int(auto_mode)} Relays=[{int(p1)},{int(p2)},{int(p3)}] PondReq={int(pond_flag)} {temp_raw/10.0:.1f}°C",
            "hex": payload_bytes.hex().upper()
        })
        if len(packet_history) > 100:
            packet_history.pop(0)

        result.extend([
            f"  │   Type       = MSG_STATUS (11 bytes)",
            f"  │   Flags      = 0x{flags:02X} → Auto={auto_mode} P1={p1} P2={p2} P3={p3} POND_PUMP={pond_flag}",
            f"  │   Water      = {water} (level 0-3)",
            f"  │   Temperature= {temp_raw/10.0:.1f} °C",
            f"  │   Humidity   = {humidity} % RH",
            f"  │   Reserved   = 0x{reserved:02X}"
        ])
        if deltas:
            result.append(f"  │   🔥 [DELTA DETECTED] " + " | ".join(deltas))

    elif len(payload_bytes) == 8:  # MSG_CMD (RX or node-to-node command)
        target = payload_bytes[5]
        action = payload_bytes[6]
        value  = payload_bytes[7]

        target_str = {
            0x01: "MODE",
            0x02: "P1",
            0x04: "P2",
            0x08: "P3",
            0x10: "POND_PUMP",
        }.get(target, f"0x{target:02X}")

        action_str = {0x00: "TOGGLE", 0x01: "SET"}.get(action, f"0x{action:02X}")
        value_str  = {0x00: "OFF/MANUAL", 0x01: "ON/AUTO"}.get(value, f"0x{value:02X}")

        # Record command packet into history
        packet_history.append({
            "time": time.strftime("%H:%M:%S"),
            "type": "MSG_CMD",
            "sender": f"0x{sender_id:02X}",
            "target": f"0x{target_id:02X}",
            "summary": f"Target={target_str} Act={action_str} Val={value_str}",
            "hex": payload_bytes.hex().upper()
        })
        if len(packet_history) > 100:
            packet_history.pop(0)

        result.extend([
            f"  │   Type       = MSG_CMD (8 bytes)",
            f"  │   Target     = {target_str}",
            f"  │   Action     = {action_str}",
            f"  │   Value      = {value_str}"
        ])

        # Autonomous Pond Node Simulator (NODE_POND = 0x03)
        if target_id == 0x03 and pond_sim["enabled"] and (target & 0x10):
            turn_on = bool(value == 1) if action == 0x01 else not pond_sim["pump_running"]
            now = time.time()
            if turn_on != pond_sim["pump_running"]:
                pond_sim["pump_running"] = turn_on
                if turn_on:
                    pond_sim["pump_started_at"] = now
                    result.append(f"  │   🌊 [POND SIMULATOR] Tank demanded refill -> POND PUMP ENERGIZED (Rate: 60 L/min)")
                else:
                    if pond_sim["pump_started_at"]:
                        dur = now - pond_sim["pump_started_at"]
                        pond_sim["total_runtime_s"] += dur
                        delivered = (dur / 60.0) * pond_sim["sim_flow_lpm"]
                        result.append(f"  │   🛑 [POND SIMULATOR] Tank stopped refill -> Pump stopped. Delivered ~{delivered:.1f} L in {dur:.1f}s")
                    pond_sim["pump_started_at"] = None
    else:
        result.append(f"  │   Unknown Payload Length ({len(payload_bytes)} bytes)")

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
                device_decode = decode_device_payload(payload_bytes)
               
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

    (pkt_type, eui, uptime, rx_count, tx_count,
     crc_err, tx_err, q_drop,
     q_depth, sync_word, rf0_freq, rf1_freq) = struct.unpack_from(HB_FMT, data)

    last_hb.update(dict(uptime=uptime, rx=rx_count, tx=tx_count,
                        crc_err=crc_err, tx_err=tx_err,
                        q_drop=q_drop, q_depth=q_depth,
                        sync_word=sync_word,
                        rf0_freq=rf0_freq, rf1_freq=rf1_freq))

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
    return f"  ★  EUI={eui:016X}  HAL={version}"

# ── TX JSON Builders ──────────────────────────────────────────────────────────

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


def build_custom(payload: bytes, freq=868_100_000, power=14, sf=7,
                 bw=125, cr="4/5", invert=True, no_crc=False, 
                 imme=True, rfch=0, modu="LORA") -> bytes:
    freq_mhz = freq / 1e6
    data_b64 = base64.b64encode(payload).decode('utf-8')
    
    if modu == "LORA":
        datr_str = f"SF{sf}BW{bw}"
    else:
        datr_str = str(sf)  

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


def send(data: bytes):
    if gateway_ip is None:
        print("\n  [!] No gateway seen yet — waiting for boot packet.")
        return False
    sock.sendto(data, (gateway_ip, SEND_PORT_DOWN))
    return True

def send_tank_cmd(target_byte: int, action_byte: int, val_byte: int):
    """
    Sends an 8-byte MSG_CMD frame over LoRa to the Tank Node (NODE_TANK = 0x02)
    via the SX1302 gateway.
    """
    pkt = bytearray([0x6B, 0x5A, 0x02, 0x01, 0x02, target_byte, action_byte, val_byte])
    tx_json = build_custom(bytes(pkt), freq=868_100_000, power=14, sf=9, bw=125, cr="4/5", invert=False)
    if send(tx_json):
        t_name = {0x01: "MODE", 0x02: "P1", 0x04: "P2", 0x08: "P3", 0x10: "POND"}.get(target_byte, hex(target_byte))
        a_name = "TOGGLE" if action_byte == 0 else ("SET ON" if val_byte == 1 else "SET OFF")
        print(f"  [TX -> TANK NODE] 8-byte LoRa command: {t_name} -> {a_name}")

# ── Listener thread ───────────────────────────────────────────────────────────

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
            log_lines.append(f"  Gateway locked at {gateway_ip} — ready to TX.")

        else:
            log_lines.append(f"[{ts}] #{pkt_counter} UNKNOWN type=0x{pkt_type:02X} len={len(data)}  {data[:32].hex()}")

        # Emit background messages safely using prompt_toolkit integration
        safe_print("\n".join(log_lines))

# ── Command handler ───────────────────────────────────────────────────────────

def print_help():
    print("""
  Station Web Dashboard:
    /gui or /web              Open Station Web GUI in your browser (http://localhost:8080)

  Gateway Commands:
    <text>                    Send text with gateway defaults (SF7 / 868.1 MHz)
    /tx <text>                Same (explicit)
    /sf <5-12> <text>         Send on a specific spreading factor
    /freq <MHz> <text>        Send on a specific frequency  e.g. /freq 868.3 hi
    /power <dBm> <text>       Set TX power (must be in TX gain LUT)
    /custom [opts] [text]     Full control. Options (space-separated key=value):
                                freq=868.1  sf=7  bw=125  power=14
                                cr=4/5  invert=1  modu=LORA  rfch=0
                                hex=0302330c01100100 (For raw binary hex payloads)
    /hex <hexstring>          Send raw bytes wrapped in JSON e.g. /hex deadbeef01
    /status                   Show last heartbeat stats
    /help                     Show this help
    Ctrl+C                    Exit

  Tank Node & Pond Station Commands:
    /tank status              Show latest cached telemetry with visual ASCII gauge
    /tank mode auto|man       Command Tank Node operational mode
    /tank p1 on|off|toggle    Command Outflow Pump 1
    /tank p2 on|off|toggle    Command Outflow Pump 2
    /tank p3 on|off|toggle    Command Outflow Pump 3
    /tank pond on|off|toggle  Command Remote Pond Pump
    /pond sim on|off          Enable/disable autonomous Pond Node Simulator
    /pond status              Show simulated Pond Node pump state and volume
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
        print(f"  🌐 Launching Station Web Dashboard in browser: {url}")
        try:
            webbrowser.open(url)
        except Exception as e:
            print(f"  [!] Failed to open browser automatically: {e}")
            print(f"  👉 Navigate manually to: {url}")
        return

    if line == "/tank status":
        if not last_tank_status:
            print("  [!] No telemetry received from Tank Node yet.")
            return
        ago = int(time.time() - last_tank_status.get("last_seen", time.time()))
        wl = last_tank_status.get("water_level", 0)
        mode = "AUTO" if last_tank_status.get("auto_mode") else "MANUAL"
        p1 = "ON" if last_tank_status.get("p1") else "OFF"
        p2 = "ON" if last_tank_status.get("p2") else "OFF"
        p3 = "ON" if last_tank_status.get("p3") else "OFF"
        pond = "DEMAND ACTIVE" if last_tank_status.get("pond_demand") else "IDLE"
        temp = last_tank_status.get("temp_c", 0.0)
        hum = last_tank_status.get("humidity", 0)
        
        bars = [
            "[░░░░░░░░░░] EMPTY (0%)",
            "[███░░░░░░░] LOW   (33%)",
            "[███████░░░] MID   (66%)",
            "[██████████] FULL  (100%)"
        ]
        gauge = bars[min(wl, 3)]

        print(f"  ┌─ Tank Node Live Telemetry (seen {ago}s ago) ─────────")
        print(f"  │ Water Level : {gauge}")
        print(f"  │ System Mode : {mode}")
        print(f"  │ Outflow P1  : {p1}")
        print(f"  │ Outflow P2  : {p2}")
        print(f"  │ Outflow P3  : {p3}")
        print(f"  │ Pond Demand : {pond}")
        print(f"  │ Environment : {temp:.1f} °C, {hum}% RH")
        print(f"  └─────────────────────────────────────────────────────")
        return

    if line.startswith("/tank "):
        parts = line[6:].strip().split()
        if len(parts) >= 2:
            sub = parts[0].lower()
            val = parts[1].lower()
            if sub == "mode":
                act = 0x00 if val == "toggle" else 0x01
                v = 0x01 if val in ("auto", "1", "on") else 0x00
                send_tank_cmd(0x01, act, v)
                return
            elif sub in ("p1", "p2", "p3"):
                p_mask = {"p1": 0x02, "p2": 0x04, "p3": 0x08}[sub]
                act = 0x00 if val == "toggle" else 0x01
                v = 0x01 if val in ("on", "1") else 0x00
                send_tank_cmd(p_mask, act, v)
                return
            elif sub == "pond":
                act = 0x00 if val == "toggle" else 0x01
                v = 0x01 if val in ("on", "1") else 0x00
                send_tank_cmd(0x10, act, v)
                return
        print("  Usage: /tank <mode|p1|p2|p3|pond> <on|off|toggle|auto|man>")
        return

    if line == "/pond status":
        running = pond_sim["pump_running"]
        st = "RUNNING (PUMPING)" if running else "IDLE (STOPPED)"
        now = time.time()
        cur_dur = (now - pond_sim["pump_started_at"]) if (running and pond_sim["pump_started_at"]) else 0
        tot_dur = pond_sim["total_runtime_s"] + cur_dur
        tot_liters = (tot_dur / 60.0) * pond_sim["sim_flow_lpm"]
        print(f"  ┌─ Pond Node (0x03) Simulator Status ─────────────────")
        print(f"  │ Simulator Active : {'YES' if pond_sim['enabled'] else 'DISABLED'}")
        print(f"  │ Pump Relay State : {st}")
        print(f"  │ Current Run Time : {cur_dur:.1f}s")
        print(f"  │ Total Operating  : {tot_dur:.1f}s")
        print(f"  │ Total Delivered  : ~{tot_liters:.1f} Liters ({pond_sim['sim_flow_lpm']} L/min)")
        print(f"  └─────────────────────────────────────────────────────")
        return

    if line.startswith("/pond sim "):
        arg = line[10:].strip().lower()
        if arg in ("on", "1", "enable"):
            pond_sim["enabled"] = True
            print("  [Pond Sim] Autonomous Pond Node simulator ENABLED.")
        elif arg in ("off", "0", "disable"):
            pond_sim["enabled"] = False
            pond_sim["pump_running"] = False
            print("  [Pond Sim] Autonomous Pond Node simulator DISABLED.")
        else:
            print("  Usage: /pond sim <on|off>")
        return

    if line == "/help":
        print_help()
        return

    if line == "/status":
        if not last_hb:
            print("  No heartbeat received yet.")
            return
        h, m, s = last_hb['uptime']//3600, (last_hb['uptime']%3600)//60, last_hb['uptime']%60
        sw = last_hb['sync_word']
        sync_str = "public(0x34)" if sw == 0x34 else f"private(0x{sw:02X})"
        print(f"  uptime={h:02d}:{m:02d}:{s:02d}  "
              f"rx={last_hb['rx']}  tx={last_hb['tx']}  "
              f"crc_err={last_hb['crc_err']}  tx_err={last_hb['tx_err']}  "
              f"q={last_hb['q_depth']}/{TX_QUEUE_SIZE}  "
              f"q_drop={last_hb['q_drop']}  "
              f"sync={sync_str}  "
              f"rf0={last_hb['rf0_freq']/1e6:.3f}MHz  rf1={last_hb['rf1_freq']/1e6:.3f}MHz")
        return

    if line.startswith("/hex "):
        hexstr = line[5:].replace(" ", "")
        try:
            payload = bytes.fromhex(hexstr)
        except ValueError as e:
            print(f"  [!] Bad hex: {e}")
            return
        pkt = build_default(payload)
        if send(pkt):
            print(f"  [TX] raw hex JSON  {len(payload)} bytes → {hexstr}")
        return

    if line.startswith("/sf "):
        parts = line.split(" ", 2)
        if len(parts) < 3:
            print("  Usage: /sf <5-12> <text>")
            return
        try:
            sf = int(parts[1])
            assert 5 <= sf <= 12
        except (ValueError, AssertionError):
            print("  [!] SF must be 5–12")
            return
        payload = parts[2].encode()
        if send(build_custom(payload, sf=sf)):
            print(f"  [TX] SF{sf} JSON  {len(payload)} bytes → \"{parts[2]}\"")
        return

    if line.startswith("/freq "):
        parts = line.split(" ", 2)
        if len(parts) < 3:
            print("  Usage: /freq <MHz> <text>")
            return
        try:
            freq = int(float(parts[1]) * 1e6)
        except ValueError:
            print("  [!] Bad frequency")
            return
        payload = parts[2].encode()
        if send(build_custom(payload, freq=freq)):
            print(f"  [TX] {freq/1e6:.4f} MHz JSON  {len(payload)} bytes → \"{parts[2]}\"")
        return

    if line.startswith("/power "):
        parts = line.split(" ", 2)
        if len(parts) < 3:
            print("  Usage: /power <dBm> <text>")
            return
        try:
            power = int(parts[1])
        except ValueError:
            print("  [!] Bad power value")
            return
        payload = parts[2].encode()
        if send(build_custom(payload, power=power)):
            print(f"  [TX] {power} dBm JSON  {len(payload)} bytes → \"{parts[2]}\"")
        return

    if line.startswith("/custom "):
        opts = dict(freq=868_100_000, sf=7, bw=125, power=14,
                    cr="4/5", invert=1, rfch=0, modu="LORA")
        words = line[8:].split()
        text_words = []
        hex_payload = None
        
        for w in words:
            if "=" in w:
                k, v = w.split("=", 1)
                if k == "freq":
                    f_val = float(v)
                    opts["freq"] = int(f_val * 1e6) if f_val < 3000 else int(f_val)
                elif k == "sf":       opts["sf"]      = int(v)
                elif k == "bw":       opts["bw"]      = int(v)
                elif k == "power":    opts["power"]   = int(v)
                elif k == "cr":       opts["cr"]      = v
                elif k == "invert":   opts["invert"]  = int(v)
                elif k == "rfch":     opts["rfch"]    = int(v)
                elif k == "modu":     opts["modu"]    = v.upper()
                elif k == "hex":
                    try:
                        hex_payload = bytes.fromhex(v.replace(" ", ""))
                    except ValueError:
                        print("  [!] Invalid hex string passed to hex= parameter.")
                        return
            else:
                text_words.append(w)
                
        if hex_payload is not None:
            payload = hex_payload
            display_str = f"hex: {payload.hex()}"
        else:
            text = " ".join(text_words)
            if not text:
                print("  [!] No payload text or hex= data provided.")
                return
            payload = text.encode()
            display_str = f"\"{text}\""
            
        pkt = build_custom(payload, freq=opts["freq"], power=opts["power"],
                           sf=opts["sf"], bw=opts["bw"], cr=opts["cr"],
                           invert=bool(opts["invert"]), rfch=opts["rfch"], modu=opts["modu"])
        if send(pkt):
            print(f"  [TX] custom JSON | SF{opts['sf']} | {opts['freq']/1e6:.4f} MHz | "
                  f"{opts['power']} dBm | invert={opts['invert']} | {len(payload)} bytes → {display_str}")
        return

    if line.startswith("/tx "):
        line = line[4:]
    payload = line.encode()
    if len(payload) > 255:
        print(f"  [!] Payload too long ({len(payload)} bytes, max 255)")
        return
    if send(build_default(payload)):
        print(f"  [TX] default JSON  {len(payload)} bytes → \"{line}\"")

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    sock.bind(("0.0.0.0", LISTEN_PORT_UP))

    t = threading.Thread(target=listener_thread, daemon=True)
    t.start()

    p = start_station_web_server(STATION_WEB_PORT)

    print("=" * 66)
    print("      SX1302 GATEWAY STATION & SCADA TELEMETRY CONSOLE")
    print("=" * 66)
    print(f"  LoRa Socket  : listening on :{LISTEN_PORT_UP}  →  TX on :{SEND_PORT_DOWN}")
    if p:
        print(f"  Station GUI  : http://localhost:{p}  (type /gui to launch browser)")
    else:
        print(f"  Station GUI  : [!] Failed to bind ports {STATION_WEB_PORT}-{STATION_WEB_PORT+9}")
    print(f"  Console Help : type /help for command reference")
    print("=" * 66)
    print("Waiting for gateway boot packet...\n")

    # Start an interactive prompt session
    session = PromptSession()

    # The patch_stdout context manager hooks into sys.stdout automatically
    with patch_stdout():
        while True:
            try:
                user_line = session.prompt("> ")
                handle(user_line)
            except KeyboardInterrupt:
                print("\nExiting.")
                break
            except EOFError:
                break

if __name__ == "__main__":
    main()