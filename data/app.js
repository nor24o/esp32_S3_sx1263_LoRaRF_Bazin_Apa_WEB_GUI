// Tank Node WebSocket & Dashboard Controller
let ws = null;
let reconnectTimer = null;

// DOM Elements
const wsStatus = document.getElementById("wsStatus");
const uptimeBadge = document.getElementById("uptimeBadge");
const btnToggleMode = document.getElementById("btnToggleMode");
const errorBanner = document.getElementById("errorBanner");
const errorMessage = document.getElementById("errorMessage");

const waterFill = document.getElementById("waterFill");
const tankLevelText = document.getElementById("tankLevelText");
const markerF1 = document.getElementById("markerF1").querySelector(".float-dot");
const markerF2 = document.getElementById("markerF2").querySelector(".float-dot");
const markerF3 = document.getElementById("markerF3").querySelector(".float-dot");

const demandStatus = document.getElementById("demandStatus");
const pondPumpStatus = document.getElementById("pondPumpStatus");
const btnTogglePond = document.getElementById("btnTogglePond");

const p1StatePill = document.getElementById("p1StatePill");
const p2StatePill = document.getElementById("p2StatePill");
const p3StatePill = document.getElementById("p3StatePill");

const chkAutoP1 = document.getElementById("chkAutoP1");
const chkAutoP2 = document.getElementById("chkAutoP2");
const chkAutoP3 = document.getElementById("chkAutoP3");

const btnP1Toggle = document.getElementById("btnP1Toggle");
const btnP2Toggle = document.getElementById("btnP2Toggle");
const btnP3Toggle = document.getElementById("btnP3Toggle");

const valRssi = document.getElementById("valRssi");
const valSnr = document.getElementById("valSnr");
const valGwAgo = document.getElementById("valGwAgo");
const valHeap = document.getElementById("valHeap");
const valBoots = document.getElementById("valBoots");
const valPcf = document.getElementById("valPcf");

const cfgRuntime = document.getElementById("cfgRuntime");
const rngRuntime = document.getElementById("rngRuntime");
const cfgCooldown = document.getElementById("cfgCooldown");
const rngCooldown = document.getElementById("rngCooldown");
const cfgTelem = document.getElementById("cfgTelem");
const rngTelem = document.getElementById("rngTelem");
const cfgBootAuto = document.getElementById("cfgBootAuto");

const btnSaveCfg = document.getElementById("btnSaveCfg");
const btnReloadCfg = document.getElementById("btnReloadCfg");
const btnDefaultsCfg = document.getElementById("btnDefaultsCfg");
const btnReboot = document.getElementById("btnReboot");
const toast = document.getElementById("toast");

// Toast helper
function showToast(msg, isErr = false) {
  toast.innerText = msg;
  toast.style.borderColor = isErr ? "#ef4444" : "#10b981";
  toast.classList.add("show");
  setTimeout(() => toast.classList.remove("show"), 2500);
}

// Synchronize inputs with range sliders
function syncRangeNumber(rangeElem, numElem) {
  rangeElem.addEventListener("input", () => { numElem.value = rangeElem.value; });
  numElem.addEventListener("input", () => { rangeElem.value = numElem.value; });
}
syncRangeNumber(rngRuntime, cfgRuntime);
syncRangeNumber(rngCooldown, cfgCooldown);
syncRangeNumber(rngTelem, cfgTelem);

// WebSocket Initialization
function connectWebSocket() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

  const wsUrl = `ws://${window.location.hostname || "192.168.4.1"}:81/`;
  ws = new WebSocket(wsUrl);

  ws.onopen = () => {
    wsStatus.className = "badge badge-green";
    wsStatus.innerText = "Connected";
    if (reconnectTimer) clearInterval(reconnectTimer);
    // Request initial config
    sendCmd({ cmd: "get_config" });
  };

  ws.onclose = () => {
    wsStatus.className = "badge badge-red";
    wsStatus.innerText = "Disconnected";
    if (!reconnectTimer) {
      reconnectTimer = setInterval(connectWebSocket, 2000);
    }
  };

  ws.onerror = () => {
    ws.close();
  };

  ws.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);
      if (data.type === "telemetry") {
        updateDashboard(data);
      } else if (data.type === "config") {
        updateConfigForm(data);
      } else if (data.type === "toast") {
        showToast(data.msg, data.is_err);
      }
    } catch (err) {
      console.error("WS parse error:", err);
    }
  };
}

function sendCmd(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  } else {
    showToast("WebSocket disconnected", true);
  }
}

