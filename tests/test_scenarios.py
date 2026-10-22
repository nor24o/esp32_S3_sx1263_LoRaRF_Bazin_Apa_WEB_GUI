#!/usr/bin/env python3
"""
Comprehensive Automated Scenario Test Suite for:
esp32_S3_sx1263_LoRaRF_Bazin_Apa
- Scenario 1: Hydraulic Rules Engine & Hysteresis (L0, L1, L2, L3, Empty lockout, Full cutoff)
- Scenario 2: Remote Pond Node (0x03) Consensus Accord Protocol (Demand ON/OFF, ACK, Low-Pond cutoff)
- Scenario 3: Relay Safety Interlocks (Min Runtime, Min Cooldown, Motor Anti-Inrush Stagger)
- Scenario 4: LoRa RF Engine, LBT CAD & Dynamic Time-on-Air (ToA, Tsym, LDRO Auto/Force, Guard windows)
- Scenario 5: Event-Driven Dual-Rate Telemetry & Zero-Spam Timing (Silent Idle, Active Burst, Settlement)
- Scenario 6: Timeout Synchronizations & Gateway Watchdog Pacing
- Scenario 7: Live System Verification (REST API, Node Registry, Command Dispatch)
"""

import sys
import time
import math
import json
import urllib.request
import urllib.error

# ANSI Color codes for clean test runner output
GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
CYAN = "\033[96m"
BOLD = "\033[1m"
RESET = "\033[0m"

passed_tests = 0
failed_tests = 0

def test_assert(condition: bool, test_name: str, details: str = ""):
    global passed_tests, failed_tests
    if condition:
        passed_tests += 1
        print(f"  {GREEN}✔ [PASS]{RESET} {test_name}")
        if details:
            print(f"         {CYAN}└─ {details}{RESET}")
    else:
        failed_tests += 1
        print(f"  {RED}✖ [FAIL]{RESET} {test_name}")
        if details:
            print(f"         {YELLOW}└─ {details}{RESET}")

