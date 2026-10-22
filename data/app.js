// ─── TANK NODE SCADA CONTROLLER (WEBSOCKET & AUDIO SYNTH) ─────────
let ws = null;
let reconnectTimer = null;
let soundEnabled = true;
let audioCtx = null;

// Telemetry history buffer for sparkline canvas (last 40 points)
const MAX_HISTORY = 40;
const historyData = {
  levels: [],
  rssi: []
};

// ─── AUDIO SYNTHESIZER (Web Audio API) ─────────────────────────────
function initAudio() {
  if (!audioCtx) {
    const AudioContext = window.AudioContext || window.webkitAudioContext;
    if (AudioContext) audioCtx = new AudioContext();
  }
  if (audioCtx && audioCtx.state === 'suspended') {
    audioCtx.resume();
  }
}

function playTone(freq = 600, duration = 0.08, type = 'sine', gainVal = 0.08) {
  if (!soundEnabled) return;
  try {
    initAudio();
    if (!audioCtx) return;
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = type;
    osc.frequency.setValueAtTime(freq, audioCtx.currentTime);
    gain.gain.setValueAtTime(gainVal, audioCtx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + duration);
    osc.connect(gain);
    gain.connect(audioCtx.destination);
    osc.start();
    osc.stop(audioCtx.currentTime + duration);
  } catch (e) {
    console.warn("Audio error:", e);
  }
}

function playClick() { playTone(800, 0.05, 'triangle', 0.06); }
function playChime() {
  playTone(523.25, 0.1, 'sine', 0.08);
  setTimeout(() => playTone(659.25, 0.15, 'sine', 0.08), 100);
}
function playAlert() {
  playTone(280, 0.2, 'sawtooth', 0.1);
  setTimeout(() => playTone(240, 0.25, 'sawtooth', 0.12), 220);
}

// ─── WEBSOCKET CLIENT & STATE ──────────────────────────────────────
function connectWebSocket() {
  const host = window.location.hostname || "192.168.4.1";
  const wsUrl = `ws://${host}:81/`;

  const ind = document.getElementById("wsIndicator");
  ind.classList.remove("online");

  ws = new WebSocket(wsUrl);

  ws.onopen = () => {
    console.log("[WS] Connected to", wsUrl);
    ind.classList.add("online");
    showToast("Connected to Tank Node", false);
    playChime();
    ws.send(JSON.stringify({ cmd: "get_config" }));
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === "telemetry") {
        renderTelemetry(msg);
      } else if (msg.type === "config") {
        renderConfig(msg);
      } else if (msg.type === "log_event") {
        addLogEntry(msg);
      } else if (msg.type === "toast") {
        showToast(msg.msg, msg.is_err);
        if (msg.is_err) playAlert();
        else playClick();
      }
    } catch (err) {
      console.error("[WS] Parse error:", err, event.data);
    }
  };

  ws.onclose = () => {
    ind.classList.remove("online");
    if (!reconnectTimer) {
      reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectWebSocket();
      }, 2000);
    }
  };

  ws.onerror = (e) => {
    console.warn("[WS] Error:", e);
    ws.close();
  };
}

function sendJson(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  } else {
    showToast("WebSocket disconnected!", true);
    playAlert();
  }
}

// ─── TELEMETRY RENDERING ───────────────────────────────────────────
let lastErrorCode = 0;