// Update Dashboard View
function updateDashboard(d) {
  // Uptime
  uptimeBadge.innerText = `Up: ${d.uptime}s`;

  // Mode
  const isAuto = d.mode === "AUTO";
  btnToggleMode.innerText = `Mode: ${d.mode}`;
  btnToggleMode.className = `btn btn-mode ${isAuto ? "badge-green" : "badge-blue"}`;

  // Water Level (0 to 3)
  const wlPercent = (d.wl / 3) * 100;
  waterFill.style.height = `${wlPercent}%`;
  tankLevelText.innerText = `Level ${d.wl}/3`;

  // Float markers
  markerF1.className = `float-dot ${d.wl >= 1 ? "active" : ""}`;
  markerF2.className = `float-dot ${d.wl >= 2 ? "active" : ""}`;
  markerF3.className = `float-dot ${d.wl >= 3 ? "active" : ""}`;

  // Error Banner
  if (d.err > 0) {
    errorBanner.style.display = "flex";
    errorMessage.innerText = `Error [${d.err}]: ${d.err_str}`;
  } else {
    errorBanner.style.display = "none";
  }

  // Pond Charging Latch & State
  demandStatus.innerText = d.demand ? "CHARGING ACTIVE" : "SATISFIED";
  demandStatus.className = `val val-pill ${d.demand ? "pill-active" : "pill-off"}`;

  pondPumpStatus.innerText = d.pond ? "RUNNING" : "OFF";
  pondPumpStatus.className = `val val-pill ${d.pond ? "pill-on" : "pill-off"}`;

  // Outflow Pumps
  updatePumpRow(p1StatePill, d.pumps[0], chkAutoP1, d.auto_mask[0]);
  updatePumpRow(p2StatePill, d.pumps[1], chkAutoP2, d.auto_mask[1]);
  updatePumpRow(p3StatePill, d.pumps[2], chkAutoP3, d.auto_mask[2]);

  // RF & System Metrics
  valRssi.innerText = `${d.rssi} dBm`;
  valSnr.innerText = `${d.snr} dB`;
  valGwAgo.innerText = d.gw_seen ? `${d.gw_ago}s ago` : "Never";
  valHeap.innerText = `${Math.round(d.heap / 1024)} KB`;
  valBoots.innerText = `#${d.boots}`;
  valPcf.innerText = d.pcf_ok ? "Connected" : "I2C Fault";
  valPcf.className = `m-val ${d.pcf_ok ? "text-green" : "badge-red"}`;
}

function updatePumpRow(pill, isOn, chk, isArmed) {
  pill.innerText = isOn ? "RUNNING" : "OFF";
  pill.className = `val-pill ${isOn ? "pill-on" : "pill-off"}`;
  if (document.activeElement !== chk) {
    chk.checked = isArmed;
  }
}

// Update Config Form Values
function updateConfigForm(c) {
  cfgRuntime.value = c.runtime;
  rngRuntime.value = c.runtime;
  cfgCooldown.value = c.cooldown;
  rngCooldown.value = c.cooldown;
  cfgTelem.value = c.telem;
  rngTelem.value = c.telem;
  cfgBootAuto.value = c.boot_auto ? "1" : "0";
}

// Event Listeners
btnToggleMode.addEventListener("click", () => {
  sendCmd({ cmd: "mode", val: "toggle" });
});

btnTogglePond.addEventListener("click", () => {
  sendCmd({ cmd: "pond", val: "toggle" });
});

btnP1Toggle.addEventListener("click", () => { sendCmd({ cmd: "pump", id: 1, val: "toggle" }); });
btnP2Toggle.addEventListener("click", () => { sendCmd({ cmd: "pump", id: 2, val: "toggle" }); });
btnP3Toggle.addEventListener("click", () => { sendCmd({ cmd: "pump", id: 3, val: "toggle" }); });

function updateAutoMask() {
  sendCmd({
    cmd: "set_auto_mask",
    mask: [chkAutoP1.checked ? 1 : 0, chkAutoP2.checked ? 1 : 0, chkAutoP3.checked ? 1 : 0]
  });
}
chkAutoP1.addEventListener("change", updateAutoMask);
chkAutoP2.addEventListener("change", updateAutoMask);
chkAutoP3.addEventListener("change", updateAutoMask);

btnSaveCfg.addEventListener("click", () => {
  const payload = {
    cmd: "set_config",
    runtime: parseInt(cfgRuntime.value, 10),
    cooldown: parseInt(cfgCooldown.value, 10),
    telem: parseInt(cfgTelem.value, 10),
    boot_auto: parseInt(cfgBootAuto.value, 10)
  };
  sendCmd(payload);
  showToast("Configuration Applied & Saved to Flash");
});

btnReloadCfg.addEventListener("click", () => {
  sendCmd({ cmd: "get_config" });
  showToast("Config Refreshed");
});

btnDefaultsCfg.addEventListener("click", () => {
  if (confirm("Reset runtime parameters to factory defaults?")) {
    sendCmd({ cmd: "defaults" });
    showToast("Reset to Defaults");
  }
});

btnReboot.addEventListener("click", () => {
  if (confirm("Flushing NVS and restarting Tank Node?")) {
    sendCmd({ cmd: "reboot" });
    showToast("Rebooting device...");
  }
});

// Start connection on load
window.addEventListener("DOMContentLoaded", connectWebSocket);