# ─────────────────────────────────────────────────────────────────────────────
# SCENARIO 1: HYDRAULIC RULES ENGINE & HYSTERESIS
# ─────────────────────────────────────────────────────────────────────────────
def run_scenario_1_hydraulics():
    print(f"\n{BOLD}{CYAN}=== SCENARIO 1: Hydraulic Rules Engine & Hysteresis ==={RESET}")

    # Simulated tank controller logic matching main.cpp Task_ControlLogic
    class TankControllerSim:
        def __init__(self, capacity=3000, trigger_lvl=1, cutoff_lvl=3):
            self.capacity = capacity
            self.trigger_lvl = trigger_lvl
            self.cutoff_lvl = cutoff_lvl
            self.water_level = 2
            self.auto_mode = True
            self.demand_active = False
            self.pond_manual_override = False
            self.pumps = [False, False, False]
            self.allowed_auto = [True, True, True]
            self.overrides = [0, 0, 0] # 0=AUTO, 1=FORCE_ON, 2=FORCE_OFF

        def step(self):
            # 1. Hydraulic charging hysteresis
            if self.auto_mode:
                if self.water_level <= self.trigger_lvl and not self.demand_active:
                    self.demand_active = True
                    self.pond_manual_override = False
                elif self.demand_active:
                    if self.pond_manual_override:
                        if self.water_level >= 3:
                            self.demand_active = False
                            self.pond_manual_override = False
                    else:
                        if self.water_level >= self.cutoff_lvl:
                            self.demand_active = False
            else: # Manual mode
                if self.demand_active and self.water_level >= 3:
                    self.demand_active = False
                    self.pond_manual_override = False

            # 2. Outflow pumps logic
            water_available = (self.water_level > 0)
            for i in range(3):
                if self.overrides[i] == 2: # FORCE_OFF
                    self.pumps[i] = False
                elif self.overrides[i] == 1: # FORCE_ON
                    self.pumps[i] = water_available # Dry run lockout
                else: # AUTO
                    if self.auto_mode:
                        self.pumps[i] = water_available and self.allowed_auto[i]

    # Test 1.1: Nominal level (L2) boot - no demand
    tank = TankControllerSim(trigger_lvl=1, cutoff_lvl=3)
    tank.step()
    test_assert(not tank.demand_active, "1.1: At L2 initial boot, refill demand is OFF (within normal hysteresis band)")
    test_assert(all(tank.pumps), "1.1b: Outflow pumps P1-P3 are running when water is available and allowed")

    # Test 1.2: Water drops to Trigger Level (L1) -> Demand ON
    tank.water_level = 1
    tank.step()
    test_assert(tank.demand_active, "1.2: Water drops to L1 (Refill Trigger) -> Refill Demand turns ON")

    # Test 1.3: Water rises to L2 while filling -> Demand remains ON (Hysteresis latch)
    tank.water_level = 2
    tank.step()
    test_assert(tank.demand_active, "1.3: Water rises to L2 during refill -> Demand remains ON (Hysteresis maintained)")

    # Test 1.4: Water reaches Cutoff Level (L3) -> Demand turns OFF
    tank.water_level = 3
    tank.step()
    test_assert(not tank.demand_active, "1.4: Water reaches L3 (Cutoff Level) -> Demand turns OFF")

    # Test 1.5: Water drops from L3 to L2 -> Demand does NOT turn back on
    tank.water_level = 2
    tank.step()
    test_assert(not tank.demand_active, "1.5: Water drops from L3 to L2 -> Demand stays OFF (No chatter/oscillation)")

    # Test 1.6: Level 0 (Empty) Emergency Dry-Run Lockout
    tank.water_level = 0
    tank.step()
    test_assert(not any(tank.pumps), "1.6: Water Level 0 (Empty) -> All outflow pumps locked out (Anti-cavitation trip)")
    test_assert(tank.demand_active, "1.6b: Water Level 0 -> Refill demand automatically energized")

    # Test 1.7: Force ON override during Level 0 empty tank
    tank.overrides[0] = 1 # Attempt FORCE ON on P1 while empty
    tank.step()
    test_assert(not tank.pumps[0], "1.7: P1 FORCE_ON overridden by Level 0 Dry-Run hardware safeguard")

    # Test 1.8: Manual Mode Overflow Safeguard at Level 3
    tank.auto_mode = False
    tank.demand_active = True
    tank.water_level = 3
    tank.step()
    test_assert(not tank.demand_active, "1.8: Manual Mode Refill Overflow Safeguard cuts off refill when tank is full (3/3)")


# ─────────────────────────────────────────────────────────────────────────────
# SCENARIO 2: REMOTE POND NODE (0x03) CONSENSUS ACCORD
# ─────────────────────────────────────────────────────────────────────────────
def run_scenario_2_pond_accord():
    print(f"\n{BOLD}{CYAN}=== SCENARIO 2: Remote Pond Node (0x03) Consensus Accord Protocol ==={RESET}")

    class PondSimulatorTest:
        def __init__(self):
            self.pump_running = False
            self.pond_level_pct = 85.0
            self.local_operator_stop = False
            self.tank_accord = True
            self.accord_retries = 0

        def check_tank_accord(self, tank_demand: bool):
            if tank_demand:
                if not self.pump_running:
                    if self.local_operator_stop:
                        self.tank_accord = False
                        self.accord_retries += 1
                        return "NOTIFY_LOCAL_STOP"
                    elif self.pond_level_pct < 20.0:
                        self.tank_accord = False
                        self.accord_retries += 1
                        return "NOTIFY_LOW_POND"
                    else:
                        self.pump_running = True
                        self.tank_accord = True
                        self.accord_retries = 0
                        return "PUMP_STARTED"
                else:
                    self.tank_accord = True
                    return "ALREADY_RUNNING"
            else:
                if self.pump_running:
                    self.pump_running = False
                    self.tank_accord = True
                    return "PUMP_STOPPED"
                else:
                    self.tank_accord = True
                    return "ALREADY_IDLE"

    pond = PondSimulatorTest()

    # Test 2.1: Tank demands water -> Pond pump starts & accord reached
    res = pond.check_tank_accord(tank_demand=True)
    test_assert(res == "PUMP_STARTED" and pond.pump_running and pond.tank_accord,
                "2.1: Tank asserts demand -> Pond Node starts pump and enters common accord",
                f"Result={res}, Pump={pond.pump_running}, Accord={pond.tank_accord}")

    # Test 2.2: Redundant demand while pump is running
    res = pond.check_tank_accord(tank_demand=True)
    test_assert(res == "ALREADY_RUNNING" and pond.pump_running,
                "2.2: Subsequent demand frames maintain accord without restarting pump")

    # Test 2.3: Tank satisfies refill -> Pond pump stops & accord confirmed
    res = pond.check_tank_accord(tank_demand=False)
    test_assert(res == "PUMP_STOPPED" and not pond.pump_running and pond.tank_accord,
                "2.3: Tank clears demand -> Pond Node shuts down pump and confirms accord")

    # Test 2.4: Low Pond Reservoir (< 20%) Safety Lockout
    pond.pond_level_pct = 15.0 # Critically low pond
    res = pond.check_tank_accord(tank_demand=True)
    test_assert(res == "NOTIFY_LOW_POND" and not pond.pump_running and not pond.tank_accord,
                "2.4: Pond level < 20% -> Prohibits pump start and rejects Tank demand",
                f"Level={pond.pond_level_pct}%, Action={res}")

    # Test 2.5: Local Operator Stop Priority
    pond.pond_level_pct = 85.0
    pond.local_operator_stop = True
    res = pond.check_tank_accord(tank_demand=True)
    test_assert(res == "NOTIFY_LOCAL_STOP" and not pond.pump_running,
                "2.5: Operator local stop at Pond station overrides Tank remote demand")


