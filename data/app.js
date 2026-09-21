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

function renderTelemetry(t) {
  // 1. Water Level Height & Text
  const heights = [0, 33, 66, 100];
  const labels = ["WL: 0 (EMPTY 0%)", "WL: 1 (LOW 33%)", "WL: 2 (MID 66%)", "WL: 3 (FULL 100%)"];
  const wl = Math.min(Math.max(t.wl, 0), 3);

  const liquidBody = document.getElementById("liquidBody");
  liquidBody.style.height = `${heights[wl]}%`;
  document.getElementById("tankLevelText").innerText = labels[wl];

  // 2. Float Sensor Buoys (L1, L2, L3)
  const b1 = document.getElementById("beadL1");
  const b2 = document.getElementById("beadL2");
  const b3 = document.getElementById("beadL3");

  b1.classList.toggle("submerged", wl >= 1);
  b2.classList.toggle("submerged", wl >= 2);
  b3.classList.toggle("submerged", wl >= 3);

  // Sensor Plausibility Alert Check
  const alertBar = document.getElementById("sensorAlertBar");
  const healthText = document.getElementById("sensorHealthText");

  if (t.err === 4) { // ERR_FLOAT_PLAUSIBILITY
    b1.classList.add("fault");
    b2.classList.add("fault");
    b3.classList.add("fault");
    alertBar.classList.add("alert");
    healthText.innerText = "SENSOR FAULT: Float switch plausibility violation!";
    if (lastErrorCode !== 4) playAlert();
  } else if (t.err === 2) { // ERR_PCF_I2C
    alertBar.classList.add("alert");
    healthText.innerText = "HARDWARE FAULT: PCF8574 I²C Bus Disconnected!";
    if (lastErrorCode !== 2) playAlert();
  } else {
    b1.classList.remove("fault");
    b2.classList.remove("fault");
    b3.classList.remove("fault");
    alertBar.classList.remove("alert");
    healthText.innerText = "Float switches healthy & plausible";
  }
  lastErrorCode = t.err;

  // 3. Inflow Pipe (From Remote Pond)
  const inflowWater = document.getElementById("inflowPipeWater");
  const waterJet = document.getElementById("waterJet");
  const pondActive = Boolean(t.pond);

  inflowWater.classList.toggle("active", pondActive);
  waterJet.classList.toggle("active", pondActive);

  // Pond Controller Card
  const indPond = document.getElementById("indPond");
  const btnPond = document.getElementById("btnPond");
  const latchBadge = document.getElementById("latchBadge");

  indPond.classList.toggle("active", pondActive);
  btnPond.innerText = pondActive ? "STOP POND" : "START POND";
  latchBadge.innerText = t.demand ? "CHARGING LATCH: ON" : "LATCH: IDLE";
  latchBadge.classList.toggle("active", Boolean(t.demand));

  // 4. Outflow Pipes & Pump Cards (P1, P2, P3)
  const isAuto = (t.mode === "AUTO");

  for (let i = 0; i < 3; i++) {
    const pNum = i + 1;
    const pState = Boolean(t.pumps[i]);
    const mask = Boolean(t.auto_mask[i]);

    // Turbines & Streams
    document.getElementById(`turbineP${pNum}`).classList.toggle("active", pState);
    document.getElementById(`streamP${pNum}`).classList.toggle("active", pState);

    // Indicators & Pills
    document.getElementById(`indP${pNum}`).classList.toggle("active", pState);
    const pill = document.getElementById(`pillP${pNum}`);
    const btn = document.getElementById(`btnP${pNum}`);
    const chk = document.getElementById(`maskP${pNum}`);

    if (pState) {
      pill.innerText = "RUNNING";
      pill.className = "status-pill running";
      btn.innerText = `STOP P${pNum}`;
    } else {
      pill.innerText = "IDLE / READY";
      pill.className = "status-pill";
      btn.innerText = `START P${pNum}`;
    }

    if (chk) chk.checked = mask;
    btn.disabled = isAuto; // Manual toggle disabled during Auto Mode
  }

  // 5. Operation Mode UI
  const btnAuto = document.getElementById("btnModeAuto");
  const btnMan = document.getElementById("btnModeMan");
  const modeBanner = document.getElementById("modeBanner");
  const modeHint = document.getElementById("modeHint");

  btnAuto.classList.toggle("active", isAuto);
  btnMan.classList.toggle("active", !isAuto);
  modeBanner.innerText = isAuto ? "AUTOMATIC MODE" : "MANUAL MODE";
  modeBanner.className = isAuto ? "mode-status-text auto" : "mode-status-text";
  modeHint.innerText = isAuto
    ? "Auto-irrigation active. Outflow pumps governed by water capacity & selection mask."
    : "Manual overrides active. Anti-chatter cooldown protection enforced.";

  // 6. HUD Badges & Diagnostics
  document.getElementById("badgeRssi").innerText = `RSSI: ${t.rssi} dBm`;
  document.getElementById("badgeUptime").innerText = `Up: ${formatUptime(t.uptime)}`;

  if (t.sta && t.sta_ip) {
    document.getElementById("badgeConn").innerText = `STA: ${t.sta_ip}`;
  } else {
    document.getElementById("badgeConn").innerText = "SoftAP (192.168.4.1)";
  }

  document.getElementById("diagPcf").innerText = t.pcf_ok ? "ONLINE" : "FAULT";
  document.getElementById("diagPcf").style.color = t.pcf_ok ? "var(--green)" : "var(--red)";
  document.getElementById("diagRssi").innerText = `${t.rssi} dBm`;
  document.getElementById("diagSnr").innerText = `${t.snr} dB`;
  document.getElementById("diagHeap").innerText = `${Math.round(t.heap / 1024)} KB`;
  document.getElementById("diagBoots").innerText = `#${t.boots}`;

  // 7. Update Sparkline Canvas
  updateSparkline(wl, t.rssi);
}

