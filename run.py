import socket
import struct
import threading
import time
import json
import base64
import sys

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

        result.extend([
            f"  │   Type       = MSG_STATUS (11 bytes)",
            f"  │   Flags      = 0x{flags:02X} → Auto={auto_mode} P1={p1} P2={p2} P3={p3} POND_PUMP={bool(flags & 0x10)}",
            f"  │   Water      = {water} (level 0-3)",
            f"  │   Temperature= {temp_raw/10.0:.1f} °C",
            f"  │   Humidity   = {humidity} % RH",
            f"  │   Reserved   = 0x{reserved:02X}"
        ])

    elif len(payload_bytes) == 8:  # MSG_CMD (RX to device)
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
  Commands:
    <text>                    Send text with gateway defaults (SF7 / 868.1 MHz)
    /tx <text>                Same (explicit)
    /sf <5-12> <text>         Send on a specific spreading factor
    /freq <MHz> <text>        Send on a specific frequency  e.g. /freq 868.3 hi
    /power <dBm> <text>       Set TX power (must be in TX gain LUT)
    /custom [opts] [text]     Full control. Options (space-separated key=value):
                                freq=868.1  sf=7  bw=125  power=14
                                cr=4/5  invert=1  modu=LORA  rfch=0
                                hex=0302330c01100100 (For raw binary hex payloads)
                              Example:  /custom sf=12 power=27 invert=1 hex=01020304
    /hex <hexstring>          Send raw bytes wrapped in JSON e.g. /hex deadbeef01
    /status                   Show last heartbeat stats
    /help                     Show this help
    Ctrl+C                    Exit
""")


def handle(line: str):
    line = line.strip()
    if not line:
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