function formatTimeMinSec(sec) {
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

function formatHoursMins(sec) {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  return `${h}h ${m.toString().padStart(2, '0')}m`;
}

function renderTelemetry(t) {
  // 0. ISA-18.2 Priority Alarm & Operational Status Banner
  const alarmContainer = document.getElementById("alarmBanner");
  const alarmBeacon = document.getElementById("alarmBeacon");
  const alarmTitle = document.getElementById("alarmTitle");
  const alarmDesc = document.getElementById("alarmDesc");

  if (t.err === 4) {
    if (alarmContainer) alarmContainer.className = "alarm-banner-container warning glass-card";
    if (alarmBeacon) alarmBeacon.className = "alarm-beacon warning";
    if (alarmTitle) alarmTitle.innerText = "WARNING • FLOAT SENSOR PLAUSIBILITY VIOLATION";
    if (alarmDesc) alarmDesc.innerText = "Float switch contacts contradictory. Verify PCF8574 float wiring & buoyant switches.";
  } else if (t.err === 2) {
    if (alarmContainer) alarmContainer.className = "alarm-banner-container critical glass-card";
    if (alarmBeacon) alarmBeacon.className = "alarm-beacon critical";
    if (alarmTitle) alarmTitle.innerText = "CRITICAL FAULT • PCF8574 I²C DISCONNECTED";
    if (alarmDesc) alarmDesc.innerText = "Digital I/O expander uncommunicative on I2C bus (SDA:1, SCL:2). Hardware interlock tripped.";
  } else if (t.err === 1) {
    if (alarmContainer) alarmContainer.className = "alarm-banner-container critical glass-card";
    if (alarmBeacon) alarmBeacon.className = "alarm-beacon critical";
    if (alarmTitle) alarmTitle.innerText = "RADIO FAULT • SX1262 LoRa SPI LINK LOST";
    if (alarmDesc) alarmDesc.innerText = "LoRa transceiver not responding to SPI transactions. Remote telemetry unreachable.";
  } else if (t.err === 3) {
    if (alarmContainer) alarmContainer.className = "alarm-banner-container warning glass-card";
    if (alarmBeacon) alarmBeacon.className = "alarm-beacon warning";
    if (alarmTitle) alarmTitle.innerText = "NETWORK WARNING • LORA GATEWAY TIMEOUT";
    if (alarmDesc) alarmDesc.innerText = "No downlink heartbeat received from LoRa Gateway Coordinator (>15 min silent).";
  } else if (t.wl === 0) {
    if (alarmContainer) alarmContainer.className = "alarm-banner-container critical glass-card";
    if (alarmBeacon) alarmBeacon.className = "alarm-beacon critical";
    if (alarmTitle) alarmTitle.innerText = "CRITICAL TRIP • LEVEL 0 DRY-RUN PROTECTION ACTIVE";
    if (alarmDesc) alarmDesc.innerText = "Reservoir is empty (<1,000L). All outflow pumps locked out to protect against cavitation. Pond refill prioritized.";
  } else {
    const staggerSec = ((t.cfg_stagger !== undefined ? t.cfg_stagger : 3000) / 1000).toFixed(1);
    const runSec = Math.round((t.cfg_runtime || 20000) / 1000);
    if (alarmContainer) alarmContainer.className = "alarm-banner-container glass-card";
    if (alarmBeacon) alarmBeacon.className = "alarm-beacon normal";
    if (alarmTitle) alarmTitle.innerText = "NORMAL OPERATION • ALL INTERLOCKS NOMINAL";
    if (alarmDesc) alarmDesc.innerText = `Water level within operating limits. Anti-inrush start stagger (${staggerSec}s) and anti-chatter (${runSec}s) locks active.`;
  }

  // Active Rule determination (firmware streams t.active_rule: 1..4)
  const activeRuleNames = [
    "UNKNOWN",
    "RULE 1 (FULL 100%)",
    "RULE 2 (MID 66%)",
    "RULE 3 (LOW 33%)",
    "RULE 4 (DRY RUN 0%)"
  ];
  const curRule = (t.active_rule >= 1 && t.active_rule <= 4) ? t.active_rule : (t.wl === 0 ? 4 : (t.wl === 1 ? 3 : (t.wl === 2 ? 2 : 1)));
  const bRule = document.getElementById("bannerActiveRule");
  if (bRule) bRule.innerText = activeRuleNames[curRule] || `RULE ${curRule}`;

  // Overrides summary
  let overrideCount = 0;
  const ovrDescriptions = [];
  if (t.overrides) {
    for (let i = 0; i < 3; i++) {
      if (t.overrides[i] === 1) {
        overrideCount++;
        ovrDescriptions.push(`P${i+1}: FORCE ON`);
      } else if (t.overrides[i] === 2) {
        overrideCount++;
        ovrDescriptions.push(`P${i+1}: LOCKED OFF`);
      }
    }
  }
  const bOvr = document.getElementById("bannerOverrides");
  if (bOvr) {
    bOvr.innerText = overrideCount > 0 ? ovrDescriptions.join(", ") : "NONE (AUTO)";
    bOvr.style.color = overrideCount > 0 ? "var(--amber)" : "var(--cyan)";
  }

  const bLatch = document.getElementById("bannerLatch");
  if (bLatch) {
    if (t.demand && t.pond) {
      bLatch.innerText = "PUMPING (0x03 ACK)";
      bLatch.style.color = "var(--green)";
    } else if (t.demand) {
      bLatch.innerText = "DEMAND ON (SYNCING)";
      bLatch.style.color = "var(--amber)";
    } else {
      bLatch.innerText = "IDLE";
      bLatch.style.color = "var(--muted)";
    }
  }

  const bRadioMode = document.getElementById("bannerRadioMode");
  if (bRadioMode) {
    const burst = Boolean(t.burst_mode);
    bRadioMode.innerText = burst ? "EVENT BURST" : "SILENT IDLE";
    bRadioMode.style.color = burst ? "var(--cyan)" : "var(--green)";
  }

  // Quick RF Bar & Quick SF Selector
  const qRfTxt = document.getElementById("quickRfText");
  const qRfDot = document.getElementById("qrfDot");
  const qSfSel = document.getElementById("quickSfSelect");
  if (qRfTxt && t.lora_sf !== undefined) {
    const bwKhz = (t.lora_bw > 1000) ? Math.round(t.lora_bw / 1000) : (t.lora_bw || 125);
    const pwr = (t.lora_power !== undefined) ? t.lora_power : (t.lora_pwr !== undefined ? t.lora_pwr : 22);
    const ldroTag = t.ldro_active ? "LDRO" : `4/${t.lora_cr || 5}`;
    qRfTxt.innerText = `SF${t.lora_sf} • ${bwKhz}k • +${pwr}dBm • ${ldroTag} • ToA:${t.toa_ms || '--'}ms`;
  }
  if (qRfDot) {
    qRfDot.classList.toggle("burst", Boolean(t.burst_mode));
  }
  if (qSfSel && t.lora_sf !== undefined && document.activeElement !== qSfSel) {
    qSfSel.value = String(t.lora_sf);
  }

  // Header TX / RX Counter Badge
  const bPkt = document.getElementById("badgePktCnt");
  if (bPkt && t.tx_cnt !== undefined) {
    bPkt.innerText = `TX: ${t.tx_cnt} | RX: ${t.rx_cnt || 0}`;
  }

  // Active Demodulator LDRO, ToA, and CAD status badges in modal
  const ldroBadge = document.getElementById("ldroLiveBadge");
  if (ldroBadge && t.ldro_active !== undefined) {
    if (t.ldro_active) {
      ldroBadge.className = "badge-tag running";
      ldroBadge.innerText = t.lora_ldro === 1 ? "LDRO: FORCE ON" : `LDRO: AUTO ON (SF${t.lora_sf || 11})`;
    } else {
      ldroBadge.className = "badge-tag idle";
      ldroBadge.innerText = t.lora_ldro === 2 ? "LDRO: FORCE OFF" : `LDRO: OFF (SF${t.lora_sf || 9})`;
    }
  }
  const toaBadge = document.getElementById("toaLiveBadge");
  if (toaBadge && t.toa_ms !== undefined) {
    toaBadge.innerText = `ToA: ${t.toa_ms} ms • Tsym: ${(t.sym_ms || 0).toFixed(2)} ms`;
  }
  const cadSum = document.getElementById("cadLiveSummary");
  if (cadSum) {
    cadSum.innerText = `🛡️ LBT 4-Symbol Hardware CAD (SX126X_CAD_ON_4_SYMB) Active • Guard: ${t.guard_ms || 400} ms • Deferrals: ${t.cad_busy_cnt || 0}`;
  }

  // 1. Dynamic Reservoir Capacity & Water Level Height/Text
  const capLiters = t.tank_capacity || 10000;
  const heights = [0, 33, 66, 100];
  const labels = [
    `WL: 0 (EMPTY 0% <${Math.round(capLiters * 0.1).toLocaleString()}L)`,
    `WL: 1 (LOW 33% ~${Math.round(capLiters * 0.33).toLocaleString()}L)`,
    `WL: 2 (MID 66% ~${Math.round(capLiters * 0.66).toLocaleString()}L)`,
    `WL: 3 (FULL 100% ~${capLiters.toLocaleString()}L)`
  ];
  const wl = Math.min(Math.max(t.wl, 0), 3);

  const liquidBody = document.getElementById("liquidBody");
  if (liquidBody) liquidBody.style.height = `${heights[wl]}%`;
  const lvlText = document.getElementById("tankLevelText");
  if (lvlText) lvlText.innerText = labels[wl];

  const pondDesc = document.getElementById("pondDesc");
  if (pondDesc && t.refill_trigger !== undefined && t.refill_cutoff !== undefined) {
    pondDesc.innerText = `Refills reservoir via telemetry demand packet. Auto trigger at ≤ L${t.refill_trigger}, cutoff at ≥ L${t.refill_cutoff}.`;
  }

  // 2. Float Sensor Buoys (L1, L2, L3)
  const b1 = document.getElementById("beadL1");
  const b2 = document.getElementById("beadL2");
  const b3 = document.getElementById("beadL3");

  if (b1) b1.classList.toggle("submerged", wl >= 1);
  if (b2) b2.classList.toggle("submerged", wl >= 2);
  if (b3) b3.classList.toggle("submerged", wl >= 3);

  // Sensor Plausibility Alert Check
  const alertBar = document.getElementById("sensorAlertBar");
  const healthText = document.getElementById("sensorHealthText");

  if (alertBar && healthText) {
    if (t.err === 4) { // ERR_FLOAT_PLAUSIBILITY
      if (b1) b1.classList.add("fault");
      if (b2) b2.classList.add("fault");
      if (b3) b3.classList.add("fault");
      alertBar.classList.add("alert");
      healthText.innerText = "SENSOR FAULT: Float switch plausibility violation!";
      if (lastErrorCode !== 4) playAlert();
    } else if (t.err === 2) { // ERR_PCF_I2C
      alertBar.classList.add("alert");
      healthText.innerText = "HARDWARE FAULT: PCF8574 I²C Bus Disconnected!";
      if (lastErrorCode !== 2) playAlert();
    } else if (t.err === 3) { // ERR_GATEWAY_TIMEOUT
      alertBar.classList.add("alert");
      healthText.innerText = "NETWORK WARNING: LoRa Gateway Coordinator Silent (>15 min)!";
      if (lastErrorCode !== 3) playAlert();
    } else {
      if (b1) b1.classList.remove("fault");
      if (b2) b2.classList.remove("fault");
      if (b3) b3.classList.remove("fault");
      alertBar.classList.remove("alert");
      healthText.innerText = "Float switches healthy & plausible • Direct suction manifold";
    }
  }
  lastErrorCode = t.err;

  // 3. Inflow Pipe (From Remote Pond)
  const inflowWater = document.getElementById("inflowPipeWater");
  const waterJet = document.getElementById("waterJet");
  const pondDemand = Boolean(t.demand);
  const pondRemoteAck = Boolean(t.pond);
  const pondFlowVisual = pondDemand || pondRemoteAck;

  if (inflowWater) inflowWater.classList.toggle("active", pondFlowVisual);
  if (waterJet) waterJet.classList.toggle("active", pondFlowVisual);

  // Pond Controller Card (Dual-State: Local Demand vs Remote 0x03 Accord)
  const indPond = document.getElementById("indPond");
  const btnPond = document.getElementById("btnPond");
  const latchBadge = document.getElementById("latchBadge");
  const accordBadge = document.getElementById("pondAccordBadge");

  if (indPond) indPond.classList.toggle("active", pondFlowVisual);
  if (btnPond) {
    btnPond.innerText = pondDemand ? "STOP POND REFILL" : "REQUEST REFILL";
    btnPond.classList.toggle("active-demand", pondDemand);
  }
  if (latchBadge) {
    latchBadge.innerText = pondDemand ? "DEMAND LATCH: ON" : "DEMAND: IDLE";
    latchBadge.classList.toggle("active", pondDemand);
  }
  if (accordBadge) {
    if (pondDemand && pondRemoteAck) {
      accordBadge.innerText = "0x03 ACK: PUMPING";
      accordBadge.className = "pond-accord-badge synced-on";
    } else if (pondDemand && !pondRemoteAck) {
      accordBadge.innerText = "0x03 ACK: SYNCING...";
      accordBadge.className = "pond-accord-badge syncing";
    } else if (!pondDemand && pondRemoteAck) {
      accordBadge.innerText = "0x03 ACK: STOPPING...";
      accordBadge.className = "pond-accord-badge syncing";
    } else {
      accordBadge.innerText = "0x03 ACK: STANDBY";
      accordBadge.className = "pond-accord-badge";
    }
  }

  // 4. Outflow Pipes & Pump Faceplates (P1, P2, P3)
  const isAuto = (t.mode === "AUTO");

  for (let i = 0; i < 3; i++) {
    const pNum = i + 1;
    const pState = Boolean(t.pumps[i]);
    const mask = t.auto_mask ? Boolean(t.auto_mask[i]) : true;
    const ovr = t.overrides ? t.overrides[i] : 0;
    const timedLeft = t.timed_left ? t.timed_left[i] : 0;
    const runLeft = t.run_left ? t.run_left[i] : 0;
    const cdLeft = t.cd_left ? t.cd_left[i] : 0;

    // Turbines & Streams in P&ID schematic
    const turb = document.getElementById(`turbineP${pNum}`);
    const stream = document.getElementById(`streamP${pNum}`);
    if (turb) turb.classList.toggle("active", pState);
    if (stream) stream.classList.toggle("active", pState);

    // Indicator
    const ind = document.getElementById(`indP${pNum}`);
    if (ind) ind.classList.toggle("active", pState);

    // 3-State Override Buttons
    const btnAuto = document.getElementById(`ovrAutoP${pNum}`);
    const btnOn = document.getElementById(`ovrOnP${pNum}`);
    const btnOff = document.getElementById(`ovrOffP${pNum}`);
    if (btnAuto) btnAuto.classList.toggle("active", ovr === 0);
    if (btnOn) btnOn.classList.toggle("active", ovr === 1);
    if (btnOff) btnOff.classList.toggle("active", ovr === 2);

    // Timed Run Badge
    const timedBadge = document.getElementById(`timedBadgeP${pNum}`);
    if (timedBadge) {
      if (timedLeft > 0) {
        timedBadge.style.display = "inline-block";
        timedBadge.innerText = formatTimeMinSec(timedLeft);
      } else {
        timedBadge.style.display = "none";
      }
    }

    // Status Pill
    const pill = document.getElementById(`pillP${pNum}`);
    const btnToggle = document.getElementById(`btnP${pNum}`);
    if (pill) {
      if (pState) {
        if (timedLeft > 0) {
          pill.innerText = `TIMED [${formatTimeMinSec(timedLeft)}]`;
          pill.className = "status-pill running";
        } else if (ovr === 1) {
          pill.innerText = "FORCED ON";
          pill.className = "status-pill running";
        } else {
          pill.innerText = "RUNNING (AUTO)";
          pill.className = "status-pill running";
        }
      } else {
        if (ovr === 2) {
          pill.innerText = "LOCKED OFF";
          pill.className = "status-pill locked";
        } else if (cdLeft > 0) {
          pill.innerText = `COOLDOWN [${cdLeft}s]`;
          pill.className = "status-pill cooldown";
        } else if (wl === 0) {
          pill.innerText = "TRIPPED (EMPTY)";
          pill.className = "status-pill fault";
        } else {
          pill.innerText = "IDLE / READY";
          pill.className = "status-pill";
        }
      }
    }

    if (btnToggle) {
      btnToggle.innerText = pState ? `STOP P${pNum}` : `START P${pNum}`;
    }

    // Auto mask checkbox
    const chkMask = document.getElementById(`maskP${pNum}`);
    if (chkMask) chkMask.checked = mask;

    // Feedback Gauge (Interlocks & Locks)
    const gLbl = document.getElementById(`gaugeLblP${pNum}`);
    const gVal = document.getElementById(`gaugeValP${pNum}`);
    const gFill = document.getElementById(`gaugeFillP${pNum}`);
    const minRunSec = (t.cfg_runtime || 20000) / 1000;
    const minCdSec = (t.cfg_cooldown || 20000) / 1000;

    if (pState && runLeft > 0) {
      if (gLbl) gLbl.innerText = "MIN RUN LOCK:";
      if (gVal) { gVal.innerText = `${runLeft}s remaining`; gVal.className = "gauge-val running"; }
      if (gFill) { gFill.style.width = `${Math.round((runLeft / minRunSec) * 100)}%`; gFill.style.background = "var(--cyan)"; }
    } else if (!pState && cdLeft > 0) {
      if (gLbl) gLbl.innerText = "COOLDOWN LOCK:";
      if (gVal) { gVal.innerText = `${cdLeft}s remaining`; gVal.className = "gauge-val locked"; }
      if (gFill) { gFill.style.width = `${Math.round((cdLeft / minCdSec) * 100)}%`; gFill.style.background = "var(--amber)"; }
    } else if (wl === 0) {
      if (gLbl) gLbl.innerText = "DRY RUN TRIP:";
      if (gVal) { gVal.innerText = "L0 Lockout (Inhibited)"; gVal.className = "gauge-val locked"; }
      if (gFill) { gFill.style.width = "100%"; gFill.style.background = "var(--red)"; }
    } else if (ovr === 2) {
      if (gLbl) gLbl.innerText = "HARD LOCKOUT:";
      if (gVal) { gVal.innerText = "Operator Inhibit (Off)"; gVal.className = "gauge-val locked"; }
      if (gFill) { gFill.style.width = "100%"; gFill.style.background = "var(--red)"; }
    } else {
      if (gLbl) gLbl.innerText = "INTERLOCK:";
      if (gVal) { gVal.innerText = "Ready (Clear)"; gVal.className = "gauge-val"; }
      if (gFill) { gFill.style.width = "0%"; }
    }

    // Odometer
    const odo = document.getElementById(`odoP${pNum}`);
    if (odo && t.rt_stats) {
      const sec = t.rt_stats[i] || 0;
      odo.innerText = `Runtime: ${formatHoursMins(sec)} • Total: ${sec}s`;
    }
  }

  // 5. Operation Mode UI
  const btnAuto = document.getElementById("btnModeAuto");
  const btnMan = document.getElementById("btnModeMan");
  const modeBanner = document.getElementById("modeBanner");
  const modeHint = document.getElementById("modeHint");

  if (btnAuto) btnAuto.classList.toggle("active", isAuto);
  if (btnMan) btnMan.classList.toggle("active", !isAuto);
  if (modeBanner) {
    modeBanner.innerText = isAuto ? "AUTOMATIC MODE" : "MANUAL MODE";
    modeBanner.className = isAuto ? "mode-status-text auto" : "mode-status-text";
  }
  if (modeHint) {
    modeHint.innerText = isAuto
      ? "Auto-irrigation active. Individual overrides take priority. Stagger delay (3s) enforced."
      : "Manual control deck active. Anti-chatter cooldown & dry-run interlocks enforced.";
  }

  // 6. HUD Badges & Diagnostics
  const bRssi = document.getElementById("badgeRssi");
  if (bRssi) bRssi.innerText = `RSSI: ${t.rssi} dBm`;
  const bUp = document.getElementById("badgeUptime");
  if (bUp) bUp.innerText = `Up: ${formatUptime(t.uptime)}`;

  const bConn = document.getElementById("badgeConn");
  if (bConn) {
    if (t.sta && t.sta_ip) {
      bConn.innerText = `STA: ${t.sta_ip}`;
    } else {
      bConn.innerText = "SoftAP (192.168.4.1)";
    }
  }

  const dPcf = document.getElementById("diagPcf");
  if (dPcf) {
    dPcf.innerText = t.pcf_ok ? "ONLINE" : "FAULT";
    dPcf.style.color = t.pcf_ok ? "var(--green)" : "var(--red)";
  }
  const dRssi = document.getElementById("diagRssi");
  if (dRssi) dRssi.innerText = `${t.rssi} dBm`;
  const dSnr = document.getElementById("diagSnr");
  if (dSnr) dSnr.innerText = `${t.snr} dB`;
  const dHeap = document.getElementById("diagHeap");
  if (dHeap) dHeap.innerText = `${Math.round(t.heap / 1024)} KB`;
  const dBoots = document.getElementById("diagBoots");
  if (dBoots) dBoots.innerText = `#${t.boots}`;

  // 7. Update Sparkline Canvas
  updateSparkline(wl, t.rssi);

  // 8. Update Hydraulic Automation Matrix Rows
  for (let r = 1; r <= 4; r++) {
    const row = document.getElementById(`ruleRow${r}`);
    const badge = document.getElementById(`ruleBadge${r}`);
    const isActive = (r === curRule);
    if (row) row.classList.toggle("active-rule", isActive);
    if (badge) {
      badge.innerText = isActive ? "▶ ACTIVE NOW" : "IDLE";
    }
  }
  const matActiveBadge = document.getElementById("activeRuleBadge");
  if (matActiveBadge) {
    matActiveBadge.innerText = `ACTIVE: ${activeRuleNames[curRule] || ('RULE ' + curRule)}`;
  }

  // 9. Render Live SCADA I/O & Parameters Matrix
  renderMatrix(t);
}

// ─── CONFIG RENDERING ──────────────────────────────────────────────
function renderConfig(c) {
  const rInput = document.getElementById("cfgRuntime");
  const cInput = document.getElementById("cfgCooldown");
  const stgInput = document.getElementById("cfgStagger");
  const tiInput = document.getElementById("cfgTelemIdle");
  const tbInput = document.getElementById("cfgTelemBurst");
  const bSelect = document.getElementById("cfgBootAuto");
  const aCheck = document.getElementById("cfgAlwaysOn");
  const sInput = document.getElementById("cfgStaSsid");

  if (rInput && c.runtime !== undefined) rInput.value = c.runtime;
  if (cInput && c.cooldown !== undefined) cInput.value = c.cooldown;
  if (stgInput && c.stagger !== undefined) stgInput.value = c.stagger;
  if (tiInput && (c.telem_idle !== undefined || c.telem !== undefined)) {
    tiInput.value = (c.telem_idle !== undefined) ? c.telem_idle : c.telem;
  }
  if (tbInput && c.telem_burst !== undefined) tbInput.value = c.telem_burst;
  if (bSelect && c.boot_auto !== undefined) bSelect.value = c.boot_auto;
  if (aCheck && c.always_on !== undefined) aCheck.checked = Boolean(c.always_on);
  if (sInput && c.sta_ssid) sInput.value = c.sta_ssid;

  // Hydraulics
  const capInput = document.getElementById("cfgCapacity");
  const trigSelect = document.getElementById("cfgTrigger");
  const cutSelect = document.getElementById("cfgCutoff");
  if (capInput && c.tank_capacity) {
    capInput.value = c.tank_capacity;
    tankCapacityLiters = c.tank_capacity;
  }
  if (trigSelect && c.refill_trigger !== undefined) trigSelect.value = c.refill_trigger;
  if (cutSelect && c.refill_cutoff !== undefined) cutSelect.value = c.refill_cutoff;

  if (c.auto_mask && Array.isArray(c.auto_mask)) {
    for (let i = 0; i < 3; i++) {
      const mChk = document.getElementById(`cfgMaskP${i + 1}`);
      if (mChk) mChk.checked = Boolean(c.auto_mask[i]);
    }
  }

  // LoRa RF & LBT
  const freqInput = document.getElementById("cfgLoraFreq");
  const pwrInput = document.getElementById("cfgLoraPwr");
  const sfSelect = document.getElementById("cfgLoraSf");
  const bwSelect = document.getElementById("cfgLoraBw");
  const crSelect = document.getElementById("cfgLoraCr");
  const ldroSelect = document.getElementById("cfgLoraLdro");
  const syncInput = document.getElementById("cfgLoraSync");
  const lbtRetInput = document.getElementById("cfgLbtRetries");
  const lbtBackInput = document.getElementById("cfgLbtBackoff");

  if (freqInput && c.lora_freq) freqInput.value = c.lora_freq;
  if (pwrInput && c.lora_power !== undefined) pwrInput.value = c.lora_power;
  if (sfSelect && c.lora_sf) sfSelect.value = c.lora_sf;
  if (bwSelect && c.lora_bw) bwSelect.value = c.lora_bw;
  if (crSelect && c.lora_cr) crSelect.value = c.lora_cr;
  if (ldroSelect && c.lora_ldro !== undefined) ldroSelect.value = c.lora_ldro;
  if (syncInput && c.lora_sync) syncInput.value = c.lora_sync;
  if (lbtRetInput && c.lbt_retries !== undefined) lbtRetInput.value = c.lbt_retries;
  if (lbtBackInput && c.lbt_backoff !== undefined) lbtBackInput.value = c.lbt_backoff;

  previewLoraAirtime();
}

function previewLoraAirtime() {
  const sfEl = document.getElementById("cfgLoraSf");
  const bwEl = document.getElementById("cfgLoraBw");
  const crEl = document.getElementById("cfgLoraCr");
  const ldroEl = document.getElementById("cfgLoraLdro");
  if (!sfEl || !bwEl || !crEl || !ldroEl) return;

  const sf = parseInt(sfEl.value, 10) || 9;
  const rawBw = parseInt(bwEl.value, 10) || 125;
  const bwKhz = rawBw > 1000 ? (rawBw / 1000.0) : rawBw;
  const cr = parseInt(crEl.value, 10) || 5;
  const ldroMode = parseInt(ldroEl.value, 10) || 0;

  const tSymMs = Math.pow(2, sf) / bwKhz;
  const ldroOn = (ldroMode === 1) ? true : (ldroMode === 2 ? false : (tSymMs >= 16.0));
  const de = ldroOn ? 1 : 0;
  const tPreamble = (8.0 + 4.25) * tSymMs;
  const num = 8.0 * 17.0 - 4.0 * sf + 28.0 + 16.0;
  const den = 4.0 * (sf - 2.0 * de);
  let payloadSymbNb = 8.0;
  if (den > 0) {
    const c = Math.ceil(num / den);
    if (c > 0) payloadSymbNb += c * cr;
  }
  const toaMs = Math.round(tPreamble + payloadSymbNb * tSymMs);
  const sfFloor = (sf >= 12) ? 1800 : (sf === 11 ? 1200 : (sf === 10 ? 800 : 400));
  const guardMs = Math.max(sfFloor, Math.floor((toaMs * 3) / 2));

  const ldroBadge = document.getElementById("ldroLiveBadge");
  if (ldroBadge) {
    const modeStr = ldroMode === 0 ? "AUTO" : (ldroMode === 1 ? "FORCE ON" : "FORCE OFF");
    ldroBadge.innerText = `LDRO: ${ldroOn ? "ON" : "OFF"} (${modeStr} | Tsym=${tSymMs.toFixed(2)}ms)`;
    ldroBadge.className = ldroOn ? "badge-tag ok" : "badge-tag idle";
  }

  const toaBadge = document.getElementById("toaLiveBadge");
  if (toaBadge) {
    toaBadge.innerText = `ToA: ${toaMs} ms | Guard: ${guardMs} ms`;
  }
}

// ─── REAL-TIME SPARKLINE CANVAS GRAPH ──────────────────────────────
function updateSparkline(level, rssi) {
  historyData.levels.push(level);
  historyData.rssi.push(rssi);
  if (historyData.levels.length > MAX_HISTORY) historyData.levels.shift();
  if (historyData.rssi.length > MAX_HISTORY) historyData.rssi.shift();

  const canvas = document.getElementById("sparklineCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  // Draw background grid lines
  ctx.strokeStyle = "rgba(255, 255, 255, 0.05)";
  ctx.lineWidth = 1;
  for (let y = 20; y < h; y += 25) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const n = historyData.levels.length;
  if (n < 2) return;

  const stepX = w / (MAX_HISTORY - 1);

  // 1. Draw Water Level line (0 to 3 scaled to height)
  ctx.beginPath();
  ctx.strokeStyle = "#0ea5e9";
  ctx.lineWidth = 2.0;
  ctx.shadowColor = "rgba(14, 165, 233, 0.3)";
  ctx.shadowBlur = 4;

  for (let i = 0; i < n; i++) {
    const x = i * stepX;
    // Map WL (0 to 3) to canvas y: WL 3 is top (y=15), WL 0 is bottom (y=h-15)
    const y = (h - 20) - (historyData.levels[i] / 3) * (h - 40);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // 2. Draw RSSI line (-120 dBm to -30 dBm)
  ctx.beginPath();
  ctx.strokeStyle = "#a78bfa";
  ctx.lineWidth = 1.5;
  ctx.shadowColor = "rgba(167, 139, 250, 0.25)";
  ctx.shadowBlur = 4;

  for (let i = 0; i < n; i++) {
    const x = i * stepX;
    const r = Math.max(-120, Math.min(-30, historyData.rssi[i]));
    const norm = (r - (-120)) / (90); // 0 to 1
    const y = (h - 15) - norm * (h - 30);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
  ctx.shadowBlur = 0;
}

// ─── USER ACTIONS & COMMANDS ───────────────────────────────────────
function switchDashboardView(view) {
  playClick();
  const mainGrid = document.querySelector(".main-grid");
  const autoSec = document.getElementById("automationMatrixSection");
  const liveSec = document.getElementById("ioMatrixSection");
  const matrixContent = document.getElementById("matrixContent");

  document.querySelectorAll(".view-tab-btn").forEach(b => b.classList.remove("active"));
  if (view === "all") {
    document.getElementById("vTabAll")?.classList.add("active");
    if (mainGrid) mainGrid.style.display = "";
    if (autoSec) autoSec.style.display = "";
    if (liveSec) liveSec.style.display = "";
  } else if (view === "ops") {
    document.getElementById("vTabOps")?.classList.add("active");
    if (mainGrid) mainGrid.style.display = "";
    if (autoSec) autoSec.style.display = "none";
    if (liveSec) liveSec.style.display = "none";
  } else if (view === "matrix") {
    document.getElementById("vTabMatrix")?.classList.add("active");
    if (mainGrid) mainGrid.style.display = "none";
    if (autoSec) autoSec.style.display = "none";
    if (liveSec) liveSec.style.display = "";
    if (matrixContent) matrixContent.classList.remove("collapsed");
  } else if (view === "rules") {
    document.getElementById("vTabRules")?.classList.add("active");
    if (mainGrid) mainGrid.style.display = "none";
    if (autoSec) autoSec.style.display = "";
    if (liveSec) liveSec.style.display = "none";
  }
}

function triggerManualBeacon() {
  playClick();
  sendJson({ cmd: "tx_now" });
}

function resetAllOverrides() {
  playClick();
  sendJson({ cmd: "reset_overrides" });
}

function emergencyStopPumps() {
  playAlert();
  for (let id = 1; id <= 3; id++) {
    sendJson({ cmd: "set_pump_override", id: id, mode: "FORCE_OFF", duration: 0 });
  }
  showToast("E-STOP: All 3 Outflow Pumps Locked OFF!", true);
}

function quickChangeSf(sfVal) {
  playChime();
  const sf = parseInt(sfVal, 10);
  if (isNaN(sf) || sf < 6 || sf > 12) return;
  const sfSelect = document.getElementById("cfgLoraSf");
  if (sfSelect) sfSelect.value = sf;
  // Only send the SF field; firmware keeps all other RF parameters untouched
  sendJson({ cmd: "set_lora_config", sf: sf });
}

function setMode(isAuto) {
  playClick();
  const btnAuto = document.getElementById("btnModeAuto");
  const btnMan = document.getElementById("btnModeMan");
  if (btnAuto) btnAuto.classList.toggle("active", Boolean(isAuto));
  if (btnMan) btnMan.classList.toggle("active", !isAuto);
  sendJson({ cmd: "mode", val: isAuto ? 1 : 0 });
}

function setPumpOverride(id, mode, durationMin = null) {
  playClick();
  if (durationMin === null) {
    const durSelect = document.getElementById(`durP${id}`);
    durationMin = durSelect ? parseInt(durSelect.value, 10) : 0;
  }
  sendJson({
    cmd: "set_pump_override",
    id: id,
    mode: mode,
    duration: durationMin
  });
}

function onDurationChange(id) {
  const durSelect = document.getElementById(`durP${id}`);
  const dur = durSelect ? parseInt(durSelect.value, 10) : 0;
  if (dur > 0) {
    showToast(`P${id} duration set to ${dur} min. Click 'FORCE ON' to start timed run.`, false);
  }
}

function togglePump(id) {
  playClick();
  const t = lastTelemetryObj;
  if (t && t.mode === "AUTO") {
    const isCurrentlyRunning = Boolean(t.pumps?.[id - 1]);
    const durSelect = document.getElementById(`durP${id}`);
    const dur = durSelect ? parseInt(durSelect.value, 10) : 0;
    // In AUTO mode, START/STOP P# directly sets FORCE ON (1) or LOCK OFF (2)
    sendJson({
      cmd: "set_pump_override",
      id: id,
      mode: isCurrentlyRunning ? "FORCE_OFF" : "FORCE_ON",
      duration: isCurrentlyRunning ? 0 : dur
    });
  } else {
    sendJson({ cmd: "pump", id: id });
  }
}

function togglePond() {
  playClick();
  const btnPond = document.getElementById("btnPond");
  if (btnPond) btnPond.innerText = "SYNCING...";
  sendJson({ cmd: "pond" });
}

function updateAutoMask() {
  playClick();
  const m1 = document.getElementById("maskP1").checked ? 1 : 0;
  const m2 = document.getElementById("maskP2").checked ? 1 : 0;
  const m3 = document.getElementById("maskP3").checked ? 1 : 0;
  sendJson({ cmd: "set_auto_mask", mask: [m1, m2, m3] });
}

// ─── LOGGING & EVENT CONSOLE ────────────────────────────────────────
let cachedLogs = [];
let logFilter = 'ALL';

function addLogEntry(e) {
  cachedLogs.push(e);
  if (cachedLogs.length > 100) cachedLogs.shift();
  renderLogs();
}

function filterLogs(cat) {
  logFilter = cat;
  renderLogs();
}

function clearLogs() {
  playAlert();
  sendJson({ cmd: "clear_logs" });
  cachedLogs = [];
  renderLogs();
}

function renderLogs() {
  const box = document.getElementById("logConsoleBox");
  if (!box) return;
  const filtered = (logFilter === 'ALL') ? cachedLogs : cachedLogs.filter(l => l.cat === logFilter);
  box.innerHTML = filtered.map(l => `
    <div class="log-entry">
      <span class="log-ts">${new Date(l.ts * 1000).toLocaleTimeString()}</span>
      <span class="log-cat log-cat-${l.cat}">${l.cat}</span>
      <span class="log-msg">${l.msg}</span>
    </div>
  `).join('');
  box.scrollTop = box.scrollHeight;
}

function saveHydraulicsConfig() {
  playChime();
  const cap = parseInt(document.getElementById("cfgCapacity").value, 10);
  const trig = parseInt(document.getElementById("cfgTrigger").value, 10);
  const cut = parseInt(document.getElementById("cfgCutoff").value, 10);
  const m1 = document.getElementById("cfgMaskP1")?.checked ? 1 : 0;
  const m2 = document.getElementById("cfgMaskP2")?.checked ? 1 : 0;
  const m3 = document.getElementById("cfgMaskP3")?.checked ? 1 : 0;
  const r = parseInt(document.getElementById("cfgRuntime").value, 10);
  const c = parseInt(document.getElementById("cfgCooldown").value, 10);
  const stg = parseInt(document.getElementById("cfgStagger").value, 10);
  const ti = parseInt(document.getElementById("cfgTelemIdle").value, 10);
  const tb = parseInt(document.getElementById("cfgTelemBurst").value, 10);
  const b = parseInt(document.getElementById("cfgBootAuto").value, 10);

  sendJson({
    cmd: "set_hydraulic_config",
    capacity: cap,
    trigger: trig,
    cutoff: cut,
    auto_mask: [m1, m2, m3],
    runtime: r,
    cooldown: c,
    stagger: stg,
    telem_idle: ti,
    telem_burst: tb,
    boot_auto: b
  });
}

function saveLoraConfig() {
  playChime();
  const freq = parseInt(document.getElementById("cfgLoraFreq").value, 10);
  const pwr = parseInt(document.getElementById("cfgLoraPwr").value, 10);
  const sf = parseInt(document.getElementById("cfgLoraSf").value, 10);
  const bw = parseInt(document.getElementById("cfgLoraBw").value, 10);
  const cr = parseInt(document.getElementById("cfgLoraCr").value, 10);
  const ldro = parseInt(document.getElementById("cfgLoraLdro").value, 10);
  const sync = document.getElementById("cfgLoraSync").value.trim();
  const lbtRetries = parseInt(document.getElementById("cfgLbtRetries").value, 10);
  const lbtBackoff = parseInt(document.getElementById("cfgLbtBackoff").value, 10);
  sendJson({
    cmd: "set_lora_config",
    freq: freq,
    power: pwr,
    sf: sf,
    bw: bw,
    cr: cr,
    ldro: isNaN(ldro) ? 0 : ldro,
    sync: sync,
    lbt_retries: isNaN(lbtRetries) ? 4 : lbtRetries,
    lbt_backoff: isNaN(lbtBackoff) ? 60 : lbtBackoff
  });
}

function saveWifiConfig() {
  playChime();
  const ssid = document.getElementById("cfgStaSsid").value.trim();
  const pass = document.getElementById("cfgStaPass").value.trim();
  const a = document.getElementById("cfgAlwaysOn").checked ? 1 : 0;
  sendJson({
    cmd: "set_wifi",
    ssid: ssid,
    pass: pass,
    always_on: a
  });
  showToast("Wi-Fi Credentials Saved!", false);
}

function uploadOtaFirmware() {
  const fileInput = document.getElementById("otaFileInput");
  if (!fileInput.files || fileInput.files.length === 0) {
    alert("Please select a .bin firmware file first.");
    return;
  }
  const file = fileInput.files[0];
  const formData = new FormData();
  formData.append("update", file);

  const progBar = document.getElementById("otaProgressBar");
  const progFill = document.getElementById("otaProgressFill");
  const otaStatus = document.getElementById("otaUploadStatus");

  progBar.style.display = "block";
  progFill.style.width = "0%";
  otaStatus.innerText = "Uploading firmware...";

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/update", true);

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      progFill.style.width = pct + "%";
      otaStatus.innerText = `Uploading: ${pct}% (${Math.round(e.loaded/1024)} KB / ${Math.round(e.total/1024)} KB)`;
    }
  };

  xhr.onload = () => {
    if (xhr.status === 200) {
      otaStatus.innerText = "Flashing complete! Device rebooting... Reloading in 10s.";
      progFill.style.background = "var(--green)";
      setTimeout(() => { location.reload(); }, 10000);
    } else {
      otaStatus.innerText = "Update failed: " + xhr.responseText;
      progFill.style.background = "var(--red)";
    }
  };

  xhr.onerror = () => {
    otaStatus.innerText = "Network error during upload.";
    progFill.style.background = "var(--red)";
  };

  xhr.send(formData);
}

function restoreDefaults() {
  if (confirm("Restore factory default parameters?")) {
    playAlert();
    sendJson({ cmd: "defaults" });
  }
}

function rebootDevice() {
  if (confirm("Reboot ESP32-S3 now?")) {
    playAlert();
    sendJson({ cmd: "reboot" });
  }
}

// ─── MODAL & UTILITY FUNCTIONS ─────────────────────────────────────
function openSettingsModal() {
  playClick();
  document.getElementById("settingsModal").classList.add("open");
  sendJson({ cmd: "get_config" });
  sendJson({ cmd: "get_logs" });
}

function closeSettingsModal() {
  playClick();
  document.getElementById("settingsModal").classList.remove("open");
}

function showToast(msg, isErr = false) {
  const container = document.getElementById("toastContainer");
  const toast = document.createElement("div");
  toast.className = isErr ? "toast error" : "toast";
  toast.innerText = msg;
  container.appendChild(toast);
  setTimeout(() => {
    toast.style.opacity = "0";
    toast.style.transition = "opacity 0.4s";
    setTimeout(() => toast.remove(), 400);
  }, 3500);
}

function formatUptime(seconds) {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

// Audio Toggle Button & Settings Tabs Init
document.addEventListener("DOMContentLoaded", () => {
  const btnAudio = document.getElementById("btnAudioToggle");
  const savedAudio = localStorage.getItem("tank_audio");
  if (savedAudio !== null) soundEnabled = (savedAudio === "true");
  btnAudio.innerText = soundEnabled ? "🔊" : "🔇";

  btnAudio.addEventListener("click", () => {
    soundEnabled = !soundEnabled;
    localStorage.setItem("tank_audio", soundEnabled);
    btnAudio.innerText = soundEnabled ? "🔊" : "🔇";
    if (soundEnabled) playClick();
  });

  document.getElementById("btnSettingsOpen").addEventListener("click", openSettingsModal);
  document.getElementById("btnSettingsClose").addEventListener("click", closeSettingsModal);

  // Tab switching inside settings modal
  document.querySelectorAll(".settings-tab-btn").forEach(btn => {
    btn.addEventListener("click", () => {
      document.querySelectorAll(".settings-tab-btn").forEach(b => b.classList.remove("active"));
      document.querySelectorAll(".settings-tab-pane").forEach(p => p.classList.remove("active"));
      btn.classList.add("active");
      const target = btn.getAttribute("data-tab");
      const pane = document.getElementById(target);
      if (pane) pane.classList.add("active");
    });
  });

  // Close modal when clicking outside card
  document.getElementById("settingsModal").addEventListener("click", (e) => {
    if (e.target.id === "settingsModal") closeSettingsModal();
  });

  connectWebSocket();
});

// ─── TOGGLE LIVE MATRIX PANEL ──────────────────────────────────────
let lastTelemetryObj = null;
let prevTelemetryData = null;

function toggleMatrix() {
  playClick();
  const content = document.getElementById("matrixContent");
  const btn = document.getElementById("btnToggleMatrix");
  const isCollapsed = content.classList.toggle("collapsed");
  btn.innerText = isCollapsed ? "EXPAND" : "COLLAPSE";
}

function copyTelemetryJson() {
  if (!lastTelemetryObj) {
    showToast("No telemetry data yet!", true);
    return;
  }
  const txt = JSON.stringify(lastTelemetryObj, null, 2);
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(txt).then(() => {
      showToast("Telemetry JSON copied to clipboard!", false);
      playClick();
    }).catch(() => {
      showToast("Clipboard copy failed", true);
    });
  } else {
    showToast("Clipboard not supported in browser", true);
  }
}

// ─── MATRIX RENDERING ──────────────────────────────────────────────
function renderMatrix(t) {
  lastTelemetryObj = t;

  // 0. Delta Change Detection
  const deltaTicker = document.getElementById("mDeltaTicker");
  if (deltaTicker && prevTelemetryData) {
    const changes = [];
    if (t.wl !== prevTelemetryData.wl) changes.push(`WL: ${prevTelemetryData.wl} ➔ ${t.wl}`);
    if (t.mode !== prevTelemetryData.mode) changes.push(`Mode: ${t.mode}`);
    if (t.pumps) {
      for (let i = 0; i < 3; i++) {
        if (Boolean(t.pumps[i]) !== Boolean(prevTelemetryData.pumps?.[i])) {
          changes.push(`P${i+1}: ${t.pumps[i] ? 'ON' : 'OFF'}`);
        }
      }
    }
    if (t.demand !== prevTelemetryData.demand) {
      changes.push(`Pond Refill: ${t.demand ? 'ACTIVE' : 'IDLE'}`);
    }

    if (changes.length > 0) {
      deltaTicker.innerText = `DELTA: ${changes.join(" | ")}`;
      deltaTicker.classList.add("changed");
      setTimeout(() => deltaTicker.classList.remove("changed"), 2500);
    }
  }
  prevTelemetryData = { ...t, pumps: [...(t.pumps || [])] };

  // 1. Physical Inputs (PCF8574)
  if (t.pcf_raw !== undefined) {
    const rawByte = t.pcf_raw;
    const hex = "0x" + rawByte.toString(16).toUpperCase().padStart(2, '0');
    const bin = "0b" + rawByte.toString(2).padStart(8, '0');
    const badge = document.getElementById("mPcfRaw");
    if (badge) badge.innerText = `${hex} (${bin})`;

    // Float bits: 0 = submerged, 1 = dry
    if (t.floats) {
      const fNames = ["mFloat1", "mFloat2", "mFloat3"];
      const chips = ["chipP0", "chipP1", "chipP2"];
      t.floats.forEach((f, idx) => {
        const isSub = (f === 0);
        const el = document.getElementById(fNames[idx]);
        if (el) {
          el.innerHTML = isSub 
            ? `<span class="badge-tag submerged">SUBMERGED (0)</span>`
            : `<span class="badge-tag idle">DRY (1)</span>`;
        }
        const chip = document.getElementById(chips[idx]);
        if (chip) chip.classList.toggle("active", isSub);
      });
    }

    // Button bits: 0 = pressed, 1 = idle
    if (t.buttons) {
      const bNames = ["mBtnMode", "mBtnP1", "mBtnP2", "mBtnP3", "mBtnPond"];
      const chips = ["chipP3", "chipP4", "chipP5", "chipP6", "chipP7"];
      t.buttons.forEach((b, idx) => {
        const isPressed = (b === 0);
        const el = document.getElementById(bNames[idx]);
        if (el) {
          el.innerHTML = isPressed 
            ? `<span class="badge-tag pressed">PRESSED (0)</span>`
            : `<span class="badge-tag idle">IDLE (1)</span>`;
        }
        const chip = document.getElementById(chips[idx]);
        if (chip) chip.classList.toggle("pressed", isPressed);
      });
    }
  }

  // Dual Segmented Level Bars
  const rawLvl = (t.raw_wl !== undefined) ? t.raw_wl : t.wl;
  const stableLvl = t.wl;
  const lvlLabels = ["L0 (EMPTY)", "L1 (LOW 33%)", "L2 (MID 66%)", "L3 (FULL 100%)"];

  for (let i = 0; i < 4; i++) {
    const rSeg = document.getElementById(`segRaw${i}`);
    if (rSeg) rSeg.classList.toggle("active", i <= rawLvl && rawLvl > 0);
    const sSeg = document.getElementById(`segStb${i}`);
    if (sSeg) sSeg.classList.toggle("active", i <= stableLvl && stableLvl > 0);
  }
  const valRaw = document.getElementById("valRawLvl");
  if (valRaw) valRaw.innerText = lvlLabels[rawLvl] || `L${rawLvl}`;
  const valStb = document.getElementById("valStableLvl");
  if (valStb) valStb.innerText = lvlLabels[stableLvl] || `L${stableLvl}`;

  // Plausibility & Debounced Level Table Row
  const plausEl = document.getElementById("mPlausibility");
  if (plausEl) {
    if (t.err === 4) {
      plausEl.innerHTML = `<span class="badge-tag fault">PLAUSIBILITY FAULT</span>`;
    } else if (t.err === 2) {
      plausEl.innerHTML = `<span class="badge-tag fault">I²C DISCONNECTED</span>`;
    } else if (t.err === 3) {
      plausEl.innerHTML = `<span class="badge-tag warn">GATEWAY TIMEOUT</span>`;
    } else {
      plausEl.innerHTML = `<span class="badge-tag ok">VALID (L:${t.wl})</span>`;
    }
  }

  // 2. Physical Actuators & Relay Safety Locks
  const minRunMs = t.cfg_runtime || 20000;
  const minCdMs = t.cfg_cooldown || 20000;

  for (let i = 0; i < 3; i++) {
    const pNum = i + 1;
    const isOn = Boolean(t.pumps?.[i]);
    const rLeft = t.run_left?.[i] || 0;
    const cLeft = t.cd_left?.[i] || 0;

    // Pin level text in table
    const rRow = document.getElementById(`mRelay${pNum}`);
    if (rRow) {
      rRow.innerHTML = isOn
        ? `<span class="badge-tag running">LOW (ENERGIZED / PUMPING)</span>`
        : `<span class="badge-tag idle">HIGH (DE-ENERGIZED / IDLE)</span>`;
    }

    // Monitor card
    const stBadge = document.getElementById(`mRelay${pNum}State`);
    if (stBadge) {
      stBadge.innerText = isOn ? "RUNNING" : "OFF";
      stBadge.className = isOn ? "lock-state running" : "lock-state";
    }

    const bar = document.getElementById(`lockBarP${pNum}`);
    const tmr = document.getElementById(`mLockTimerP${pNum}`);

    if (isOn && rLeft > 0) {
      const pct = Math.round((rLeft / (minRunMs / 1000)) * 100);
      if (bar) {
        bar.style.width = `${pct}%`;
        bar.style.background = "var(--cyan)";
      }
      if (tmr) {
        tmr.innerText = `Min Runtime Lock: ${rLeft}s remaining`;
        tmr.className = "lock-timer active";
      }
    } else if (!isOn && cLeft > 0) {
      const pct = Math.round((cLeft / (minCdMs / 1000)) * 100);
      if (bar) {
        bar.style.width = `${pct}%`;
        bar.style.background = "var(--amber)";
      }
      if (tmr) {
        tmr.innerText = `Cooldown Lock: ${cLeft}s remaining`;
        tmr.className = "lock-timer active";
      }
    } else {
      if (bar) bar.style.width = "0%";
      if (tmr) {
        tmr.innerText = "No active lock (Ready)";
        tmr.className = "lock-timer";
      }
    }
  }

  const pondDemandEl = document.getElementById("mPondDemand");
  if (pondDemandEl) {
    pondDemandEl.innerHTML = t.demand 
      ? `<span class="badge-tag active">ACTIVE REFILL DEMAND</span>`
      : `<span class="badge-tag idle">IDLE</span>`;
  }

  // Lifetime Runtimes
  const lifetimeEl = document.getElementById("mLifetime");
  if (lifetimeEl && t.rt_stats) {
    lifetimeEl.innerText = `P1:${t.rt_stats[0]}s P2:${t.rt_stats[1]}s P3:${t.rt_stats[2]}s`;
  }

  // 3. Radio & Sensor Metrics
  const radioHealthEl = document.getElementById("mRadioHealth");
  if (radioHealthEl) {
    radioHealthEl.innerHTML = (t.radio_ok !== 0 && t.err !== 1)
      ? `<span class="badge-tag ok">ONLINE (16 MHz SPI)</span>`
      : `<span class="badge-tag fault">RADIO FAULT</span>`;
  }

  // Signal Strip
  const rssiVal = document.getElementById("mRssiVal");
  if (rssiVal) rssiVal.innerText = `${t.rssi} dBm`;
  const rssiBar = document.getElementById("mRssiBar");
  if (rssiBar) {
    const pct = Math.min(100, Math.max(5, (t.rssi + 130) * (100 / 90)));
    rssiBar.style.width = `${pct}%`;
  }

  const snrVal = document.getElementById("mSnrVal");
  if (snrVal) snrVal.innerText = `${t.snr} dB`;
  const snrRating = document.getElementById("mSnrRating");
  if (snrRating) {
    if (t.snr >= 6) {
      snrRating.innerText = "EXCELLENT";
      snrRating.className = "badge-tag ok";
    } else if (t.snr >= 0) {
      snrRating.innerText = "GOOD";
      snrRating.className = "badge-tag active";
    } else {
      snrRating.innerText = "WEAK";
      snrRating.className = "badge-tag fault";
    }
  }

  const tempEl = document.getElementById("mTemp");
  if (tempEl && t.temp !== undefined) {
    tempEl.innerText = `${t.temp.toFixed(1)} °C`;
  }

  const humEl = document.getElementById("mHum");
  if (humEl && t.hum !== undefined) {
    humEl.innerText = `${t.hum} % RH`;
  }

  // RF Modulation, ToA & LBT CAD Matrix Fields
  const rfModEl = document.getElementById("mRfModulation");
  if (rfModEl && t.lora_sf !== undefined) {
    const bwKhz = (t.lora_bw > 1000) ? Math.round(t.lora_bw / 1000) : (t.lora_bw || 125);
    const pwr = (t.lora_power !== undefined) ? t.lora_power : (t.lora_pwr !== undefined ? t.lora_pwr : 22);
    rfModEl.innerText = `SF${t.lora_sf} | ${bwKhz}kHz | 4/${t.lora_cr || 5} | ${pwr >= 0 ? "+" : ""}${pwr}dBm`;
  }

  const rfToaEl = document.getElementById("mRfToaLdro");
  if (rfToaEl && t.toa_ms !== undefined) {
    const ldroTxt = t.ldro_active ? "LDRO:ON" : "LDRO:OFF";
    rfToaEl.innerHTML = `<span class="badge-tag ${t.ldro_active ? 'ok' : 'idle'}">${ldroTxt}</span> ${t.toa_ms}ms (Guard: ${t.guard_ms}ms)`;
  }

  const rfCadEl = document.getElementById("mRfCadLbt");
  if (rfCadEl) {
    const defers = (t.cad_busy_cnt !== undefined) ? t.cad_busy_cnt : (t.cad_defers || 0);
    const lastBo = t.last_cad_backoff || 0;
    rfCadEl.innerText = `4-Symb | Retries: ${t.lbt_retries ?? 4} | Deferrals: ${defers} | Backoff: ${lastBo}ms`;
  }

  const telemModeEl = document.getElementById("mTelemRateMode");
  if (telemModeEl) {
    const burst = Boolean(t.burst_mode);
    const activeMs = t.cfg_telem || (burst ? t.cfg_telem_burst : t.cfg_telem_idle) || 3000;
    telemModeEl.innerHTML = burst
      ? `<span class="badge-tag active">EVENT BURST (${(activeMs / 1000).toFixed(1)}s)</span>`
      : `<span class="badge-tag ok">SILENT IDLE (${Math.round(activeMs / 1000)}s)</span>`;
  }

  const pktStatsEl = document.getElementById("mRfPktStats");
  if (pktStatsEl && t.tx_cnt !== undefined) {
    const txAgo = (t.last_tx_ago >= 0) ? `${t.last_tx_ago}s ago` : "never";
    const rxAgo = (t.last_rx_ago >= 0) ? `${t.last_rx_ago}s ago` : "never";
    pktStatsEl.innerText = `TX: ${t.tx_cnt} (${txAgo}) • RX: ${t.rx_cnt || 0} (${rxAgo})`;
  }

  // 4. MCU & Network
  const freeBytes = t.heap || 0;
  const freeKb = Math.round(freeBytes / 1024);
  const totalSram = 327680;
  const freePct = Math.min(100, Math.round((freeBytes / totalSram) * 100));

  const heapPct = document.getElementById("mHeapPercent");
  if (heapPct) heapPct.innerText = `${freeKb} KB (${freePct}% Free)`;
  const heapFill = document.getElementById("mHeapFill");
  if (heapFill) heapFill.style.width = `${freePct}%`;

  const heapEl = document.getElementById("mMcuHeap");
  if (heapEl) {
    const minKb = Math.round((t.min_heap || 0) / 1024);
    heapEl.innerText = `${freeKb} KB (Min: ${minKb} KB)`;
  }

  const uptimeEl = document.getElementById("mMcuUptime");
  if (uptimeEl) {
    uptimeEl.innerText = `#${t.boots || 0} | ${formatUptime(t.uptime || 0)}`;
  }

  const softApEl = document.getElementById("mSoftApInfo");
  if (softApEl) {
    softApEl.innerText = `192.168.4.1 (${t.ap_clients || 0} clients)`;
  }

  const staEl = document.getElementById("mStaInfo");
  if (staEl) {
    if (t.sta && t.sta_ip) {
      const sig = (t.sta_rssi) ? ` (${t.sta_rssi} dBm)` : "";
      staEl.innerHTML = `<span class="badge-tag ok">${t.sta_ip}${sig}</span>`;
    } else {
      staEl.innerHTML = `<span class="badge-tag idle">Disconnected</span>`;
    }
  }

  // NVS Timers & Auto Masks
  const nvsTimersEl = document.getElementById("mNvsTimers");
  if (nvsTimersEl && t.cfg_runtime) {
    const stgS = (t.cfg_stagger || 3000) / 1000;
    const idleS = (t.cfg_telem_idle || t.cfg_telem || 600000) / 1000;
    const burstS = (t.cfg_telem_burst || 3000) / 1000;
    nvsTimersEl.innerText = `RT:${t.cfg_runtime/1000}s CD:${t.cfg_cooldown/1000}s STG:${stgS}s | Idle:${idleS}s Burst:${burstS}s`;
  }

  const masksEl = document.getElementById("mAutoMasks");
  if (masksEl && t.auto_mask) {
    masksEl.innerText = `[${t.auto_mask.join(", ")}]`;
  }
}

