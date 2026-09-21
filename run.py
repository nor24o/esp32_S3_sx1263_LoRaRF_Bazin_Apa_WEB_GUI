import socket
import struct
import threading
import time
import json
import base64
import sys

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
LISTEN_PORT_UP = 5555   # gateway -> PC
SEND_PORT_DOWN = 5556   # PC -> gateway
TX_QUEUE_SIZE  = 32     # must match raw_lora_app.c
# ---------------------

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

        # Cache live status for the console
        last_tank_status.update({
            "water_level": water,
            "auto_mode": auto_mode,
            "p1": p1, "p2": p2, "p3": p3,
            "pond_demand": pond_flag,
            "temp_c": temp_raw / 10.0,
            "humidity": humidity,
            "last_seen": time.time()
        })

        result.extend([
            f"  │   Type       = MSG_STATUS (11 bytes)",
            f"  │   Flags      = 0x{flags:02X} → Auto={auto_mode} P1={p1} P2={p2} P3={p3} POND_PUMP={pond_flag}",
            f"  │   Water      = {water} (level 0-3)",
            f"  │   Temperature= {temp_raw/10.0:.1f} °C",
            f"  │   Humidity   = {humidity} % RH",
            f"  │   Reserved   = 0x{reserved:02X}"
        ])

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
    /tank status              Show latest cached telemetry from Tank Node
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
        wl_str = ["EMPTY (0%)", "LOW (33%)", "MID (66%)", "FULL (100%)"][min(wl, 3)]
        print(f"  ┌─ Tank Node Live Telemetry (seen {ago}s ago) ─────────")
        print(f"  │ Water Level : {wl}/3 ({wl_str})")
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

    print(f"SX1302 Debug Console")
    print(f"  listening on :{LISTEN_PORT_UP}  →  sending to :{SEND_PORT_DOWN}")
    print(f"  type /help for commands\n")
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