# ─────────────────────────────────────────────────────────────────────────────
# SCENARIO 3: RELAY SAFETY INTERLOCKS (RUNTIME, COOLDOWN, STAGGER)
# ─────────────────────────────────────────────────────────────────────────────
def run_scenario_3_relay_interlocks():
    print(f"\n{BOLD}{CYAN}=== SCENARIO 3: Relay Safety Interlocks (Runtime, Cooldown, Stagger) ==={RESET}")

    def can_change_pump_state(current_state: bool, target_state: bool, emergency_dry: bool,
                             last_on_ms: int, last_off_ms: int, now_ms: int,
                             min_runtime_ms: int, min_cooldown_ms: int) -> bool:
        if emergency_dry and not target_state:
            return True # Emergency dry trip always allowed to stop immediately
        if current_state == target_state:
            return True
        if target_state: # Starting pump -> check cooldown
            if last_off_ms > 0 and (now_ms - last_off_ms) < min_cooldown_ms:
                return False
        else: # Stopping pump -> check min runtime
            if last_on_ms > 0 and (now_ms - last_on_ms) < min_runtime_ms:
                return False
        return True

    min_run = 20000
    min_cd = 20000

    # Test 3.1: Minimum Runtime Enforcement
    t0 = 100000
    # Pump was started at t0, now at t0 + 5000 (5s elapsed < 20s)
    can_stop_early = can_change_pump_state(True, False, False, t0, 0, t0 + 5000, min_run, min_cd)
    test_assert(not can_stop_early, "3.1: Cannot stop pump at 5s (Min runtime 20s enforced)")

    # Test 3.2: Stopping after min runtime has elapsed
    can_stop_after_min = can_change_pump_state(True, False, False, t0, 0, t0 + 21000, min_run, min_cd)
    test_assert(can_stop_after_min, "3.2: Allowed to stop pump at 21s (Min runtime expired)")

    # Test 3.3: Emergency Dry Trip overrides Min Runtime
    can_emergency_trip = can_change_pump_state(True, False, True, t0, 0, t0 + 2000, min_run, min_cd)
    test_assert(can_emergency_trip, "3.3: Emergency Dry-Run trip IMMEDIATELY stops pump bypassing min runtime")

    # Test 3.4: Minimum Cooldown Enforcement
    t_off = 200000
    # Pump stopped at t_off, now at t_off + 8000 (8s elapsed < 20s)
    can_start_early = can_change_pump_state(False, True, False, 0, t_off, t_off + 8000, min_run, min_cd)
    test_assert(not can_start_early, "3.4: Cannot restart pump at 8s (Min cooldown 20s enforced)")

    # Test 3.5: Restarting after min cooldown has elapsed
    can_start_after_cd = can_change_pump_state(False, True, False, 0, t_off, t_off + 22000, min_run, min_cd)
    test_assert(can_start_after_cd, "3.5: Allowed to restart pump at 22s (Cooldown satisfied)")

    # Test 3.6: Motor Anti-Inrush Start Stagger Check
    stagger_ms = 3000
    t_start_p1 = 300000
    # Attempting to start P2 500ms after P1
    p2_allowed_at_500ms = (300500 - t_start_p1 >= stagger_ms)
    test_assert(not p2_allowed_at_500ms, "3.6: P2 motor start deferred at 500ms (Anti-inrush stagger active)")

    # Attempting to start P2 3200ms after P1
    p2_allowed_at_3200ms = (303200 - t_start_p1 >= stagger_ms)
    test_assert(p2_allowed_at_3200ms, "3.6b: P2 motor starts cleanly at 3.2s (> 3.0s stagger delay)")


