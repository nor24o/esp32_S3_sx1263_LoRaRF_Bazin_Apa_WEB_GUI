#pragma once
#include <Arduino.h>

// Fallback embedded single-file HTML/CSS/JS if LittleFS partition has not been flashed.
static const char PAGE_INDEX_FALLBACK[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tank Node &bull; Reservoir Control</title>
  <style>
    :root { --bg-base:#0b0f19; --bg-card:rgba(26,34,53,0.75); --border-card:rgba(255,255,255,0.08); --text:#f1f5f9; --muted:#94a3b8; --cyan:#06b6d4; --blue:#3b82f6; --green:#10b981; --red:#ef4444; }
    * { box-sizing:border-box; margin:0; padding:0; font-family:sans-serif; }
    body { background:var(--bg-base); color:var(--text); padding:16px; }
    .nav { display:flex; justify-content:space-between; align-items:center; background:var(--bg-card); padding:14px 18px; border-radius:12px; margin-bottom:16px; border:1px solid var(--border-card); }
    .badge { font-size:0.75rem; font-weight:700; padding:4px 10px; border-radius:999px; text-transform:uppercase; }
    .badge-green { background:rgba(16,185,129,0.2); color:#34d399; }
    .badge-red { background:rgba(239,68,68,0.2); color:#f87171; }
    .badge-blue { background:rgba(59,130,246,0.2); color:#60a5fa; }
    .grid { display:grid; grid-template-columns:repeat(auto-fit, minmax(300px, 1fr)); gap:16px; }
    .card { background:var(--bg-card); padding:18px; border-radius:12px; border:1px solid var(--border-card); }
    .card-h { display:flex; justify-content:space-between; margin-bottom:12px; }
    .btn { padding:6px 12px; border-radius:6px; font-weight:600; cursor:pointer; border:none; background:var(--blue); color:#fff; font-size:0.85rem; }
    .btn-green { background:var(--green); }
    .btn-red { background:var(--red); }
    .btn-out { background:transparent; border:1px solid var(--border-card); color:var(--text); }
    .tank-box { width:120px; height:160px; border:3px solid #64748b; border-radius:14px; margin:10px auto; position:relative; overflow:hidden; background:#000; }
    .water { position:absolute; bottom:0; left:0; right:0; background:linear-gradient(180deg, #22d3ee, #0284c7); transition:height 0.5s ease; }
    .row { display:flex; justify-content:space-between; align-items:center; padding:8px 0; border-bottom:1px solid var(--border-card); }
    .mgrid { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
    .mbox { background:rgba(255,255,255,0.03); padding:10px; border-radius:8px; }
    .mlab { font-size:0.7rem; color:var(--muted); text-transform:uppercase; }
    .mval { font-size:1.1rem; font-weight:700; }
    .form-group { margin-bottom:10px; display:flex; flex-direction:column; gap:4px; font-size:0.85rem; }
    input[type=number], select { background:#000; color:#fff; border:1px solid var(--border-card); padding:6px; border-radius:6px; }
    .toast { position:fixed; bottom:20px; right:20px; background:#0f172a; padding:10px 16px; border-radius:8px; border:1px solid #334155; opacity:0; transition:0.3s; }
    .toast.show { opacity:1; }
  </style>
</head>
<body>
  <div class="nav">
    <div><h2>💧 Tank Node</h2><small style="color:var(--muted)">ESP32-S3 &bull; SX1262 LoRa</small></div>
    <div style="display:flex; gap:8px; align-items:center;">
      <span id="wsBadge" class="badge badge-red">Disconnected</span>
      <button id="btnMode" class="btn btn-out">Mode: --</button>
    </div>
  </div>

  <div id="errBox" style="display:none; background:rgba(239,68,68,0.2); border:1px solid #ef4444; padding:10px; border-radius:8px; margin-bottom:16px;">
    <span id="errMsg" style="font-weight:700; color:#fca5a5;">Alert</span>
  </div>

  <div class="grid">
    <div class="card">
      <div class="card-h"><h3>Reservoir Depth</h3><span id="lvlTxt" class="badge badge-blue">Level --</span></div>
      <div class="tank-box"><div id="water" class="water" style="height:0%;"></div></div>
      <div class="row"><span>Pond Refill Latch:</span><b id="demandTxt">--</b></div>
      <div class="row"><span>Pond Pump State:</span><b id="pondTxt">--</b></div>
      <div style="margin-top:10px;"><button id="btnPond" class="btn btn-out" style="width:100%;">Toggle Pond Pump</button></div>
    </div>

    <div class="card">
      <div class="card-h"><h3>Outflow Pumps</h3><span class="badge badge-blue">Parallel</span></div>
      <div class="row"><div><b>Pump 1</b> (<span id="p1Txt">OFF</span>)</div><div style="display:flex; gap:8px;"><label><input type="checkbox" id="chk1"> Auto</label><button id="btnP1" class="btn">Toggle</button></div></div>
      <div class="row"><div><b>Pump 2</b> (<span id="p2Txt">OFF</span>)</div><div style="display:flex; gap:8px;"><label><input type="checkbox" id="chk2"> Auto</label><button id="btnP2" class="btn">Toggle</button></div></div>
      <div class="row"><div><b>Pump 3</b> (<span id="p3Txt">OFF</span>)</div><div style="display:flex; gap:8px;"><label><input type="checkbox" id="chk3"> Auto</label><button id="btnP3" class="btn">Toggle</button></div></div>
    </div>

    <div class="card">
      <div class="card-h"><h3>Diagnostics</h3><span class="badge badge-green">868.1 MHz</span></div>
      <div class="mgrid">
        <div class="mbox"><div class="mlab">LoRa RSSI</div><div id="rssi" class="mval">--</div></div>
        <div class="mbox"><div class="mlab">LoRa SNR</div><div id="snr" class="mval">--</div></div>
        <div class="mbox"><div class="mlab">Uptime</div><div id="up" class="mval">--</div></div>
        <div class="mbox"><div class="mlab">Gateway Ping</div><div id="gw" class="mval">--</div></div>
      </div>
    </div>

    <div class="card">
      <div class="card-h"><h3>Tuning &amp; Configuration</h3></div>
      <div class="form-group"><label>Min Runtime (ms):</label><input type="number" id="cRun" min="1000" max="300000"></div>
      <div class="form-group"><label>Min Cooldown (ms):</label><input type="number" id="cCool" min="5000" max="600000"></div>
      <div class="form-group"><label>Telemetry Heartbeat (ms):</label><input type="number" id="cTelem" min="2000" max="300000"></div>
      <div style="display:flex; gap:8px; margin-top:12px; flex-wrap:wrap;">
        <button id="btnSave" class="btn btn-green">💾 Save</button>
        <button id="btnRefresh" class="btn btn-out">🔄 Refresh</button>
        <button id="btnReboot" class="btn btn-red">⚡ Reboot</button>
      </div>
    </div>
  </div>
  <div id="toast" class="toast">Saved</div>

  <script>
    let ws;
    const wsBadge = document.getElementById('wsBadge');
    const toast = document.getElementById('toast');
    function notify(m) { toast.innerText = m; toast.classList.add('show'); setTimeout(() => toast.classList.remove('show'), 2000); }
    function connect() {
      ws = new WebSocket(`ws://${location.hostname || '192.168.4.1'}:81/`);
      ws.onopen = () => { wsBadge.className = 'badge badge-green'; wsBadge.innerText = 'Connected'; ws.send(JSON.stringify({cmd:'get_config'})); };
      ws.onclose = () => { wsBadge.className = 'badge badge-red'; wsBadge.innerText = 'Disconnected'; setTimeout(connect, 2000); };
      ws.onmessage = (e) => {
        const d = JSON.parse(e.data);
        if (d.type === 'telemetry') {
          document.getElementById('lvlTxt').innerText = `Level ${d.wl}/3`;
          document.getElementById('water').style.height = `${(d.wl/3)*100}%`;
          document.getElementById('btnMode').innerText = `Mode: ${d.mode}`;
          document.getElementById('demandTxt').innerText = d.demand ? 'CHARGING' : 'SATISFIED';
          document.getElementById('pondTxt').innerText = d.pond ? 'ON' : 'OFF';
          document.getElementById('p1Txt').innerText = d.pumps[0] ? 'ON' : 'OFF';
          document.getElementById('p2Txt').innerText = d.pumps[1] ? 'ON' : 'OFF';
          document.getElementById('p3Txt').innerText = d.pumps[2] ? 'ON' : 'OFF';
          document.getElementById('chk1').checked = d.auto_mask[0];
          document.getElementById('chk2').checked = d.auto_mask[1];
          document.getElementById('chk3').checked = d.auto_mask[2];
          document.getElementById('rssi').innerText = `${d.rssi} dBm`;
          document.getElementById('snr').innerText = `${d.snr} dB`;
          document.getElementById('up').innerText = `${d.uptime}s`;
          document.getElementById('gw').innerText = d.gw_seen ? `${d.gw_ago}s ago` : 'Never';
          if (d.err > 0) { document.getElementById('errBox').style.display='block'; document.getElementById('errMsg').innerText = `Error [${d.err}]: ${d.err_str}`; }
          else { document.getElementById('errBox').style.display='none'; }
        } else if (d.type === 'config') {
          document.getElementById('cRun').value = d.runtime;
          document.getElementById('cCool').value = d.cooldown;
          document.getElementById('cTelem').value = d.telem;
        }
      };
    }
    document.getElementById('btnMode').onclick = () => ws.send(JSON.stringify({cmd:'mode', val:'toggle'}));
    document.getElementById('btnPond').onclick = () => ws.send(JSON.stringify({cmd:'pond', val:'toggle'}));
    document.getElementById('btnP1').onclick = () => ws.send(JSON.stringify({cmd:'pump', id:1, val:'toggle'}));
    document.getElementById('btnP2').onclick = () => ws.send(JSON.stringify({cmd:'pump', id:2, val:'toggle'}));
    document.getElementById('btnP3').onclick = () => ws.send(JSON.stringify({cmd:'pump', id:3, val:'toggle'}));
    function saveMask() {
      ws.send(JSON.stringify({cmd:'set_auto_mask', mask:[
        document.getElementById('chk1').checked ? 1 : 0,
        document.getElementById('chk2').checked ? 1 : 0,
        document.getElementById('chk3').checked ? 1 : 0
      ]}));
    }
    document.getElementById('chk1').onchange = saveMask;
    document.getElementById('chk2').onchange = saveMask;
    document.getElementById('chk3').onchange = saveMask;
    document.getElementById('btnSave').onclick = () => {
      ws.send(JSON.stringify({
        cmd:'set_config',
        runtime: parseInt(document.getElementById('cRun').value, 10),
        cooldown: parseInt(document.getElementById('cCool').value, 10),
        telem: parseInt(document.getElementById('cTelem').value, 10),
        boot_auto: 0
      }));
      notify('Configuration Saved');
    };
    document.getElementById('btnRefresh').onclick = () => { ws.send(JSON.stringify({cmd:'get_config'})); notify('Refreshed'); };
    document.getElementById('btnReboot').onclick = () => { if(confirm('Reboot device?')) { ws.send(JSON.stringify({cmd:'reboot'})); notify('Rebooting...'); } };
    window.onload = connect;
  </script>
</body>
</html>
)rawliteral";