// ─── CONFIG RENDERING ──────────────────────────────────────────────
function renderConfig(c) {
  const rInput = document.getElementById("cfgRuntime");
  const cInput = document.getElementById("cfgCooldown");
  const tInput = document.getElementById("cfgTelem");
  const bSelect = document.getElementById("cfgBootAuto");
  const aCheck = document.getElementById("cfgAlwaysOn");
  const sInput = document.getElementById("cfgStaSsid");

  if (rInput) rInput.value = c.runtime;
  if (cInput) cInput.value = c.cooldown;
  if (tInput) tInput.value = c.telem;
  if (bSelect) bSelect.value = c.boot_auto;
  if (aCheck) aCheck.checked = Boolean(c.always_on);
  if (sInput && c.sta_ssid) sInput.value = c.sta_ssid;
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
  ctx.strokeStyle = "#00f0ff";
  ctx.lineWidth = 2.5;
  ctx.shadowColor = "rgba(0, 240, 255, 0.5)";
  ctx.shadowBlur = 8;

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
  ctx.strokeStyle = "#ff00ff";
  ctx.lineWidth = 1.5;
  ctx.shadowColor = "rgba(255, 0, 255, 0.4)";
  ctx.shadowBlur = 6;

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
function setMode(isAuto) {
  playClick();
  sendJson({ cmd: "mode" });
}

function togglePump(id) {
  playClick();
  sendJson({ cmd: "pump", id: id });
}

function togglePond() {
  playClick();
  sendJson({ cmd: "pond" });
}

function updateAutoMask() {
  playClick();
  const m1 = document.getElementById("maskP1").checked ? 1 : 0;
  const m2 = document.getElementById("maskP2").checked ? 1 : 0;
  const m3 = document.getElementById("maskP3").checked ? 1 : 0;
  sendJson({ cmd: "set_auto_mask", mask: [m1, m2, m3] });
}

function saveConfiguration() {
  playChime();
  const r = parseInt(document.getElementById("cfgRuntime").value, 10);
  const c = parseInt(document.getElementById("cfgCooldown").value, 10);
  const t = parseInt(document.getElementById("cfgTelem").value, 10);
  const b = parseInt(document.getElementById("cfgBootAuto").value, 10);
  const a = document.getElementById("cfgAlwaysOn").checked ? 1 : 0;
  const ssid = document.getElementById("cfgStaSsid").value.trim();
  const pass = document.getElementById("cfgStaPass").value.trim();

  // Save runtime timing config
  sendJson({
    cmd: "set_config",
    runtime: r,
    cooldown: c,
    telem: t,
    boot_auto: b,
    always_on: a
  });

  // Save Home Wi-Fi credentials if entered
  sendJson({
    cmd: "set_wifi",
    ssid: ssid,
    pass: pass,
    always_on: a
  });

  closeSettingsModal();
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

// Audio Toggle Button
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

  // Close modal when clicking outside card
  document.getElementById("settingsModal").addEventListener("click", (e) => {
    if (e.target.id === "settingsModal") closeSettingsModal();
  });

  connectWebSocket();
});