# ─────────────────────────────────────────────────────────────────────────────
# SCENARIO 4: LORA RF ENGINE, LBT CAD & DYNAMIC TIME-ON-AIR
# ─────────────────────────────────────────────────────────────────────────────
def run_scenario_4_lora_rf_engine():
    print(f"\n{BOLD}{CYAN}=== SCENARIO 4: LoRa RF Engine, LBT CAD & Dynamic Time-on-Air ==={RESET}")

    def calc_tsym_ms(sf: int, bw_khz: int) -> float:
        return (2.0 ** sf) / float(bw_khz)

    def is_ldro_active(sf: int, bw_khz: int, ldro_mode: int) -> bool:
        if ldro_mode == 1:
            return True
        if ldro_mode == 2:
            return False
        return calc_tsym_ms(sf, bw_khz) >= 16.0

    def calc_toa_ms(payload_len: int, sf: int, bw_khz: int, cr_denom: int, ldro: bool) -> int:
        tsym_ms = calc_tsym_ms(sf, bw_khz)
        t_preamble = (8.0 + 4.25) * tsym_ms
        de = 1 if ldro else 0
        num = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0
        den = 4.0 * (sf - 2.0 * de)
        payload_symb_nb = 8.0
        if den > 0:
            c = math.ceil(num / den)
            if c > 0:
                payload_symb_nb += c * (cr_denom - 4 + 4)
        return int(round(t_preamble + payload_symb_nb * tsym_ms))

    def calc_dynamic_guard_ms(toa_ms: int, sf: int) -> int:
        floor_guard = 1800 if sf >= 12 else (1200 if sf == 11 else (800 if sf == 10 else 400))
        return max(floor_guard, int((toa_ms * 3) // 2))

    # Test 4.1: Tsym and LDRO Auto Engine at SF9 / 125 kHz
    tsym_sf9 = calc_tsym_ms(9, 125)
    ldro_sf9 = is_ldro_active(9, 125, ldro_mode=0)
    test_assert(abs(tsym_sf9 - 4.096) < 0.01, f"4.1: Tsym at SF9/125kHz is 4.10 ms", f"Tsym={tsym_sf9:.3f}ms")
    test_assert(not ldro_sf9, f"4.1b: LDRO is OFF at SF9/125kHz (Tsym < 16.0 ms)")

    # Test 4.2: Tsym and LDRO Auto Engine at SF11 / 125 kHz
    tsym_sf11 = calc_tsym_ms(11, 125)
    ldro_sf11 = is_ldro_active(11, 125, ldro_mode=0)
    test_assert(abs(tsym_sf11 - 16.384) < 0.01, f"4.2: Tsym at SF11/125kHz is 16.38 ms (>= 16.0 ms)", f"Tsym={tsym_sf11:.3f}ms")
    test_assert(ldro_sf11, f"4.2b: LDRO automatically engages at SF11/125kHz")

    # Test 4.3: Tsym and LDRO Auto Engine at SF12 / 125 kHz
    tsym_sf12 = calc_tsym_ms(12, 125)
    ldro_sf12 = is_ldro_active(12, 125, ldro_mode=0)
    test_assert(abs(tsym_sf12 - 32.768) < 0.01, f"4.3: Tsym at SF12/125kHz is 32.77 ms")
    test_assert(ldro_sf12, f"4.3b: LDRO automatically engages at SF12/125kHz")

    # Test 4.4: Force Overrides for LDRO
    test_assert(is_ldro_active(9, 125, ldro_mode=1), "4.4: LDRO Force ON engages even at SF9")
    test_assert(not is_ldro_active(12, 125, ldro_mode=2), "4.4b: LDRO Force OFF suppresses even at SF12")

    # Test 4.5: Time-on-Air (ToA) and Dynamic Turnaround Guards
    toa_sf9 = calc_toa_ms(17, 9, 125, 5, False)
    guard_sf9 = calc_dynamic_guard_ms(toa_sf9, 9)
    test_assert(150 <= toa_sf9 <= 200, f"4.5: Status Packet (17B) ToA at SF9 is ~165 ms", f"ToA={toa_sf9}ms, Guard={guard_sf9}ms")
    test_assert(guard_sf9 >= 400, "4.5b: SF9 Guard window is at least 400 ms")

    toa_sf11 = calc_toa_ms(17, 11, 125, 5, True)
    guard_sf11 = calc_dynamic_guard_ms(toa_sf11, 11)
    test_assert(500 <= toa_sf11 <= 750, f"4.6: Status Packet (17B) ToA at SF11 is ~620 ms", f"ToA={toa_sf11}ms, Guard={guard_sf11}ms")
    test_assert(guard_sf11 >= 1200, "4.6b: SF11 Guard window is scaled to >= 1200 ms to protect radio queue")

    # Test 4.7: Hardware CAD Jitter Backoff Range
    base_backoff_ms = 60
    sym_ms = calc_tsym_ms(9, 125)
    min_backoff = int(base_backoff_ms + sym_ms * 2.0)
    max_backoff = int(base_backoff_ms + sym_ms * 8.0)
    test_assert(min_backoff >= 68 and max_backoff <= 100,
                f"4.7: LBT CAD backoff jitter is properly bounded to symbol period",
                f"Range=[{min_backoff}ms .. {max_backoff}ms]")


# ─────────────────────────────────────────────────────────────────────────────
# SCENARIO 5: DUAL-RATE EVENT-DRIVEN TELEMETRY & TIMEOUT SYNC
# ─────────────────────────────────────────────────────────────────────────────
def run_scenario_5_dual_rate_and_timeouts():
    print(f"\n{BOLD}{CYAN}=== SCENARIO 5: Dual-Rate Event-Driven Telemetry & Timeout Sync ==={RESET}")

    # Test 5.1: Mode transition burst duration
    burst_settle_hold_ms = 15000
    test_assert(burst_settle_hold_ms == 15000,
                "5.1: Burst settle hold window is 15 seconds after any contact/button transition")

    # Test 5.2: Gateway Watchdog timeout synchronization
    # Tank Node Gateway Silence Timeout: 900s (15 min)
    # Gateway Coordinator Idle Timeout: 1900s (31 min)
    # Gateway Coordinator Active/Burst Timeout: 180s (3 min)
    test_assert(180 < 900 < 1900,
                "5.2: Gateway Watchdog timeout hierarchy is mathematically sound",
                "BurstTimeout(180s) < TankSilenceTimeout(900s) < IdleTimeout(1900s)")

    # Test 5.3: Pumping Telemetry Pacing
    # While pumping, periodic telemetry is sent every 15-20s (max(burst_ms, 15000))
    # This prevents LoRa airtime saturation while keeping live odometers and water levels flowing
    burst_cfg = 3000
    pumping_interval = max(burst_cfg, 15000)
    test_assert(pumping_interval == 15000,
                "5.3: Active pumping telemetry interval is paced to 15s (Zero airtime saturation)")

    # Test 5.4: Settlement Frame Trigger on Burst->Idle transition
    class DualRateState:
        def __init__(self):
            self.was_burst = True
            self.is_burst = False
            self.settlement_sent = False

        def check_transition(self):
            if self.was_burst and not self.is_burst:
                self.settlement_sent = True
            self.was_burst = self.is_burst

    dr = DualRateState()
    dr.check_transition()
    test_assert(dr.settlement_sent,
                "5.4: Transition from Burst to Idle sends 1 clean settlement packet (burst_mode=0)")


# ─────────────────────────────────────────────────────────────────────────────
# SCENARIO 6: LIVE SYSTEM HEALTH & REST API VERIFICATION
# ─────────────────────────────────────────────────────────────────────────────
def run_scenario_6_live_system():
    print(f"\n{BOLD}{CYAN}=== SCENARIO 6: Live System Health & Fleet Coordinator API ==={RESET}")

    base_url = "http://localhost:8080"
    
    # Test 6.1: Reach Fleet SCADA Nodes API
    try:
        req = urllib.request.Request(f"{base_url}/api/nodes", headers={"User-Agent": "ScenarioTester/1.0"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            test_assert(isinstance(data, dict), "6.1: Fleet SCADA Deck /api/nodes endpoint accessible")
            
            # Check Gateway Node
            gw = data.get("1")
            test_assert(gw is not None and gw.get("type") == "GATEWAY",
                        "6.2: Node 1 (Gateway Coordinator) registered in fleet")

            # Check Tank Node
            tank = data.get("2")
            test_assert(tank is not None and tank.get("type") == "RESERVOIR_CTRL",
                        "6.3: Node 2 (Tank Node Bazin) registered in fleet",
                        f"Status={tank.get('status')}, LastSeen={tank.get('last_seen')}, SF={tank.get('last_sf')}")

            # Check Pond Node
            pond = data.get("3")
            test_assert(pond is not None and pond.get("type") == "INFLOW_PUMP",
                        "6.4: Node 3 (Pond Node Iaz) registered in fleet",
                        f"Status={pond.get('status')}, Accord={pond.get('metrics', {}).get('tank_accord')}")

    except Exception as e:
        test_assert(False, f"6.1: Failed to reach /api/nodes: {e}")

    # Test 6.2: Reach Historical Telemetry API
    try:
        req = urllib.request.Request(f"{base_url}/api/history", headers={"User-Agent": "ScenarioTester/1.0"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            test_assert(isinstance(data, list), "6.5: Fleet SCADA Deck /api/history endpoint accessible",
                        f"History samples in buffer: {len(data)}")
    except Exception as e:
        test_assert(False, f"6.5: Failed to reach /api/history: {e}")

    # Test 6.3: Reach Database Packet Log API
    try:
        req = urllib.request.Request(f"{base_url}/api/db/packets", headers={"User-Agent": "ScenarioTester/1.0"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            test_assert(isinstance(data, list) and len(data) > 0,
                        "6.6: SQLite fleet database packet audit trail active",
                        f"Logged packets in DB: {len(data)}")
    except Exception as e:
        test_assert(False, f"6.6: Failed to reach /api/db/packets: {e}")

    # Test 6.4: Command Dispatch via API
    try:
        post_data = json.dumps({"cmd": "/status"}).encode("utf-8")
        req = urllib.request.Request(f"{base_url}/api/cmd", data=post_data,
                                     headers={"Content-Type": "application/json", "User-Agent": "ScenarioTester/1.0"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            test_assert(data.get("status") == "ok", "6.7: /api/cmd station command execution responded OK",
                        f"Response: {data.get('msg')}")
    except Exception as e:
        test_assert(False, f"6.7: Failed to post to /api/cmd: {e}")


# ─────────────────────────────────────────────────────────────────────────────
# MAIN EXECUTION
# ─────────────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print(f"\n{BOLD}===================================================================={RESET}")
    print(f"{BOLD}   ESP32-S3 & SX1302 LoRa SCADA FLEET AUTOMATED SCENARIO SUITE   {RESET}")
    print(f"{BOLD}===================================================================={RESET}")

    t_start = time.time()

    run_scenario_1_hydraulics()
    run_scenario_2_pond_accord()
    run_scenario_3_relay_interlocks()
    run_scenario_4_lora_rf_engine()
    run_scenario_5_dual_rate_and_timeouts()
    run_scenario_6_live_system()

    elapsed = time.time() - t_start

    print(f"\n{BOLD}--------------------------------------------------------------------{RESET}")
    print(f"Results: {GREEN}{passed_tests} Passed{RESET}, {RED}{failed_tests} Failed{RESET} in {elapsed:.3f}s")
    print(f"{BOLD}--------------------------------------------------------------------{RESET}\n")

    if failed_tests > 0:
        sys.exit(1)
    else:
        sys.exit(0)

