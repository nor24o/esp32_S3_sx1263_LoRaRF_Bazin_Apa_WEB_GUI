#pragma once
#include <Arduino.h>

const char PAGE_INDEX_FALLBACK[] PROGMEM = R"RAW_HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tank Node // SCADA Control Deck</title>
  <style>
/* ─── SCADA INDUSTRIAL CLEAN PALETTE & DESIGN SYSTEM ─────────────── */
:root {
  /* Slate / Obsidian Neutral Dark Canvas */
  --bg-dark: #0a0e17;
  --bg-card: #111827;
  --bg-card-subtle: #162032;
  --bg-card-hover: #1e293b;
  --bg-input: #0b0f19;
  
  --border-subtle: rgba(255, 255, 255, 0.07);
  --border-card: rgba(255, 255, 255, 0.10);
  --border-active: #0ea5e9;
  
  /* Precision Industrial Functional Colors */
  --cyan: #0ea5e9;         /* Sky Blue (Primary Brand & Telemetry) */
  --cyan-glow: rgba(14, 165, 233, 0.25);
  
  --green: #10b981;        /* Emerald (Healthy / Running) */
  --green-glow: rgba(16, 185, 129, 0.25);
  
  --amber: #f59e0b;        /* Amber (Notice / Cooldown / Warning) */
  --amber-glow: rgba(245, 158, 11, 0.25);
  --orange: #f59e0b;
  --orange-glow: rgba(245, 158, 11, 0.25);
  
  --red: #ef4444;          /* Crimson (Trip / Lockout / Alarm) */
  --red-glow: rgba(239, 68, 68, 0.25);
  
  --purple: #8b5cf6;       /* Violet (System / Secondary) */
  --purple-glow: rgba(139, 92, 246, 0.25);
  --magenta: #ec4899;
  
  /* Natural Water Gradient */
  --water-grad-top: #38bdf8;
  --water-grad-bot: #0369a1;
  
  /* Typography & Text */
  --text-main: #f8fafc;
  --text-muted: #94a3b8;
  --text-dim: #64748b;
  
  --font-mono: 'JetBrains Mono', 'SF Mono', 'Fira Code', 'Consolas', monospace;
  --font-sans: -apple-system, BlinkMacSystemFont, 'Inter', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
}

* {
  box-sizing: border-box;
  margin: 0;
  padding: 0;
}

body {
  background-color: var(--bg-dark);
  color: var(--text-main);
  font-family: var(--font-sans);
  min-height: 100vh;
  position: relative;
  overflow-x: hidden;
  padding: 16px;
  background-image: 
    radial-gradient(at 0% 0%, rgba(14, 165, 233, 0.05) 0%, transparent 50%),
    radial-gradient(at 100% 100%, rgba(16, 185, 129, 0.04) 0%, transparent 50%),
    linear-gradient(to right, rgba(255, 255, 255, 0.012) 1px, transparent 1px),
    linear-gradient(to bottom, rgba(255, 255, 255, 0.012) 1px, transparent 1px);
  background-size: 100% 100%, 100% 100%, 32px 32px, 32px 32px;
}

/* Background elements */
.bg-glow, .bg-grid {
  display: none; /* Replaced with clean body background pattern */
}

.app-container {
  position: relative;
  z-index: 1;
  max-width: 1320px;
  margin: 0 auto;
  display: flex;
  flex-direction: column;
  gap: 14px;
}

/* ─── PRECISION INDUSTRIAL CARD ─────────────────────────────────── */
.glass-card {
  background: var(--bg-card);
  border: 1px solid var(--border-subtle);
  border-radius: 12px;
  box-shadow: 0 4px 20px -2px rgba(0, 0, 0, 0.5), 0 0 0 1px rgba(255, 255, 255, 0.03);
  transition: border-color 0.2s, box-shadow 0.2s;
}

.glass-card:hover {
  border-color: rgba(255, 255, 255, 0.14);
}

/* ─── TOP HUD HEADER ───────────────────────────────────────────── */
.hud-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 18px;
  flex-wrap: wrap;
  gap: 12px;
}

.brand-group {
  display: flex;
  align-items: center;
  gap: 12px;
}

.pulse-indicator {
  width: 11px;
  height: 11px;
  border-radius: 50%;
  background: var(--red);
  box-shadow: 0 0 8px var(--red-glow);
  transition: background 0.3s, box-shadow 0.3s;
}

.pulse-indicator.online {
  background: var(--green);
  box-shadow: 0 0 10px var(--green-glow);
  animation: pulseBeacon 2s infinite ease-in-out;
}

@keyframes pulseBeacon {
  0% { transform: scale(0.95); opacity: 0.85; }
  50% { transform: scale(1.1); opacity: 1; }
  100% { transform: scale(0.95); opacity: 0.85; }
}

.brand-titles h1 {
  font-size: 1.05rem;
  font-weight: 700;
  letter-spacing: 0.06em;
  color: var(--text-main);
  display: flex;
  align-items: center;
  gap: 8px;
  font-family: var(--font-mono);
}

.brand-titles .tag {
  font-size: 0.65rem;
  padding: 2px 7px;
  background: rgba(14, 165, 233, 0.12);
  border: 1px solid rgba(14, 165, 233, 0.35);
  border-radius: 4px;
  color: var(--cyan);
  letter-spacing: 0.05em;
  font-weight: 600;
}

.brand-titles .subtitle {
  font-size: 0.72rem;
  color: var(--text-muted);
  font-family: var(--font-mono);
  margin-top: 1px;
}

.hud-badges {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.badge {
  display: flex;
  align-items: center;
  gap: 6px;
  background: #0b0f19;
  border: 1px solid var(--border-subtle);
  padding: 6px 11px;
  border-radius: 7px;
  font-size: 0.73rem;
  font-family: var(--font-mono);
  color: #cbd5e1;
}

.icon-btn {
  background: #0b0f19;
  border: 1px solid var(--border-subtle);
  color: #cbd5e1;
  border-radius: 7px;
  width: 34px;
  height: 34px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 0.95rem;
  transition: all 0.18s ease;
}

.icon-btn:hover {
  background: #1e293b;
  border-color: rgba(255, 255, 255, 0.2);
  color: #fff;
  transform: translateY(-1px);
}

/* ─── ISA-18.2 SYSTEM STATUS & PRIORITY ALARM BANNER ──────────── */
.alarm-banner-container {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 18px;
  flex-wrap: wrap;
  gap: 12px;
  border-left: 4px solid var(--green);
  background: #0d1424;
  border-radius: 10px;
  transition: all 0.25s ease;
}

.alarm-banner-container.warning {
  border-left-color: var(--amber);
  background: rgba(245, 158, 11, 0.06);
}

.alarm-banner-container.critical {
  border-left-color: var(--red);
  background: rgba(239, 68, 68, 0.08);
}

.alarm-left {
  display: flex;
  align-items: center;
  gap: 12px;
}

.alarm-beacon {
  width: 11px;
  height: 11px;
  border-radius: 50%;
  flex-shrink: 0;
}

.alarm-beacon.normal {
  background: var(--green);
  box-shadow: 0 0 8px var(--green-glow);
}

.alarm-beacon.warning {
  background: var(--amber);
  box-shadow: 0 0 10px var(--amber-glow);
  animation: pulseBeacon 1s infinite alternate;
}

.alarm-beacon.critical {
  background: var(--red);
  box-shadow: 0 0 12px var(--red-glow);
  animation: pulseBeacon 0.6s infinite alternate;
}

.alarm-text-block .alarm-title {
  font-family: var(--font-mono);
  font-size: 0.82rem;
  font-weight: 700;
  color: var(--text-main);
  letter-spacing: 0.04em;
}

.alarm-text-block .alarm-desc {
  font-size: 0.70rem;
  color: var(--text-muted);
  margin-top: 1px;
}

.alarm-right-metrics {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
  align-items: center;
}

.banner-pill {
  background: #090d16;
  border: 1px solid var(--border-subtle);
  padding: 4px 9px;
  border-radius: 6px;
  font-family: var(--font-mono);
  font-size: 0.70rem;
  display: inline-flex;
  gap: 6px;
  align-items: center;
}

.bp-label { color: var(--text-dim); font-size: 0.66rem; font-weight: 600; }
.bp-val { color: var(--cyan); font-weight: 700; }

/* ─── MAIN GRID ────────────────────────────────────────────────── */
.main-grid {
  display: grid;
  grid-template-columns: 1.15fr 0.85fr;
  gap: 14px;
}

@media (max-width: 980px) {
  .main-grid {
    grid-template-columns: 1fr;
  }
}

/* ─── RESERVOIR CARD & P&ID SCHEMATIC ──────────────────────────── */
.reservoir-card {
  padding: 18px 20px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  padding-bottom: 8px;
  border-bottom: 1px solid var(--border-subtle);
}

.card-header-left h2 {
  font-size: 0.88rem;
  font-weight: 700;
  letter-spacing: 0.06em;
  color: var(--text-main);
  font-family: var(--font-mono);
}

.section-sub {
  font-size: 0.68rem;
  color: var(--text-muted);
  font-family: var(--font-mono);
  margin-top: 2px;
}

.level-display {
  font-family: var(--font-mono);
  font-size: 0.82rem;
  font-weight: 700;
  color: var(--cyan);
  background: #090d16;
  padding: 4px 10px;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
}

/* Tank Stage Blueprint Area */
.tank-stage {
  position: relative;
  height: 360px;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 20px 40px;
  background: #090d16;
  border-radius: 10px;
  border: 1px solid rgba(255, 255, 255, 0.04);
}

/* Inflow Pipe at Top Left */
.pipe-inflow-container {
  position: absolute;
  top: 20px;
  left: 12px;
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  z-index: 5;
}

.inflow-pipe {
  width: 110px;
  height: 20px;
  background: #1e293b;
  border: 2px solid #334155;
  border-right: none;
  border-radius: 4px 0 0 4px;
  position: relative;
  overflow: hidden;
}

.pipe-label {
  position: absolute;
  top: -16px;
  left: 0;
  font-size: 0.58rem;
  font-family: var(--font-mono);
  color: var(--text-dim);
  font-weight: 700;
  letter-spacing: 0.04em;
}

.pipe-flow-water {
  width: 100%;
  height: 100%;
  background: repeating-linear-gradient(
    -45deg,
    #0284c7,
    #0284c7 8px,
    #0ea5e9 8px,
    #0ea5e9 16px
  );
  opacity: 0;
  transition: opacity 0.3s;
}

.pipe-flow-water.active {
  opacity: 0.95;
  animation: flowSlide 1.2s linear infinite;
}

@keyframes flowSlide {
  0% { background-position: 0 0; }
  100% { background-position: 32px 0; }
}

/* Water Jet Downward into Reservoir */
.water-jet {
  width: 10px;
  height: 0;
  background: linear-gradient(180deg, #38bdf8 0%, rgba(3, 105, 161, 0.8) 100%);
  border-radius: 0 0 5px 5px;
  transition: height 0.4s ease;
  position: relative;
}

.water-jet.active {
  height: 130px;
}

.splash-ring {
  position: absolute;
  bottom: 0;
  left: -10px;
  width: 30px;
  height: 10px;
  border: 2px solid var(--cyan);
  border-radius: 50%;
  opacity: 0;
}

.water-jet.active .splash-ring {
  animation: splashRipple 0.8s ease-out infinite;
}

@keyframes splashRipple {
  0% { transform: scale(0.3); opacity: 1; }
  100% { transform: scale(2); opacity: 0; }
}

/* Outflow Pipe Group at Bottom Right (3 Parallel Lines, Direct Suction) */
.pipe-outflow-group {
  position: absolute;
  bottom: 22px;
  right: 12px;
  display: flex;
  flex-direction: column;
  gap: 16px;
  z-index: 5;
}

.outflow-pipe {
  width: 105px;
  height: 22px;
  background: #1e293b;
  border: 2px solid #334155;
  border-left: none;
  border-radius: 0 4px 4px 0;
  position: relative;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 6px;
  overflow: visible;
}

.p-tag {
  font-size: 0.62rem;
  font-family: var(--font-mono);
  font-weight: 700;
  color: #94a3b8;
  z-index: 2;
}

.field-line-label {
  position: absolute;
  bottom: -15px;
  left: 50%;
  transform: translateX(-50%);
  font-family: var(--font-mono);
  font-size: 0.56rem;
  color: var(--text-dim);
  font-weight: 700;
  letter-spacing: 0.05em;
  white-space: nowrap;
}

.turbine {
  width: 14px;
  height: 14px;
  border-radius: 50%;
  border: 2px dashed #64748b;
  z-index: 2;
  transition: all 0.2s;
}

.turbine.active {
  border-color: var(--green);
  animation: spinTurbine 0.5s linear infinite;
}

@keyframes spinTurbine {
  100% { transform: rotate(360deg); }
}

.pipe-stream {
  position: absolute;
  top: 0; left: 0; width: 100%; height: 100%;
  background: repeating-linear-gradient(
    90deg,
    rgba(16, 185, 129, 0.85),
    rgba(16, 185, 129, 0.85) 10px,
    transparent 10px,
    transparent 20px
  );
  opacity: 0;
  transition: opacity 0.3s;
}

.pipe-stream.active {
  opacity: 1;
  animation: streamRight 0.7s linear infinite;
}

@keyframes streamRight {
  0% { background-position: 0 0; }
  100% { background-position: 40px 0; }
}

/* ─── PHYSICAL RESERVOIR TANK VESSEL ───────────────────────────── */
.tank-vessel {
  width: 260px;
  height: 310px;
  background: #060911;
  border: 3px solid #334155;
  border-radius: 10px;
  position: relative;
  overflow: hidden;
  box-shadow: inset 0 0 24px rgba(0, 0, 0, 0.9);
}

.tank-reflection {
  position: absolute;
  top: 0; left: 0; width: 100%; height: 100%;
  background: linear-gradient(135deg, rgba(255, 255, 255, 0.04) 0%, transparent 45%);
  pointer-events: none;
  z-index: 4;
}

/* Float Switch Buoy & Wire */
.sensor-buoy {
  position: absolute;
  left: 20px;
  display: flex;
  align-items: center;
  gap: 8px;
  z-index: 3;
}

.sensor-wire {
  position: absolute;
  left: 6px;
  top: -240px;
  width: 2px;
  height: 240px;
  background: #334155;
}

.sensor-bead {
  width: 14px;
  height: 14px;
  border-radius: 50%;
  background: #1e293b;
  border: 2px solid #475569;
  transition: all 0.25s ease;
}

.sensor-bead.submerged {
  background: var(--green);
  border-color: #a7f3d0;
  box-shadow: 0 0 8px var(--green-glow);
}

.sensor-bead.fault {
  background: var(--red);
  border-color: #fca5a5;
  box-shadow: 0 0 10px var(--red-glow);
  animation: pulseBeacon 0.6s infinite alternate;
}

.sensor-label {
  font-size: 0.64rem;
  font-family: var(--font-mono);
  color: #94a3b8;
  background: rgba(15, 23, 42, 0.9);
  padding: 2px 6px;
  border-radius: 4px;
  border: 1px solid var(--border-subtle);
}

/* Dynamic Liquid Body inside Tank */
.liquid-body {
  position: absolute;
  bottom: 0;
  left: 0;
  width: 100%;
  height: 0%;
  background: linear-gradient(180deg, var(--water-grad-top) 0%, var(--water-grad-bot) 100%);
  transition: height 0.8s cubic-bezier(0.4, 0, 0.2, 1);
  overflow: visible;
}

.wave-svg {
  position: absolute;
  top: -20px;
  left: 0;
  width: 100%;
  height: 22px;
}

.wave-path { fill: var(--water-grad-top); }
.wave-back { fill: rgba(56, 189, 248, 0.45); animation: waveMotionBack 3s ease-in-out infinite alternate; }
.wave-front { fill: var(--water-grad-top); opacity: 0.9; animation: waveMotionFront 2s ease-in-out infinite alternate; }

@keyframes waveMotionBack { 0% { transform: translateX(-25px); } 100% { transform: translateX(25px); } }
@keyframes waveMotionFront { 0% { transform: translateX(25px); } 100% { transform: translateX(-25px); } }

.bubble {
  position: absolute;
  background: rgba(255, 255, 255, 0.3);
  border-radius: 50%;
  bottom: 0;
  animation: bubbleRise 4s infinite linear;
}

.b1 { width: 5px; height: 5px; left: 20%; animation-duration: 3.5s; }
.b2 { width: 7px; height: 7px; left: 50%; animation-duration: 4.2s; animation-delay: 1s; }
.b3 { width: 4px; height: 4px; left: 75%; animation-duration: 3.8s; animation-delay: 0.5s; }
.b4 { width: 8px; height: 8px; left: 35%; animation-duration: 5s; animation-delay: 2s; }
.b5 { width: 6px; height: 6px; left: 85%; animation-duration: 3.2s; animation-delay: 0.8s; }

@keyframes bubbleRise {
  0% { transform: translateY(0) scale(0.8); opacity: 0; }
  20% { opacity: 0.7; }
  80% { opacity: 0.7; }
  100% { transform: translateY(-280px) scale(1.2); opacity: 0; }
}

.tank-ruler {
  position: absolute;
  right: 10px;
  top: 0;
  height: 100%;
  display: flex;
  flex-direction: column;
}

.tank-ruler .mark {
  position: absolute;
  right: 0;
  font-family: var(--font-mono);
  font-size: 0.62rem;
  color: var(--text-dim);
}

.sensor-status-bar {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  background: #090d16;
  border: 1px solid var(--border-subtle);
  border-radius: 8px;
  font-size: 0.72rem;
  font-family: var(--font-mono);
  color: #cbd5e1;
}

.sensor-status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--green);
}

.sensor-status-bar.alert {
  border-color: rgba(239, 68, 68, 0.4);
  background: rgba(239, 68, 68, 0.08);
}

.sensor-status-bar.alert .sensor-status-dot {
  background: var(--red);
  box-shadow: 0 0 8px var(--red-glow);
}

/* ─── RIGHT COLUMN: CONTROL DECK ───────────────────────────────── */
.control-deck {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

/* Master Mode Switcher */
.mode-card {
  padding: 14px 18px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.mode-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.mode-header h3 {
  font-size: 0.84rem;
  font-weight: 700;
  letter-spacing: 0.05em;
  color: var(--text-main);
  font-family: var(--font-mono);
}

.mode-status-text {
  font-family: var(--font-mono);
  font-size: 0.74rem;
  font-weight: 700;
  color: var(--amber);
  background: #090d16;
  padding: 3px 8px;
  border-radius: 5px;
  border: 1px solid var(--border-subtle);
}

.mode-status-text.auto {
  color: var(--green);
  border-color: rgba(16, 185, 129, 0.3);
}

.mode-toggle-group {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 6px;
  background: #090d16;
  padding: 4px;
  border-radius: 8px;
  border: 1px solid var(--border-subtle);
}

.mode-btn {
  background: transparent;
  border: 1px solid transparent;
  color: var(--text-muted);
  padding: 8px;
  font-size: 0.78rem;
  font-weight: 700;
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.2s;
  letter-spacing: 0.05em;
  font-family: var(--font-mono);
}

.mode-btn.active {
  background: #1e293b;
  color: #fff;
  border-color: rgba(255, 255, 255, 0.15);
}

.mode-btn#btnModeAuto.active {
  background: rgba(16, 185, 129, 0.18);
  color: #34d399;
  border-color: var(--green);
}

.mode-hint {
  font-size: 0.68rem;
  color: var(--text-dim);
  line-height: 1.35;
}

/* ─── PUMPS GRID (INDUSTRIAL FACEPLATES) ────────────────────────── */
.pumps-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 10px;
}

@media (max-width: 640px) {
  .pumps-grid {
    grid-template-columns: 1fr;
  }
}

.pump-card {
  padding: 12px 14px;
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  gap: 8px;
}

.pump-card-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  gap: 6px;
}

.pump-title {
  display: flex;
  align-items: center;
  gap: 7px;
}

.pump-title h4 {
  font-size: 0.80rem;
  font-weight: 700;
  letter-spacing: 0.04em;
  color: var(--text-main);
  font-family: var(--font-mono);
}

.pump-sub-tag {
  display: block;
  font-size: 0.60rem;
  color: var(--text-dim);
  font-family: var(--font-mono);
  font-weight: 400;
  margin-top: 1px;
}

.pump-indicator {
  width: 9px;
  height: 9px;
  border-radius: 50%;
  background: #334155;
  transition: all 0.2s;
  flex-shrink: 0;
}

.pump-indicator.active {
  background: var(--green);
  box-shadow: 0 0 8px var(--green-glow);
}

.pump-indicator.pond-ind.active {
  background: var(--green);
  box-shadow: 0 0 10px var(--green-glow);
  animation: pulseBeacon 1.2s infinite ease-in-out;
}

/* 3-Position Override Segmented Switch */
.override-panel {
  background: #090d16;
  padding: 6px 8px;
  border-radius: 7px;
  border: 1px solid var(--border-subtle);
}

.override-label {
  font-size: 0.58rem;
  font-family: var(--font-mono);
  color: var(--text-dim);
  font-weight: 700;
  display: block;
  margin-bottom: 5px;
  letter-spacing: 0.04em;
}

.override-btn-group {
  display: grid;
  grid-template-columns: 1fr 1.15fr 1fr;
  gap: 3px;
  background: #111827;
  padding: 3px;
  border-radius: 6px;
  border: 1px solid rgba(255, 255, 255, 0.04);
}

.btn-override {
  background: transparent;
  border: 1px solid transparent;
  color: var(--text-dim);
  font-family: var(--font-mono);
  font-size: 0.66rem;
  font-weight: 700;
  padding: 5px 2px;
  border-radius: 4px;
  cursor: pointer;
  transition: all 0.16s ease;
  text-align: center;
}

.btn-override:hover {
  color: #fff;
  background: rgba(255, 255, 255, 0.06);
}

.btn-override.btn-ovr-auto.active {
  background: rgba(14, 165, 233, 0.18);
  border-color: rgba(14, 165, 233, 0.45);
  color: #38bdf8;
}

.btn-override.btn-ovr-on.active {
  background: #059669;
  border-color: #10b981;
  color: #ffffff;
  box-shadow: 0 0 8px rgba(16, 185, 129, 0.35);
}

.btn-override.btn-ovr-off.active {
  background: #dc2626;
  border-color: #ef4444;
  color: #ffffff;
  box-shadow: 0 0 8px rgba(239, 68, 68, 0.35);
}

.timed-run-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: #090d16;
  padding: 5px 8px;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
  gap: 6px;
}

.timed-label {
  font-size: 0.64rem;
  font-family: var(--font-mono);
  color: var(--text-muted);
}

.timed-select {
  background: #111827;
  border: 1px solid var(--border-subtle);
  color: #e2e8f0;
  font-family: var(--font-mono);
  font-size: 0.66rem;
  padding: 2px 5px;
  border-radius: 4px;
  cursor: pointer;
}

.timed-select:focus {
  border-color: var(--cyan);
  outline: none;
}

.timed-active-badge {
  font-family: var(--font-mono);
  font-size: 0.66rem;
  font-weight: 700;
  color: var(--cyan);
  background: rgba(14, 165, 233, 0.15);
  border: 1px solid rgba(14, 165, 233, 0.35);
  padding: 2px 6px;
  border-radius: 4px;
  animation: pulseBeacon 1.2s infinite alternate;
}

.pump-feedback-box {
  background: #090d16;
  padding: 6px 8px;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
}

.feedback-gauge .gauge-hdr {
  display: flex;
  justify-content: space-between;
  font-family: var(--font-mono);
  font-size: 0.62rem;
  margin-bottom: 3px;
}

.feedback-gauge .gauge-title { color: var(--text-dim); }
.feedback-gauge .gauge-val { color: var(--text-muted); font-weight: 600; }
.feedback-gauge .gauge-val.locked { color: var(--amber); }
.feedback-gauge .gauge-val.running { color: var(--cyan); }

.feedback-gauge .gauge-track {
  height: 3px;
  background: #1e293b;
  border-radius: 2px;
  overflow: hidden;
}

.feedback-gauge .gauge-fill {
  height: 100%;
  background: var(--cyan);
  width: 0%;
  transition: width 0.3s;
}

/* Status Pill */
.status-pill {
  font-size: 0.62rem;
  font-family: var(--font-mono);
  font-weight: 700;
  padding: 2px 6px;
  border-radius: 4px;
  background: #090d16;
  color: var(--text-dim);
  border: 1px solid var(--border-subtle);
  white-space: nowrap;
}

.status-pill.running {
  background: rgba(16, 185, 129, 0.14);
  color: #34d399;
  border-color: rgba(16, 185, 129, 0.35);
}

.status-pill.cooldown {
  background: rgba(245, 158, 11, 0.14);
  color: #fbbf24;
  border-color: rgba(245, 158, 11, 0.35);
}

.status-pill.locked, .status-pill.fault {
  background: rgba(239, 68, 68, 0.14);
  color: #f87171;
  border-color: rgba(239, 68, 68, 0.35);
}

.pump-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 6px;
  margin-top: 4px;
}

.auto-mask-check {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 0.64rem;
  font-family: var(--font-mono);
  color: var(--text-dim);
  cursor: pointer;
}

.auto-mask-check input {
  accent-color: var(--cyan);
  cursor: pointer;
}

.pump-toggle-btn {
  background: #1e293b;
  border: 1px solid rgba(255, 255, 255, 0.12);
  color: var(--text-main);
  padding: 5px 10px;
  border-radius: 6px;
  font-size: 0.68rem;
  font-weight: 700;
  font-family: var(--font-mono);
  cursor: pointer;
  transition: all 0.16s ease;
}

.pump-toggle-btn:hover:not(:disabled) {
  background: #334155;
  border-color: rgba(255, 255, 255, 0.25);
}

.pump-toggle-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.pump-odometer {
  font-family: var(--font-mono);
  font-size: 0.58rem;
  color: var(--text-dim);
  text-align: center;
  margin-top: 3px;
  padding-top: 3px;
  border-top: 1px dashed rgba(255, 255, 255, 0.05);
}

/* Remote Pond Refill Card */
.pond-card {
  padding: 12px 18px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.pond-info .pump-title h4 {
  font-size: 0.84rem;
}

.pond-desc {
  font-size: 0.68rem;
  color: var(--text-muted);
  margin-top: 2px;
  font-family: var(--font-mono);
}

.pond-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.charge-latch-badge {
  font-size: 0.66rem;
  font-family: var(--font-mono);
  font-weight: 700;
  padding: 4px 8px;
  border-radius: 5px;
  background: #090d16;
  color: var(--text-dim);
  border: 1px solid var(--border-subtle);
}

.charge-latch-badge.active {
  background: rgba(245, 158, 11, 0.15);
  color: var(--amber);
  border-color: rgba(245, 158, 11, 0.4);
}

.pond-toggle-btn {
  background: rgba(14, 165, 233, 0.12);
  border: 1px solid rgba(14, 165, 233, 0.35);
  color: var(--cyan);
  padding: 6px 12px;
  border-radius: 6px;
  font-size: 0.72rem;
  font-weight: 700;
  font-family: var(--font-mono);
  cursor: pointer;
  transition: all 0.16s ease;
}

.pond-toggle-btn:hover {
  background: rgba(14, 165, 233, 0.22);
  border-color: var(--cyan);
}

/* ─── LIVE SPARKLINE GRAPH CANVAS ──────────────────────────────── */
.sparkline-card {
  padding: 12px 16px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.sparkline-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.sparkline-header h3 {
  font-size: 0.76rem;
  font-family: var(--font-mono);
  font-weight: 700;
  color: var(--text-muted);
  letter-spacing: 0.05em;
}

.sparkline-legend {
  font-size: 0.65rem;
  font-family: var(--font-mono);
  color: var(--text-dim);
  display: flex;
  align-items: center;
  gap: 8px;
}

.leg-dot {
  display: inline-block;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  margin-right: 2px;
}

.level-dot { background: var(--cyan); }
.rssi-dot { background: #a78bfa; }

#sparklineCanvas {
  width: 100%;
  height: 90px;
  background: #090d16;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
}

/* ─── HYDRAULIC AUTOMATION MATRIX ─────────────────────────────── */
.automation-matrix-card {
  padding: 16px 20px;
}

.matrix-card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
  flex-wrap: wrap;
  gap: 8px;
}

.matrix-active-tag {
  font-family: var(--font-mono);
  font-size: 0.70rem;
  font-weight: 700;
  padding: 4px 10px;
  border-radius: 6px;
  background: rgba(14, 165, 233, 0.12);
  color: var(--cyan);
  border: 1px solid rgba(14, 165, 233, 0.35);
}

.automation-table-wrap {
  overflow-x: auto;
}

.automation-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.72rem;
  font-family: var(--font-mono);
}

.automation-table th {
  background: #090d16;
  color: var(--text-dim);
  font-weight: 600;
  text-align: left;
  padding: 8px 12px;
  border-bottom: 1px solid var(--border-subtle);
  font-size: 0.66rem;
  letter-spacing: 0.05em;
}

.automation-table td {
  padding: 9px 12px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.04);
  color: #cbd5e1;
  vertical-align: middle;
}

.rule-row {
  transition: all 0.18s ease;
}

.rule-row:hover {
  background: rgba(255, 255, 255, 0.02);
}

.rule-row.active-rule {
  background: rgba(14, 165, 233, 0.07) !important;
  outline: 1px solid var(--cyan);
}

.rule-row.trip-row.active-rule {
  background: rgba(239, 68, 68, 0.09) !important;
  outline: 1px solid var(--red);
}

.rule-num {
  background: #1e293b;
  padding: 2px 5px;
  border-radius: 4px;
  font-size: 0.62rem;
  margin-right: 6px;
  font-weight: 700;
  color: #cbd5e1;
}

.rule-status-badge {
  font-size: 0.62rem;
  font-weight: 700;
  padding: 2px 7px;
  border-radius: 4px;
  background: #090d16;
  color: var(--text-dim);
  border: 1px solid var(--border-subtle);
}

.rule-row.active-rule .rule-status-badge {
  background: rgba(14, 165, 233, 0.18);
  color: var(--cyan);
  border-color: var(--cyan);
}

.rule-row.trip-row.active-rule .rule-status-badge {
  background: rgba(239, 68, 68, 0.18);
  color: #f87171;
  border-color: var(--red);
}

/* ─── LIVE SYSTEM I/O & PARAMETERS MATRIX ─────────────────────── */
.matrix-card {
  padding: 16px 20px;
}

.matrix-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid var(--border-subtle);
  padding-bottom: 12px;
  margin-bottom: 14px;
}

.matrix-title-group {
  display: flex;
  align-items: center;
  gap: 10px;
}

.matrix-icon {
  font-size: 1.3rem;
  color: var(--amber);
}

.matrix-header h3 {
  font-family: var(--font-mono);
  font-size: 0.92rem;
  font-weight: 700;
  letter-spacing: 0.06em;
  color: var(--text-main);
  margin: 0;
}

.matrix-sub {
  font-size: 0.68rem;
  color: var(--text-muted);
  margin: 1px 0 0 0;
  font-family: var(--font-mono);
}

.matrix-header-right {
  display: flex;
  align-items: center;
  gap: 8px;
}

.matrix-live-pill {
  display: flex;
  align-items: center;
  gap: 5px;
  font-size: 0.68rem;
  font-family: var(--font-mono);
  font-weight: 700;
  color: var(--green);
  background: rgba(16, 185, 129, 0.1);
  border: 1px solid rgba(16, 185, 129, 0.3);
  padding: 3px 8px;
  border-radius: 12px;
}

.matrix-pulse {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--green);
}

.matrix-act-btn, .matrix-toggle-btn {
  background: #090d16;
  border: 1px solid var(--border-subtle);
  color: #cbd5e1;
  padding: 4px 10px;
  border-radius: 6px;
  font-family: var(--font-mono);
  font-size: 0.68rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.16s ease;
}

.matrix-act-btn:hover, .matrix-toggle-btn:hover {
  background: #1e293b;
  border-color: rgba(255, 255, 255, 0.2);
  color: #fff;
}

.matrix-delta-pill {
  font-family: var(--font-mono);
  font-size: 0.66rem;
  font-weight: 700;
  color: var(--cyan);
  background: rgba(14, 165, 233, 0.08);
  border: 1px solid rgba(14, 165, 233, 0.25);
  padding: 3px 8px;
  border-radius: 12px;
}

.matrix-delta-pill.changed {
  color: #fff;
  background: rgba(245, 158, 11, 0.25);
  border-color: var(--amber);
}

.matrix-content {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
  gap: 14px;
}

.matrix-content.collapsed { display: none; }

.matrix-subcard {
  background: #090d16;
  border: 1px solid var(--border-subtle);
  border-radius: 8px;
  padding: 12px 14px;
}

.subcard-title {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 10px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.04);
  padding-bottom: 6px;
}

.subcard-title h4 {
  font-family: var(--font-mono);
  font-size: 0.74rem;
  font-weight: 700;
  color: var(--text-muted);
  letter-spacing: 0.05em;
  margin: 0;
  flex: 1;
}

.subcard-badge {
  font-family: var(--font-mono);
  font-size: 0.65rem;
  background: #111827;
  padding: 2px 7px;
  border-radius: 4px;
  color: var(--cyan);
  border: 1px solid var(--border-subtle);
}

/* 8-Bit Pin Rack */
.pin-bus-rack {
  display: grid;
  grid-template-columns: repeat(8, 1fr);
  gap: 4px;
  margin-bottom: 10px;
  background: #111827;
  padding: 6px;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
}

.pin-chip {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
  background: #090d16;
  border: 1px solid var(--border-subtle);
  padding: 4px 1px;
  border-radius: 4px;
}

.pin-chip.active {
  border-color: var(--green);
  background: rgba(16, 185, 129, 0.12);
}

.pin-chip.pressed {
  border-color: var(--amber);
  background: rgba(245, 158, 11, 0.15);
}

.pin-num {
  font-size: 0.58rem;
  font-family: var(--font-mono);
  font-weight: 700;
  color: var(--text-dim);
}

.pin-dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: #334155;
}

.pin-chip.active .pin-dot { background: var(--green); }
.pin-chip.pressed .pin-dot { background: var(--amber); }

.pin-lbl {
  font-size: 0.54rem;
  font-family: var(--font-mono);
  color: var(--text-muted);
}

/* Level comparison */
.level-compare-box {
  background: #111827;
  border: 1px solid var(--border-subtle);
  border-radius: 6px;
  padding: 7px 9px;
  margin-bottom: 10px;
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.lvl-track {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  font-family: var(--font-mono);
  font-size: 0.68rem;
}

.lvl-title {
  color: var(--text-dim);
  width: 130px;
  flex-shrink: 0;
  font-size: 0.62rem;
}

.segmented-meter {
  display: flex;
  gap: 2px;
  flex: 1;
}

.seg {
  flex: 1;
  text-align: center;
  background: #090d16;
  color: var(--text-dim);
  font-size: 0.60rem;
  font-weight: 700;
  padding: 2px 0;
  border-radius: 3px;
}

.seg.active {
  background: var(--cyan);
  color: #000;
}

.lvl-val {
  font-weight: 700;
  font-size: 0.66rem;
  color: var(--cyan);
  width: 70px;
  text-align: right;
}

/* Relay visual monitor */
.relay-locks-grid {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin-bottom: 10px;
  background: #111827;
  padding: 8px;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
}

.relay-lock-item {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.lock-hdr {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-family: var(--font-mono);
  font-size: 0.66rem;
}

.lock-name { color: var(--text-muted); font-weight: 600; }

.lock-state {
  font-weight: 700;
  padding: 1px 5px;
  border-radius: 3px;
  font-size: 0.62rem;
  background: #090d16;
  color: var(--text-dim);
}

.lock-state.running {
  background: rgba(16, 185, 129, 0.16);
  color: #34d399;
}

.lock-bar-track {
  height: 3px;
  background: #090d16;
  border-radius: 2px;
  overflow: hidden;
}

.lock-bar-fill {
  height: 100%;
  background: var(--cyan);
  transition: width 0.3s;
}

.lock-timer {
  font-size: 0.60rem;
  font-family: var(--font-mono);
  color: var(--text-dim);
}

.lock-timer.active { color: var(--amber); }

/* Matrix Tables */
.matrix-table {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.matrix-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 3px 6px;
  border-radius: 4px;
  font-size: 0.70rem;
  font-family: var(--font-mono);
}

.matrix-row:hover { background: rgba(255, 255, 255, 0.02); }

.m-col-pin {
  color: var(--text-dim);
  font-size: 0.65rem;
  font-weight: 600;
  width: 70px;
  flex-shrink: 0;
}

.m-col-name {
  color: #cbd5e1;
  flex: 1;
  padding: 0 6px;
}

.m-col-val {
  font-weight: 600;
  text-align: right;
}

.badge-tag {
  display: inline-block;
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 0.62rem;
  font-weight: 700;
  background: #111827;
  color: var(--text-muted);
  border: 1px solid var(--border-subtle);
}

.badge-tag.submerged, .badge-tag.running, .badge-tag.active {
  background: rgba(16, 185, 129, 0.14);
  color: #34d399;
  border-color: rgba(16, 185, 129, 0.35);
}

.badge-tag.pressed {
  background: rgba(245, 158, 11, 0.18);
  color: var(--amber);
  border-color: rgba(245, 158, 11, 0.4);
}

.badge-tag.ok {
  background: rgba(14, 165, 233, 0.14);
  color: var(--cyan);
  border-color: rgba(14, 165, 233, 0.3);
}

.badge-tag.fault, .badge-tag.alert {
  background: rgba(239, 68, 68, 0.16);
  color: #fca5a5;
  border-color: rgba(239, 68, 68, 0.4);
}

.badge-tag.warn {
  background: rgba(245, 158, 11, 0.16);
  color: var(--amber);
  border-color: rgba(245, 158, 11, 0.4);
}

.badge-tag.idle {
  background: #090d16;
  color: var(--text-dim);
}

/* Radio & Environment Strip */
.radio-signal-strip {
  display: flex;
  gap: 8px;
  margin-bottom: 10px;
}

.sig-card {
  flex: 1;
  background: #111827;
  padding: 8px;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
}

.sig-lbl {
  font-size: 0.60rem;
  font-family: var(--font-mono);
  color: var(--text-dim);
}

.sig-val {
  font-size: 0.82rem;
  font-family: var(--font-mono);
  font-weight: 700;
  color: var(--cyan);
  margin: 2px 0 4px 0;
}

.sig-bar-box {
  height: 3px;
  background: #090d16;
  border-radius: 2px;
  overflow: hidden;
}

.sig-bar-fill {
  height: 100%;
  background: var(--cyan);
}

/* Heap meter */
.heap-meter-box {
  background: #111827;
  padding: 8px;
  border-radius: 6px;
  border: 1px solid var(--border-subtle);
  margin-bottom: 10px;
}

.heap-hdr {
  display: flex;
  justify-content: space-between;
  font-size: 0.64rem;
  font-family: var(--font-mono);
  color: var(--text-muted);
  margin-bottom: 4px;
}

.heap-track {
  height: 4px;
  background: #090d16;
  border-radius: 2px;
  overflow: hidden;
}

.heap-fill {
  height: 100%;
  background: var(--green);
}

/* ─── BOTTOM DIAGNOSTICS FOOTER ────────────────────────────────── */
.hud-footer {
  padding: 10px 18px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 12px;
  font-size: 0.72rem;
  font-family: var(--font-mono);
}

.diag-item { display: flex; gap: 6px; }
.diag-k { color: var(--text-dim); }
.diag-v { color: #cbd5e1; font-weight: 700; }

/* ─── SETTINGS MODAL DRAWER ────────────────────────────────────── */
.modal-backdrop {
  position: fixed;
  top: 0; left: 0; width: 100%; height: 100%;
  background: rgba(0, 0, 0, 0.75);
  backdrop-filter: blur(4px);
  display: none;
  align-items: center;
  justify-content: center;
  z-index: 100;
  padding: 16px;
}

.modal-backdrop.open { display: flex; }

.modal-card {
  width: 100%;
  max-width: 680px;
  padding: 20px;
  background: #111827;
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 12px;
  display: flex;
  flex-direction: column;
  gap: 14px;
  max-height: 90vh;
  overflow-y: auto;
}

.modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid var(--border-subtle);
  padding-bottom: 8px;
}

.modal-header h2 {
  font-size: 0.96rem;
  letter-spacing: 0.05em;
  font-family: var(--font-mono);
  color: var(--text-main);
}

.modal-body {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin-top: 6px;
}

.badge-icon {
  font-size: 0.85rem;
  display: inline-flex;
  align-items: center;
}

.subcard-icon {
  font-size: 1.1rem;
  display: inline-flex;
  align-items: center;
}

.highlight {
  color: var(--cyan);
  font-weight: 700;
}

/* Settings Tabs */
.settings-tabs-nav {
  display: flex;
  gap: 4px;
  background: #090d16;
  padding: 4px;
  border-radius: 8px;
  border: 1px solid var(--border-subtle);
  flex-wrap: wrap;
}

.settings-tab-btn {
  background: transparent;
  border: none;
  color: var(--text-muted);
  padding: 6px 12px;
  font-family: var(--font-mono);
  font-size: 0.72rem;
  font-weight: 600;
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.16s ease;
}

.settings-tab-btn:hover { color: #fff; }

.settings-tab-btn.active {
  background: #1e293b;
  color: #fff;
}

.settings-tab-pane { display: none; }
.settings-tab-pane.active { display: block; }

.form-grid-2 {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
  margin-bottom: 8px;
}

@media (max-width: 580px) {
  .form-grid-2 { grid-template-columns: 1fr; }
}

.form-row {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-bottom: 8px;
}

.form-row label {
  font-size: 0.72rem;
  font-family: var(--font-mono);
  color: var(--text-muted);
}

.form-row input[type="number"],
.form-row input[type="text"],
.form-row input[type="password"],
.form-row select {
  background: #090d16;
  border: 1px solid var(--border-subtle);
  color: var(--text-main);
  padding: 7px 10px;
  border-radius: 6px;
  font-family: var(--font-mono);
  font-size: 0.75rem;
}

.form-row input:focus, .form-row select:focus {
  border-color: var(--cyan);
  outline: none;
}

.checkbox-row {
  flex-direction: row;
  align-items: center;
  gap: 8px;
}

.checkbox-row input[type="checkbox"] {
  width: 16px;
  height: 16px;
  accent-color: var(--cyan);
  cursor: pointer;
}

.section-help {
  font-size: 0.68rem;
  color: var(--text-dim);
  line-height: 1.35;
  margin-bottom: 8px;
}

.modal-actions {
  display: flex;
  gap: 8px;
  margin-top: 12px;
}

.btn {
  padding: 8px 14px;
  border-radius: 6px;
  font-weight: 700;
  font-size: 0.74rem;
  font-family: var(--font-mono);
  cursor: pointer;
  border: none;
  transition: all 0.16s ease;
}

.btn-primary {
  background: #0284c7;
  color: #fff;
}

.btn-primary:hover {
  background: #0369a1;
}

.btn-secondary {
  background: #1e293b;
  color: #e2e8f0;
  border: 1px solid var(--border-subtle);
}

.btn-danger {
  background: rgba(239, 68, 68, 0.15);
  color: #f87171;
  border: 1px solid rgba(239, 68, 68, 0.3);
}

/* Toast Notifications */
.toast-container {
  position: fixed;
  bottom: 20px;
  right: 20px;
  display: flex;
  flex-direction: column;
  gap: 8px;
  z-index: 200;
}

.toast {
  background: #090d16;
  border-left: 3px solid var(--cyan);
  border: 1px solid var(--border-subtle);
  border-left-width: 3px;
  border-left-color: var(--cyan);
  padding: 10px 14px;
  border-radius: 6px;
  font-size: 0.75rem;
  font-family: var(--font-mono);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.6);
  color: #f1f5f9;
}

.toast.error {
  border-left-color: var(--red);
  color: #fca5a5;
}

/* Log Box */
.log-box {
  background: #090d16;
  border: 1px solid var(--border-subtle);
  border-radius: 8px;
  height: 220px;
  overflow-y: auto;
  padding: 8px 10px;
  font-family: var(--font-mono);
  font-size: 0.68rem;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.log-entry {
  display: flex;
  gap: 6px;
  line-height: 1.35;
}

.log-ts { color: var(--text-dim); flex-shrink: 0; }
.log-cat { font-weight: 700; padding: 0 3px; border-radius: 3px; font-size: 0.62rem; }
.log-cat-SYS { background: #1e293b; color: #94a3b8; }
.log-cat-LORA { background: rgba(14, 165, 233, 0.18); color: var(--cyan); }
.log-cat-PUMP { background: rgba(245, 158, 11, 0.18); color: var(--amber); }
.log-cat-HYDR { background: rgba(16, 185, 129, 0.18); color: var(--green); }
.log-cat-WIFI { background: rgba(139, 92, 246, 0.18); color: var(--purple); }
.log-cat-OTA { background: rgba(239, 68, 68, 0.18); color: #f87171; }
.log-msg { color: #cbd5e1; word-break: break-all; }

/* OTA Progress Bar */
.prog-bar {
  height: 8px;
  background: #1e293b;
  border-radius: 4px;
  overflow: hidden;
  margin: 10px 0;
}

.prog-fill {
  height: 100%;
  width: 0%;
  background: var(--cyan);
  transition: width 0.2s;
}

/* ─── QUICK NAVIGATION & OPERATOR ACTION BAR ─────────────────────── */
.quick-nav-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 14px;
  gap: 10px;
  flex-wrap: wrap;
  background: linear-gradient(90deg, rgba(17, 24, 39, 0.95), rgba(15, 23, 42, 0.95));
  border: 1px solid rgba(14, 165, 233, 0.18);
}

.view-tab-group {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}

.view-tab-btn {
  background: #0b0f19;
  border: 1px solid var(--border-subtle);
  color: var(--text-muted);
  padding: 6px 12px;
  border-radius: 7px;
  font-size: 0.72rem;
  font-family: var(--font-mono);
  font-weight: 600;
  cursor: pointer;
  transition: all 0.15s ease;
}

.view-tab-btn:hover {
  color: var(--text-main);
  border-color: rgba(255, 255, 255, 0.2);
  background: #162032;
}

.view-tab-btn.active {
  background: rgba(14, 165, 233, 0.16);
  border-color: var(--cyan);
  color: #38bdf8;
  box-shadow: 0 0 12px rgba(14, 165, 233, 0.18);
}

.quick-action-group {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.quick-rf-pill {
  display: flex;
  align-items: center;
  gap: 7px;
  background: #090d16;
  border: 1px solid rgba(14, 165, 233, 0.22);
  padding: 5px 10px;
  border-radius: 6px;
  font-family: var(--font-mono);
  font-size: 0.69rem;
  color: #cbd5e1;
}

.qrf-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--green);
  box-shadow: 0 0 6px var(--green-glow);
}

.qrf-dot.burst {
  background: var(--cyan);
  box-shadow: 0 0 8px var(--cyan-glow);
  animation: pulseBeacon 0.9s infinite alternate;
}

.quick-sf-select {
  background: #090d16;
  border: 1px solid rgba(139, 92, 246, 0.35);
  color: #e2e8f0;
  padding: 5px 8px;
  border-radius: 6px;
  font-family: var(--font-mono);
  font-size: 0.71rem;
  font-weight: 600;
  cursor: pointer;
}

.quick-sf-select:focus {
  border-color: var(--cyan);
  outline: none;
}

.q-act-btn {
  padding: 6px 11px;
  border-radius: 6px;
  font-family: var(--font-mono);
  font-size: 0.70rem;
  font-weight: 700;
  cursor: pointer;
  transition: all 0.15s ease;
  border: 1px solid transparent;
}

.q-act-btn:active {
  transform: scale(0.96);
}

.q-act-cyan {
  background: rgba(14, 165, 233, 0.14);
  border-color: rgba(14, 165, 233, 0.4);
  color: #38bdf8;
}
.q-act-cyan:hover {
  background: rgba(14, 165, 233, 0.26);
  color: #fff;
}

.q-act-green {
  background: rgba(16, 185, 129, 0.14);
  border-color: rgba(16, 185, 129, 0.4);
  color: #34d399;
}
.q-act-green:hover {
  background: rgba(16, 185, 129, 0.26);
  color: #fff;
}

.q-act-red {
  background: rgba(239, 68, 68, 0.15);
  border-color: rgba(239, 68, 68, 0.45);
  color: #f87171;
}
.q-act-red:hover {
  background: rgba(239, 68, 68, 0.3);
  color: #fff;
}

/* ─── POND DUAL-STATE ACCORD BADGES ──────────────────────────────── */
.pond-badges-stack {
  display: flex;
  flex-direction: column;
  gap: 4px;
  align-items: flex-end;
}

.pond-accord-badge {
  font-family: var(--font-mono);
  font-size: 0.64rem;
  font-weight: 700;
  padding: 3px 8px;
  border-radius: 5px;
  background: rgba(148, 163, 184, 0.1);
  border: 1px solid rgba(148, 163, 184, 0.22);
  color: var(--text-muted);
  letter-spacing: 0.03em;
}

.pond-accord-badge.synced-on {
  background: rgba(16, 185, 129, 0.15);
  border-color: rgba(16, 185, 129, 0.45);
  color: #34d399;
}

.pond-accord-badge.syncing {
  background: rgba(245, 158, 11, 0.16);
  border-color: rgba(245, 158, 11, 0.45);
  color: #fbbf24;
  animation: pulseBeacon 0.9s infinite alternate;
}

.pond-toggle-btn.active-demand {
  background: rgba(239, 68, 68, 0.18);
  border-color: rgba(239, 68, 68, 0.5);
  color: #fca5a5;
}
.pond-toggle-btn.active-demand:hover {
  background: rgba(239, 68, 68, 0.32);
  color: #fff;
}


</style>
</head>
<body>
  <div class="app-container">
    <!-- Top HUD Navigation Header -->
    <header class="hud-header glass-card">
      <div class="brand-group">
        <div class="pulse-indicator online" id="wsIndicator"></div>
        <div class="brand-titles">
          <h1>TANK NODE <span class="tag">BAZIN APĂ</span></h1>
          <p class="subtitle">ESP32-S3 + SX1262 LoRa Telemetry System</p>
        </div>
      </div>

      <div class="hud-badges">
        <div class="badge" title="Connection Mode">
          <span class="badge-icon">📡</span>
          <span id="badgeConn">SoftAP (192.168.4.1)</span>
        </div>
        <div class="badge" title="LoRa Link Quality">
          <span class="badge-icon">📶</span>
          <span id="badgeRssi">RSSI: -- dBm</span>
        </div>
        <div class="badge" title="LoRa TX/RX Packet Counter (Event-Driven)">
          <span class="badge-icon">⚡</span>
          <span id="badgePktCnt">TX: 0 | RX: 0</span>
        </div>
        <div class="badge" title="System Uptime">
          <span class="badge-icon">⏱️</span>
          <span id="badgeUptime">Up: 0s</span>
        </div>
        <button class="icon-btn" id="btnTxNowHeader" onclick="triggerManualBeacon()" title="Transmit Immediate LoRa Status Beacon">📤</button>
        <button class="icon-btn" id="btnMatrixHeader" onclick="toggleMatrix()" title="Toggle Live Parameters & I/O Matrix">📊</button>
        <button class="icon-btn" id="btnAudioToggle" title="Toggle Sound FX">🔊</button>
        <button class="icon-btn" id="btnSettingsOpen" title="Settings & Network">⚙️</button>
      </div>
    </header>

    <!-- Quick Navigation & Operator Action Bar -->
    <nav class="quick-nav-bar glass-card">
      <div class="view-tab-group">
        <button class="view-tab-btn active" id="vTabAll" onclick="switchDashboardView('all')">🎛️ Full SCADA Deck</button>
        <button class="view-tab-btn" id="vTabOps" onclick="switchDashboardView('ops')">💧 Hydraulic Control</button>
        <button class="view-tab-btn" id="vTabMatrix" onclick="switchDashboardView('matrix')">⚡ Hardware I/O &amp; RF</button>
        <button class="view-tab-btn" id="vTabRules" onclick="switchDashboardView('rules')">📋 Automation Rules</button>
      </div>
      <div class="quick-action-group">
        <div class="quick-rf-pill" id="quickRfBar" title="Active LoRa Modulation, LDRO, and Time-on-Air">
          <span class="qrf-dot" id="qrfDot"></span>
          <span id="quickRfText">SF9 &bull; BW125 &bull; +22dBm &bull; ToA: --ms</span>
        </div>
        <select class="quick-sf-select" id="quickSfSelect" onchange="quickChangeSf(this.value)" title="Instant Spreading Factor Switch">
          <option value="7">SF7 (Fast)</option>
          <option value="8">SF8</option>
          <option value="9" selected>SF9 (Bal)</option>
          <option value="10">SF10 (700m+)</option>
          <option value="11">SF11 (LDRO)</option>
          <option value="12">SF12 (Max)</option>
        </select>
        <button class="q-act-btn q-act-cyan" onclick="triggerManualBeacon()" title="Transmit 1 Status Packet Now">📡 Beacon TX</button>
        <button class="q-act-btn q-act-green" onclick="resetAllOverrides()" title="Return P1, P2, P3 to AUTO Schedule">🔄 All AUTO</button>
        <button class="q-act-btn q-act-red" onclick="emergencyStopPumps()" title="Lock OFF all 3 Outflow Pumps immediately">🛑 E-Stop P1-P3</button>
      </div>
    </nav>

    <!-- ISA-18.2 Priority Alarm & Operational Status Banner -->
    <div class="alarm-banner-container glass-card" id="alarmBanner">
      <div class="alarm-left">
        <div class="alarm-beacon normal" id="alarmBeacon"></div>
        <div class="alarm-text-block">
          <div class="alarm-title" id="alarmTitle">NORMAL OPERATION &bull; INTERLOCKS NOMINAL</div>
          <div class="alarm-desc" id="alarmDesc">Water level within operating limits. Anti-inrush start stagger (3s) and anti-chatter locks active.</div>
        </div>
      </div>
      <div class="alarm-right-metrics">
        <div class="banner-pill" title="Current Automation Rule active on ESP32-S3"><span class="bp-label">RULE:</span> <span class="bp-val" id="bannerActiveRule">RULE 2 (MID WL)</span></div>
        <div class="banner-pill" title="Pumps with active manual overrides"><span class="bp-label">OVERRIDES:</span> <span class="bp-val" id="bannerOverrides">NONE (AUTO)</span></div>
        <div class="banner-pill" title="LoRa remote pond refill latch"><span class="bp-label">POND REFILL:</span> <span class="bp-val" id="bannerLatch">IDLE</span></div>
        <div class="banner-pill" title="Event-Driven Radio Telemetry State"><span class="bp-label">RADIO MODE:</span> <span class="bp-val" id="bannerRadioMode">SILENT IDLE</span></div>
      </div>
    </div>

    <!-- Main Grid -->
    <main class="main-grid">
      <!-- Left Column: Fluid Tank Reservoir & Mechanical Simulation -->
      <section class="reservoir-card glass-card">
        <div class="card-header">
          <div class="card-header-left">
            <h2>HYDRAULIC PROCESS SCHEMATIC (P&amp;ID)</h2>
            <p class="section-sub">Direct Inflow &bull; 10,000L Storage &bull; 3 Parallel Outflow Lines (Direct Suction, No Check Valves / No PID)</p>
          </div>
          <span class="level-display" id="tankLevelText">WL: 0 (EMPTY 0%)</span>
        </div>

        <div class="tank-stage">
          <!-- Inflow pipe from remote pond pump (top left) -->
          <div class="pipe-inflow-container" id="inflowContainer">
            <div class="pipe inflow-pipe">
              <div class="pipe-label">INFLOW (POND 0x03)</div>
              <div class="pipe-flow-water" id="inflowPipeWater"></div>
            </div>
            <!-- Animated Water Jet into Reservoir -->
            <div class="water-jet" id="waterJet">
              <div class="splash-ring"></div>
            </div>
          </div>

          <!-- Outflow pipes to field (bottom right) -->
          <div class="pipe-outflow-group">
            <div class="pipe outflow-pipe" id="outflowP1">
              <span class="p-tag">P1</span>
              <div class="turbine" id="turbineP1"></div>
              <div class="pipe-stream" id="streamP1"></div>
              <span class="field-line-label">LINE A</span>
            </div>
            <div class="pipe outflow-pipe" id="outflowP2">
              <span class="p-tag">P2</span>
              <div class="turbine" id="turbineP2"></div>
              <div class="pipe-stream" id="streamP2"></div>
              <span class="field-line-label">LINE B</span>
            </div>
            <div class="pipe outflow-pipe" id="outflowP3">
              <span class="p-tag">P3</span>
              <div class="turbine" id="turbineP3"></div>
              <div class="pipe-stream" id="streamP3"></div>
              <span class="field-line-label">LINE C</span>
            </div>
          </div>

          <!-- Physical Reservoir Tank Vessel -->
          <div class="tank-vessel">
            <!-- Glass reflection layer -->
            <div class="tank-reflection"></div>

            <!-- Float Switch Sensor Indicators inside the tank -->
            <div class="sensor-buoy" id="buoyL3" style="bottom: 78%;">
              <div class="sensor-bead" id="beadL3"></div>
              <div class="sensor-wire"></div>
              <span class="sensor-label">L3 (High 100%)</span>
            </div>
            <div class="sensor-buoy" id="buoyL2" style="bottom: 50%;">
              <div class="sensor-bead" id="beadL2"></div>
              <div class="sensor-wire"></div>
              <span class="sensor-label">L2 (Mid 66%)</span>
            </div>
            <div class="sensor-buoy" id="buoyL1" style="bottom: 22%;">
              <div class="sensor-bead" id="beadL1"></div>
              <div class="sensor-wire"></div>
              <span class="sensor-label">L1 (Low 33%)</span>
            </div>

            <!-- Dynamic Liquid Simulation Body -->
            <div class="liquid-body" id="liquidBody" style="height: 0%;">
              <!-- SVG Wave Crest Surface -->
              <svg class="wave-svg" viewBox="0 0 1200 120" preserveAspectRatio="none">
                <path class="wave-path wave-back" d="M0,40 C150,90 350,-20 500,40 C650,100 900,-10 1200,40 L1200,120 L0,120 Z"></path>
                <path class="wave-path wave-front" d="M0,50 C200,0 450,100 650,50 C850,0 1050,90 1200,50 L1200,120 L0,120 Z"></path>
              </svg>

              <!-- Rising air bubbles inside water -->
              <div class="bubble b1"></div>
              <div class="bubble b2"></div>
              <div class="bubble b3"></div>
              <div class="bubble b4"></div>
              <div class="bubble b5"></div>
            </div>
          </div>

          <!-- Tank Vessel Markings Ruler -->
          <div class="tank-ruler">
            <span class="mark" style="bottom: 95%;">100% (10,000L)</span>
            <span class="mark" style="bottom: 66%;">66% (6,600L)</span>
            <span class="mark" style="bottom: 33%;">33% (3,300L)</span>
            <span class="mark" style="bottom: 5%;">0% (&lt;1,000L)</span>
          </div>
        </div>

        <!-- Sensor Status & Fault Alert Banner -->
        <div class="sensor-status-bar" id="sensorAlertBar">
          <span class="sensor-status-dot" id="sensorHealthDot"></span>
          <span id="sensorHealthText">Float switches healthy &bull; Direct suction manifold</span>
        </div>
      </section>

      <!-- Right Column: System Controls & Diagnostics Deck -->
      <section class="control-deck">
        <!-- Operation Mode Pill Switcher -->
        <div class="mode-card glass-card">
          <div class="mode-header">
            <div>
              <h3>MASTER OPERATION MODE</h3>
              <p class="section-sub">Autonomous Schedule vs Manual Control</p>
            </div>
            <span class="mode-status-text" id="modeBanner">MANUAL MODE</span>
          </div>
          <div class="mode-toggle-group">
            <button class="mode-btn" id="btnModeAuto" onclick="setMode(true)">AUTOMATIC</button>
            <button class="mode-btn active" id="btnModeMan" onclick="setMode(false)">MANUAL</button>
          </div>
          <p class="mode-hint" id="modeHint">Individual motor overrides take precedence. Anti-chatter (20s) and anti-inrush stagger (3s) enforced.</p>
        </div>

        <!-- Irrigation Outflow Pumps (P1 - P3) Industrial Faceplates -->
        <div class="pumps-grid">
          <!-- Pump 1 Faceplate -->
          <div class="pump-card glass-card" id="cardP1">
            <div class="pump-card-header">
              <div class="pump-title">
                <span class="pump-indicator" id="indP1"></span>
                <div>
                  <h4>PUMP 1 (LINE A)</h4>
                  <span class="pump-sub-tag">GPIO 47 &bull; RELAY 1</span>
                </div>
              </div>
              <span class="status-pill" id="pillP1">IDLE / READY</span>
            </div>

            <!-- 3-Position Override Controller -->
            <div class="override-panel">
              <span class="override-label">MOTOR OVERRIDE:</span>
              <div class="override-btn-group">
                <button class="btn-override btn-ovr-auto active" id="ovrAutoP1" onclick="setPumpOverride(1, 'AUTO')" title="Follow automatic water level schedule">AUTO</button>
                <button class="btn-override btn-ovr-on" id="ovrOnP1" onclick="setPumpOverride(1, 'FORCE_ON')" title="Force motor ON immediately">FORCE ON</button>
                <button class="btn-override btn-ovr-off" id="ovrOffP1" onclick="setPumpOverride(1, 'FORCE_OFF')" title="Hard lockout - prevent motor from starting">LOCK OFF</button>
              </div>
            </div>

            <!-- Timed Dosage Duration Selector -->
            <div class="timed-run-row">
              <span class="timed-label">⏱️ Duration:</span>
              <select class="timed-select" id="durP1">
                <option value="0">Continuous</option>
                <option value="5">5 min</option>
                <option value="15">15 min</option>
                <option value="30">30 min</option>
                <option value="60">60 min</option>
              </select>
              <span class="timed-active-badge" id="timedBadgeP1" style="display:none;">00:00</span>
            </div>

            <!-- Feedback & Lock Countdown Meters -->
            <div class="pump-feedback-box">
              <div class="feedback-gauge" id="gaugeBoxP1">
                <div class="gauge-hdr">
                  <span class="gauge-title" id="gaugeLblP1">INTERLOCK:</span>
                  <span class="gauge-val" id="gaugeValP1">Ready</span>
                </div>
                <div class="gauge-track"><div class="gauge-fill" id="gaugeFillP1" style="width:0%;"></div></div>
              </div>
            </div>

            <!-- Bottom Action & Auto Eligibility -->
            <div class="pump-footer">
              <label class="auto-mask-check" title="Allowed in Auto Mode">
                <input type="checkbox" id="maskP1" onchange="updateAutoMask()">
                <span>AUTO MASK</span>
              </label>
              <button class="pump-toggle-btn" id="btnP1" onclick="togglePump(1)">START P1</button>
            </div>
            <div class="pump-odometer" id="odoP1">Runtime: 0h 00m &bull; Total: 0s</div>
          </div>

          <!-- Pump 2 Faceplate -->
          <div class="pump-card glass-card" id="cardP2">
            <div class="pump-card-header">
              <div class="pump-title">
                <span class="pump-indicator" id="indP2"></span>
                <div>
                  <h4>PUMP 2 (LINE B)</h4>
                  <span class="pump-sub-tag">GPIO 41 &bull; RELAY 2</span>
                </div>
              </div>
              <span class="status-pill" id="pillP2">IDLE / READY</span>
            </div>

            <!-- 3-Position Override Controller -->
            <div class="override-panel">
              <span class="override-label">MOTOR OVERRIDE:</span>
              <div class="override-btn-group">
                <button class="btn-override btn-ovr-auto active" id="ovrAutoP2" onclick="setPumpOverride(2, 'AUTO')" title="Follow automatic water level schedule">AUTO</button>
                <button class="btn-override btn-ovr-on" id="ovrOnP2" onclick="setPumpOverride(2, 'FORCE_ON')" title="Force motor ON immediately">FORCE ON</button>
                <button class="btn-override btn-ovr-off" id="ovrOffP2" onclick="setPumpOverride(2, 'FORCE_OFF')" title="Hard lockout - prevent motor from starting">LOCK OFF</button>
              </div>
            </div>

            <!-- Timed Dosage Duration Selector -->
            <div class="timed-run-row">
              <span class="timed-label">⏱️ Duration:</span>
              <select class="timed-select" id="durP2">
                <option value="0">Continuous</option>
                <option value="5">5 min</option>
                <option value="15">15 min</option>
                <option value="30">30 min</option>
                <option value="60">60 min</option>
              </select>
              <span class="timed-active-badge" id="timedBadgeP2" style="display:none;">00:00</span>
            </div>

            <!-- Feedback & Lock Countdown Meters -->
            <div class="pump-feedback-box">
              <div class="feedback-gauge" id="gaugeBoxP2">
                <div class="gauge-hdr">
                  <span class="gauge-title" id="gaugeLblP2">INTERLOCK:</span>
                  <span class="gauge-val" id="gaugeValP2">Ready</span>
                </div>
                <div class="gauge-track"><div class="gauge-fill" id="gaugeFillP2" style="width:0%;"></div></div>
              </div>
            </div>

            <!-- Bottom Action & Auto Eligibility -->
            <div class="pump-footer">
              <label class="auto-mask-check" title="Allowed in Auto Mode">
                <input type="checkbox" id="maskP2" onchange="updateAutoMask()">
                <span>AUTO MASK</span>
              </label>
              <button class="pump-toggle-btn" id="btnP2" onclick="togglePump(2)">START P2</button>
            </div>
            <div class="pump-odometer" id="odoP2">Runtime: 0h 00m &bull; Total: 0s</div>
          </div>

          <!-- Pump 3 Faceplate -->
          <div class="pump-card glass-card" id="cardP3">
            <div class="pump-card-header">
              <div class="pump-title">
                <span class="pump-indicator" id="indP3"></span>
                <div>
                  <h4>PUMP 3 (LINE C)</h4>
                  <span class="pump-sub-tag">GPIO 39 &bull; RELAY 3</span>
                </div>
              </div>
              <span class="status-pill" id="pillP3">IDLE / READY</span>
            </div>

            <!-- 3-Position Override Controller -->
            <div class="override-panel">
              <span class="override-label">MOTOR OVERRIDE:</span>
              <div class="override-btn-group">
                <button class="btn-override btn-ovr-auto active" id="ovrAutoP3" onclick="setPumpOverride(3, 'AUTO')" title="Follow automatic water level schedule">AUTO</button>
                <button class="btn-override btn-ovr-on" id="ovrOnP3" onclick="setPumpOverride(3, 'FORCE_ON')" title="Force motor ON immediately">FORCE ON</button>
                <button class="btn-override btn-ovr-off" id="ovrOffP3" onclick="setPumpOverride(3, 'FORCE_OFF')" title="Hard lockout - prevent motor from starting">LOCK OFF</button>
              </div>
            </div>

            <!-- Timed Dosage Duration Selector -->
            <div class="timed-run-row">
              <span class="timed-label">⏱️ Duration:</span>
              <select class="timed-select" id="durP3">
                <option value="0">Continuous</option>
                <option value="5">5 min</option>
                <option value="15">15 min</option>
                <option value="30">30 min</option>
                <option value="60">60 min</option>
              </select>
              <span class="timed-active-badge" id="timedBadgeP3" style="display:none;">00:00</span>
            </div>

            <!-- Feedback & Lock Countdown Meters -->
            <div class="pump-feedback-box">
              <div class="feedback-gauge" id="gaugeBoxP3">
                <div class="gauge-hdr">
                  <span class="gauge-title" id="gaugeLblP3">INTERLOCK:</span>
                  <span class="gauge-val" id="gaugeValP3">Ready</span>
                </div>
                <div class="gauge-track"><div class="gauge-fill" id="gaugeFillP3" style="width:0%;"></div></div>
              </div>
            </div>

            <!-- Bottom Action & Auto Eligibility -->
            <div class="pump-footer">
              <label class="auto-mask-check" title="Allowed in Auto Mode">
                <input type="checkbox" id="maskP3" onchange="updateAutoMask()">
                <span>AUTO MASK</span>
              </label>
              <button class="pump-toggle-btn" id="btnP3" onclick="togglePump(3)">START P3</button>
            </div>
            <div class="pump-odometer" id="odoP3">Runtime: 0h 00m &bull; Total: 0s</div>
          </div>
        </div>

        <!-- Remote Pond Inflow Pump Controller -->
        <div class="pond-card glass-card" id="cardPond">
          <div class="pond-info">
            <div class="pump-title">
              <span class="pump-indicator pond-ind" id="indPond"></span>
              <div>
                <h4>REMOTE POND REFILL PUMP</h4>
                <span class="pump-sub-tag">SX1262 LoRa Node 0x03 &bull; EU868 Accord</span>
              </div>
            </div>
            <p class="pond-desc" id="pondDesc">Refills reservoir via telemetry demand packet. Auto trigger at &le; L1, cutoff at &ge; L3.</p>
          </div>
          <div class="pond-actions">
            <div class="pond-badges-stack">
              <div class="charge-latch-badge" id="latchBadge">DEMAND: IDLE</div>
              <div class="pond-accord-badge" id="pondAccordBadge">0x03 ACK: STANDBY</div>
            </div>
            <button class="pond-toggle-btn" id="btnPond" onclick="togglePond()">REQUEST REFILL</button>
          </div>
        </div>

        <!-- Live Canvas Signal & Level Sparkline -->
        <div class="sparkline-card glass-card">
          <div class="sparkline-header">
            <h3>LIVE TELEMETRY TRENDS</h3>
            <span class="sparkline-legend">
              <span class="leg-dot level-dot"></span> Water Level
              <span class="leg-dot rssi-dot"></span> RSSI
            </span>
          </div>
          <canvas id="sparklineCanvas" width="560" height="110"></canvas>
        </div>
      </section>
    </main>

    <!-- "What Happens When & How" Hydraulic Automation Matrix -->
    <section class="automation-matrix-card glass-card" id="automationMatrixSection">
      <div class="matrix-card-header">
        <div class="matrix-title-group">
          <span class="matrix-icon">⚙️</span>
          <div>
            <h3>HYDRAULIC AUTOMATION MATRIX ("WHAT HAPPENS WHEN &amp; HOW")</h3>
            <p class="matrix-sub">Firmware state-machine decision table with real-time active rule tracking</p>
          </div>
        </div>
        <div class="matrix-active-tag" id="activeRuleBadge">ACTIVE: RULE 2 (MID WL)</div>
      </div>

      <div class="automation-table-wrap">
        <table class="automation-table">
          <thead>
            <tr>
              <th>OPERATIONAL TIER</th>
              <th>WATER LEVEL</th>
              <th>ESTIMATED VOLUME</th>
              <th>POND REFILL (INFLOW)</th>
              <th>OUTFLOW PUMPS (AUTO)</th>
              <th>CONTROL ACTION &amp; SAFETY INTERLOCK</th>
              <th>STATUS</th>
            </tr>
          </thead>
          <tbody>
            <tr id="ruleRow1" class="rule-row">
              <td class="rule-col-tier"><span class="rule-num">RULE 1</span> <strong>FULL RESERVOIR</strong></td>
              <td class="rule-col-wl"><span class="badge-tag ok">LEVEL 3 (100%)</span></td>
              <td class="rule-col-vol">~10,000 Liters</td>
              <td class="rule-col-pond"><span class="badge-tag idle">AUTO CUTOFF (OFF)</span></td>
              <td class="rule-col-pumps"><span class="badge-tag running">ALL 3 PUMPS (P1, P2, P3)</span></td>
              <td class="rule-col-desc">Maximum irrigation capacity. Pond refill cut off to prevent overflow.</td>
              <td class="rule-col-active"><span class="rule-status-badge" id="ruleBadge1">IDLE</span></td>
            </tr>
            <tr id="ruleRow2" class="rule-row">
              <td class="rule-col-tier"><span class="rule-num">RULE 2</span> <strong>NOMINAL STORAGE</strong></td>
              <td class="rule-col-wl"><span class="badge-tag active">LEVEL 2 (66%)</span></td>
              <td class="rule-col-vol">~6,600 Liters</td>
              <td class="rule-col-pond"><span class="badge-tag idle">STANDBY / IDLE</span></td>
              <td class="rule-col-pumps"><span class="badge-tag running">2 PUMPS (P1, P2)</span></td>
              <td class="rule-col-desc">Sustainable balanced flow. P3 throttled unless manually forced on.</td>
              <td class="rule-col-active"><span class="rule-status-badge" id="ruleBadge2">IDLE</span></td>
            </tr>
            <tr id="ruleRow3" class="rule-row">
              <td class="rule-col-tier"><span class="rule-num">RULE 3</span> <strong>LOW WATER</strong></td>
              <td class="rule-col-wl"><span class="badge-tag warn">LEVEL 1 (33%)</span></td>
              <td class="rule-col-vol">~3,300 Liters</td>
              <td class="rule-col-pond"><span class="badge-tag ok">REFILL DEMAND ON</span></td>
              <td class="rule-col-pumps"><span class="badge-tag running">1 PUMP (P1 ONLY)</span></td>
              <td class="rule-col-desc">Conservation mode. Tank charges from pond via LoRa packet demand.</td>
              <td class="rule-col-active"><span class="rule-status-badge" id="ruleBadge3">IDLE</span></td>
            </tr>
            <tr id="ruleRow4" class="rule-row trip-row">
              <td class="rule-col-tier"><span class="rule-num">RULE 4</span> <strong>EMPTY / DRY RUN</strong></td>
              <td class="rule-col-wl"><span class="badge-tag fault">LEVEL 0 (0%)</span></td>
              <td class="rule-col-vol">&lt; 1,000 Liters</td>
              <td class="rule-col-pond"><span class="badge-tag fault">PRIORITY CHARGE LATCH</span></td>
              <td class="rule-col-pumps"><span class="badge-tag fault">HARD SHUTDOWN (0 PUMPS)</span></td>
              <td class="rule-col-desc"><strong>DRY RUN EMERGENCY TRIP:</strong> All pumps locked out to protect against cavitation.</td>
              <td class="rule-col-active"><span class="rule-status-badge" id="ruleBadge4">IDLE</span></td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- Comprehensive Live I/O, Actuators & Parameters SCADA Inspector Matrix -->
    <section class="matrix-card glass-card" id="ioMatrixSection">
      <div class="matrix-header">
        <div class="matrix-title-group">
          <span class="matrix-icon">⚡</span>
          <div>
            <h3>LIVE SYSTEM I/O & PARAMETERS MATRIX</h3>
            <p class="matrix-sub">Real-time hardware registers, digital pin states, sensor readings & NVS parameters</p>
          </div>
        </div>
        <div class="matrix-header-right">
          <span class="matrix-delta-pill" id="mDeltaTicker">ALL SIGNALS STABLE</span>
          <span class="matrix-live-pill"><span class="matrix-pulse"></span>LIVE (500ms)</span>
          <button class="matrix-act-btn" onclick="copyTelemetryJson()" title="Copy raw telemetry JSON to clipboard">📋 COPY JSON</button>
          <button class="matrix-act-btn" onclick="sendJson({cmd:'get_config'})" title="Fetch Config from ESP32">🔄 REFRESH</button>
          <button class="matrix-toggle-btn" id="btnToggleMatrix" onclick="toggleMatrix()">COLLAPSE</button>
        </div>
      </div>

      <div class="matrix-content" id="matrixContent">
        <!-- 1. Physical Digital Inputs (PCF8574 Port 0x27) -->
        <div class="matrix-subcard">
          <div class="subcard-title">
            <span class="subcard-icon">🔌</span>
            <h4>PHYSICAL INPUTS (PCF8574 @ 0x27)</h4>
            <span class="subcard-badge" id="mPcfRaw">0xFF (0b11111111)</span>
          </div>

          <!-- 8-Bit Visual Pin Bus Rack -->
          <div class="pin-bus-rack" id="pinBusRack">
            <div class="pin-chip" id="chipP0" title="Float 1 Contact (Low WL)"><span class="pin-num">P0</span><span class="pin-dot"></span><span class="pin-lbl">FS1</span></div>
            <div class="pin-chip" id="chipP1" title="Float 2 Contact (Mid WL)"><span class="pin-num">P1</span><span class="pin-dot"></span><span class="pin-lbl">FS2</span></div>
            <div class="pin-chip" id="chipP2" title="Float 3 Contact (High WL)"><span class="pin-num">P2</span><span class="pin-dot"></span><span class="pin-lbl">FS3</span></div>
            <div class="pin-chip" id="chipP3" title="Button Mode Toggle"><span class="pin-num">P3</span><span class="pin-dot"></span><span class="pin-lbl">MODE</span></div>
            <div class="pin-chip" id="chipP4" title="Button Pump 1 Toggle"><span class="pin-num">P4</span><span class="pin-dot"></span><span class="pin-lbl">P1</span></div>
            <div class="pin-chip" id="chipP5" title="Button Pump 2 Toggle"><span class="pin-num">P5</span><span class="pin-dot"></span><span class="pin-lbl">P2</span></div>
            <div class="pin-chip" id="chipP6" title="Button Pump 3 Toggle"><span class="pin-num">P6</span><span class="pin-dot"></span><span class="pin-lbl">P3</span></div>
            <div class="pin-chip" id="chipP7" title="Button Pond Toggle"><span class="pin-num">P7</span><span class="pin-dot"></span><span class="pin-lbl">POND</span></div>
          </div>

          <!-- Dual Visual Level Comparison Bar -->
          <div class="level-compare-box">
            <div class="lvl-track">
              <span class="lvl-title">RAW INPUT:</span>
              <div class="segmented-meter" id="meterRawLvl">
                <div class="seg" id="segRaw0">L0</div>
                <div class="seg" id="segRaw1">L1</div>
                <div class="seg" id="segRaw2">L2</div>
                <div class="seg" id="segRaw3">L3</div>
              </div>
              <span class="lvl-val" id="valRawLvl">L0 (EMPTY)</span>
            </div>
            <div class="lvl-track">
              <span class="lvl-title">FILTERED (ANTI-SLOSH):</span>
              <div class="segmented-meter" id="meterStableLvl">
                <div class="seg" id="segStb0">L0</div>
                <div class="seg" id="segStb1">L1</div>
                <div class="seg" id="segStb2">L2</div>
                <div class="seg" id="segStb3">L3</div>
              </div>
              <span class="lvl-val" id="valStableLvl">L0 (EMPTY)</span>
            </div>
          </div>

          <div class="matrix-table">
            <div class="matrix-row">
              <span class="m-col-pin">P0 (Bit 0)</span>
              <span class="m-col-name">Float Switch Level 1</span>
              <span class="m-col-val" id="mFloat1"><span class="badge-tag idle">DRY (1)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">P1 (Bit 1)</span>
              <span class="m-col-name">Float Switch Level 2</span>
              <span class="m-col-val" id="mFloat2"><span class="badge-tag idle">DRY (1)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">P2 (Bit 2)</span>
              <span class="m-col-name">Float Switch Level 3</span>
              <span class="m-col-val" id="mFloat3"><span class="badge-tag idle">DRY (1)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">P3 (Bit 3)</span>
              <span class="m-col-name">Button: Mode Switch</span>
              <span class="m-col-val" id="mBtnMode"><span class="badge-tag idle">IDLE (1)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">P4 (Bit 4)</span>
              <span class="m-col-name">Button: Pump 1 Toggle</span>
              <span class="m-col-val" id="mBtnP1"><span class="badge-tag idle">IDLE (1)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">P5 (Bit 5)</span>
              <span class="m-col-name">Button: Pump 2 Toggle</span>
              <span class="m-col-val" id="mBtnP2"><span class="badge-tag idle">IDLE (1)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">P6 (Bit 6)</span>
              <span class="m-col-name">Button: Pump 3 Toggle</span>
              <span class="m-col-val" id="mBtnP3"><span class="badge-tag idle">IDLE (1)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">P7 (Bit 7)</span>
              <span class="m-col-name">Button: Pond Refill</span>
              <span class="m-col-val" id="mBtnPond"><span class="badge-tag idle">IDLE (1)</span></span>
            </div>
            <div class="matrix-row highlight">
              <span class="m-col-pin">FILTER</span>
              <span class="m-col-name">Debounced Level / Plausibility</span>
              <span class="m-col-val" id="mPlausibility"><span class="badge-tag ok">VALID (L:0)</span></span>
            </div>
          </div>
        </div>

        <!-- 2. Physical Actuators & Relays (GPIO & Safety Locks) -->
        <div class="matrix-subcard">
          <div class="subcard-title">
            <span class="subcard-icon">⚡</span>
            <h4>ACTUATORS & RELAY SAFETY LOCKS</h4>
            <span class="subcard-badge">ACTIVE-LOW OPTO</span>
          </div>

          <!-- Relay Visual Monitor Cards -->
          <div class="relay-locks-grid">
            <div class="relay-lock-item" id="lockCardP1">
              <div class="lock-hdr">
                <span class="lock-name">RELAY 1 (GPIO 47)</span>
                <span class="lock-state" id="mRelay1State">OFF</span>
              </div>
              <div class="lock-bar-track"><div class="lock-bar-fill" id="lockBarP1" style="width: 0%;"></div></div>
              <div class="lock-timer" id="mLockTimerP1">No active lock (Ready)</div>
            </div>

            <div class="relay-lock-item" id="lockCardP2">
              <div class="lock-hdr">
                <span class="lock-name">RELAY 2 (GPIO 41)</span>
                <span class="lock-state" id="mRelay2State">OFF</span>
              </div>
              <div class="lock-bar-track"><div class="lock-bar-fill" id="lockBarP2" style="width: 0%;"></div></div>
              <div class="lock-timer" id="mLockTimerP2">No active lock (Ready)</div>
            </div>

            <div class="relay-lock-item" id="lockCardP3">
              <div class="lock-hdr">
                <span class="lock-name">RELAY 3 (GPIO 39)</span>
                <span class="lock-state" id="mRelay3State">OFF</span>
              </div>
              <div class="lock-bar-track"><div class="lock-bar-fill" id="lockBarP3" style="width: 0%;"></div></div>
              <div class="lock-timer" id="mLockTimerP3">No active lock (Ready)</div>
            </div>
          </div>

          <div class="matrix-table">
            <div class="matrix-row">
              <span class="m-col-pin">GPIO 47</span>
              <span class="m-col-name">Outflow Pump 1 Pin Level</span>
              <span class="m-col-val" id="mRelay1"><span class="badge-tag idle">HIGH (DE-ENERGIZED)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">GPIO 41</span>
              <span class="m-col-name">Outflow Pump 2 Pin Level</span>
              <span class="m-col-val" id="mRelay2"><span class="badge-tag idle">HIGH (DE-ENERGIZED)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">GPIO 39</span>
              <span class="m-col-name">Outflow Pump 3 Pin Level</span>
              <span class="m-col-val" id="mRelay3"><span class="badge-tag idle">HIGH (DE-ENERGIZED)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">LoRa 0x03</span>
              <span class="m-col-name">Remote Pond Refill Demand</span>
              <span class="m-col-val" id="mPondDemand"><span class="badge-tag idle">IDLE</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">GPIO 48</span>
              <span class="m-col-name">WS2812B Diagnostic Strip</span>
              <span class="m-col-val" id="mNeoPixels"><span class="badge-tag ok">10x LEDs ACTIVE</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">LIFETIME</span>
              <span class="m-col-name">Pump Cumulative Runtime</span>
              <span class="m-col-val" id="mLifetime">P1:0s P2:0s P3:0s</span>
            </div>
          </div>
        </div>

        <!-- 3. Radio & Sensor Metrics -->
        <div class="matrix-subcard">
          <div class="subcard-title">
            <span class="subcard-icon">📡</span>
            <h4>RADIO & ENVIRONMENT</h4>
            <span class="subcard-badge">EU868 (868.1 MHz)</span>
          </div>

          <!-- Signal Strength & Metrics Strip -->
          <div class="radio-signal-strip">
            <div class="sig-card">
              <span class="sig-lbl">LORA RSSI</span>
              <div class="sig-val" id="mRssiVal">-- dBm</div>
              <div class="sig-bar-box"><div class="sig-bar-fill" id="mRssiBar" style="width: 0%;"></div></div>
            </div>
            <div class="sig-card">
              <span class="sig-lbl">LORA SNR</span>
              <div class="sig-val" id="mSnrVal">-- dB</div>
              <span class="badge-tag ok" id="mSnrRating">UNKNOWN</span>
            </div>
          </div>

          <div class="matrix-table">
            <div class="matrix-row">
              <span class="m-col-pin">SPI (FSPI)</span>
              <span class="m-col-name">SX1262 LoRa Transceiver</span>
              <span class="m-col-val" id="mRadioHealth"><span class="badge-tag ok">ONLINE</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">RF MOD</span>
              <span class="m-col-name">Modulation &amp; TX Power</span>
              <span class="m-col-val" id="mRfModulation">868.1 MHz &bull; SF9 BW125 (+22 dBm)</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">AIRTIME</span>
              <span class="m-col-name">LDRO Engine &amp; Packet ToA</span>
              <span class="m-col-val" id="mRfToaLdro"><span class="badge-tag idle">LDRO: OFF</span> &bull; ToA: -- ms</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">CAD / LBT</span>
              <span class="m-col-name">4-Symbol Hardware CAD</span>
              <span class="m-col-val" id="mRfCadLbt"><span class="badge-tag ok">4-SYMB CAD</span> &bull; Deferrals: 0</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">TELEM</span>
              <span class="m-col-name">Event-Driven Dual-Rate Engine</span>
              <span class="m-col-val" id="mTelemRateMode"><span class="badge-tag idle">SILENT IDLE (600s)</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">PACKETS</span>
              <span class="m-col-name">LoRa TX / RX Frame Counters</span>
              <span class="m-col-val" id="mRfPktStats">TX: 0 &bull; RX: 0 (Idle)</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">NODES</span>
              <span class="m-col-name">Tank: 0x02 | GW: 0x01 | Pond: 0x03</span>
              <span class="m-col-val"><span class="badge-tag ok">CONFIGURED</span></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">ENV</span>
              <span class="m-col-name">Ambient Temperature</span>
              <span class="m-col-val" id="mTemp">--.- °C</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">ENV</span>
              <span class="m-col-name">Relative Humidity</span>
              <span class="m-col-val" id="mHum">-- % RH</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">I²C BUS</span>
              <span class="m-col-name">SDA: GPIO 1 | SCL: GPIO 2</span>
              <span class="m-col-val" id="mI2cBus"><span class="badge-tag ok">100 kHz OK</span></span>
            </div>
          </div>
        </div>

        <!-- 4. System Health & NVS Parameters -->
        <div class="matrix-subcard">
          <div class="subcard-title">
            <span class="subcard-icon">🧠</span>
            <h4>MCU HEALTH & NVS CONFIG</h4>
            <span class="subcard-badge">ESP32-S3</span>
          </div>

          <!-- RAM Heap Meter -->
          <div class="heap-meter-box">
            <div class="heap-hdr">
              <span>SRAM USAGE</span>
              <span id="mHeapPercent">--% Free</span>
            </div>
            <div class="heap-track"><div class="heap-fill" id="mHeapFill" style="width: 0%;"></div></div>
          </div>

          <div class="matrix-table">
            <div class="matrix-row">
              <span class="m-col-pin">CPU / RAM</span>
              <span class="m-col-name">240 MHz Dual Core / Free Heap</span>
              <span class="m-col-val" id="mMcuHeap">-- KB (Min: -- KB)</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">BOOT / UP</span>
              <span class="m-col-name">Boot Counter / System Uptime</span>
              <span class="m-col-val" id="mMcuUptime">#0 | 0s</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">WI-FI</span>
              <span class="m-col-name">SoftAP IP & Clients</span>
              <span class="m-col-val" id="mSoftApInfo">192.168.4.1 (0 clients)</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">STATION</span>
              <span class="m-col-name">Home Wi-Fi (STA) & RSSI</span>
              <span class="m-col-val" id="mStaInfo">Disconnected</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">mDNS</span>
              <span class="m-col-name">Local Hostname</span>
              <span class="m-col-val"><a href="http://tanknode.local" target="_blank" style="color:var(--cyan);text-decoration:none;">http://tanknode.local</a></span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">NVS CFG</span>
              <span class="m-col-name">Runtime / Cooldown / Telem</span>
              <span class="m-col-val" id="mNvsTimers">20s / 20s / 30s</span>
            </div>
            <div class="matrix-row">
              <span class="m-col-pin">MASKS</span>
              <span class="m-col-name">Auto Pumps Allowed [P1, P2, P3]</span>
              <span class="m-col-val" id="mAutoMasks">[1, 1, 1]</span>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- Bottom Diagnostics Bar -->
    <footer class="hud-footer glass-card">
      <div class="diag-item">
        <span class="diag-k">PCF8574 I²C:</span>
        <span class="diag-v" id="diagPcf">OK</span>
      </div>
      <div class="diag-item">
        <span class="diag-k">LoRa RSSI:</span>
        <span class="diag-v" id="diagRssi">-- dBm</span>
      </div>
      <div class="diag-item">
        <span class="diag-k">LoRa SNR:</span>
        <span class="diag-v" id="diagSnr">-- dB</span>
      </div>
      <div class="diag-item">
        <span class="diag-k">Free Heap:</span>
        <span class="diag-v" id="diagHeap">-- KB</span>
      </div>
      <div class="diag-item">
        <span class="diag-k">Boot Count:</span>
        <span class="diag-v" id="diagBoots">#0</span>
      </div>
    </footer>
  </div>

  <!-- Settings & Network Modal Drawer -->
  <div class="modal-backdrop" id="settingsModal">
    <div class="modal-card glass-card" style="max-width: 680px;">
      <div class="modal-header">
        <h2>SYSTEM CONFIGURATION &amp; OTA</h2>
        <button class="icon-btn" id="btnSettingsClose">✕</button>
      </div>

      <!-- 5-Tab Navigation -->
      <div class="settings-tabs-nav">
        <button class="settings-tab-btn active" data-tab="tabHydr">💧 Hydraulics</button>
        <button class="settings-tab-btn" data-tab="tabLora">📡 LoRa RF</button>
        <button class="settings-tab-btn" data-tab="tabWifi">📶 Wi-Fi &amp; Net</button>
        <button class="settings-tab-btn" data-tab="tabOta">⚡ Wireless OTA</button>
        <button class="settings-tab-btn" data-tab="tabLogs">📜 Live Logs</button>
      </div>

      <div class="modal-body">
        <!-- Tab 1: Hydraulics & Timings -->
        <div id="tabHydr" class="settings-tab-pane active">
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgCapacity">Reservoir Capacity (L):</label>
              <input type="number" id="cfgCapacity" min="100" max="1000000" step="100" value="10000">
            </div>
            <div class="form-row">
              <label for="cfgBootAuto">Default Boot Mode:</label>
              <select id="cfgBootAuto">
                <option value="0">Manual Mode</option>
                <option value="1">Automatic Mode</option>
              </select>
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgTrigger">Refill Trigger (Low L0-L2):</label>
              <select id="cfgTrigger">
                <option value="0">Level 0 (Empty 0%)</option>
                <option value="1" selected>Level 1 (Low 33%)</option>
                <option value="2">Level 2 (Mid 66%)</option>
              </select>
            </div>
            <div class="form-row">
              <label for="cfgCutoff">Refill Cutoff (Full L1-L3):</label>
              <select id="cfgCutoff">
                <option value="1">Level 1 (Low 33%)</option>
                <option value="2">Level 2 (Mid 66%)</option>
                <option value="3" selected>Level 3 (Full 100%)</option>
              </select>
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgTelemIdle">Silent Idle Keepalive (ms):</label>
              <input type="number" id="cfgTelemIdle" min="5000" max="3600000" step="5000" value="600000" title="Default 600,000 ms (10 min). Range: 5s to 60 min">
            </div>
            <div class="form-row">
              <label for="cfgTelemBurst">Active Burst Interval (ms):</label>
              <input type="number" id="cfgTelemBurst" min="1000" max="60000" step="500" value="3000" title="Default 3,000 ms (3.0s). Range: 1.0s to 60.0s">
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgRuntime">Pump Min Runtime (ms):</label>
              <input type="number" id="cfgRuntime" min="1000" max="300000" step="1000" value="20000">
            </div>
            <div class="form-row">
              <label for="cfgCooldown">Pump Min Cooldown (ms):</label>
              <input type="number" id="cfgCooldown" min="1000" max="600000" step="1000" value="20000">
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgStagger">Motor Anti-Inrush Stagger (ms):</label>
              <input type="number" id="cfgStagger" min="0" max="60000" step="500" value="3000">
            </div>
            <div class="form-row">
              <label>Auto Pump Masks (P1-P3):</label>
              <div style="display:flex; gap:12px; align-items:center; margin-top:8px;">
                <label class="auto-mask-check"><input type="checkbox" id="cfgMaskP1" checked><span>P1</span></label>
                <label class="auto-mask-check"><input type="checkbox" id="cfgMaskP2" checked><span>P2</span></label>
                <label class="auto-mask-check"><input type="checkbox" id="cfgMaskP3" checked><span>P3</span></label>
              </div>
            </div>
          </div>
          <div class="modal-actions" style="margin-top:14px;">
            <button class="btn btn-primary" onclick="saveHydraulicsConfig()">SAVE HYDRAULICS &amp; TIMINGS</button>
          </div>
        </div>

        <!-- Tab 2: LoRa RF Tuning -->
        <div id="tabLora" class="settings-tab-pane">
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgLoraFreq">Carrier Frequency (Hz):</label>
              <input type="number" id="cfgLoraFreq" min="150000000" max="960000000" step="100000" value="868100000">
            </div>
            <div class="form-row">
              <label for="cfgLoraPwr">TX Power (dBm, -9..+22):</label>
              <input type="number" id="cfgLoraPwr" min="-9" max="22" step="1" value="22">
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgLoraSf">Spreading Factor (SF6-SF12):</label>
              <select id="cfgLoraSf" onchange="previewLoraAirtime()">
                <option value="6">SF6 (Ultra-Fast / Short Range)</option>
                <option value="7">SF7 (Fast / High Data Rate)</option>
                <option value="8">SF8</option>
                <option value="9" selected>SF9 (Balanced Default)</option>
                <option value="10">SF10 (Long Range 700m+)</option>
                <option value="11">SF11 (High Penetration + Auto LDRO)</option>
                <option value="12">SF12 (Maximum Penetration + Auto LDRO)</option>
              </select>
            </div>
            <div class="form-row">
              <label for="cfgLoraBw">Bandwidth (kHz):</label>
              <select id="cfgLoraBw" onchange="previewLoraAirtime()">
                <option value="125" selected>125 kHz (Standard EU868)</option>
                <option value="250">250 kHz</option>
                <option value="500">500 kHz</option>
              </select>
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgLoraCr">Coding Rate (CR):</label>
              <select id="cfgLoraCr" onchange="previewLoraAirtime()">
                <option value="5" selected>4/5 (Standard FEC)</option>
                <option value="6">4/6</option>
                <option value="7">4/7</option>
                <option value="8">4/8 (Maximum FEC Protection)</option>
              </select>
            </div>
            <div class="form-row">
              <label for="cfgLoraSync">Sync Word (Hex):</label>
              <input type="text" id="cfgLoraSync" value="0x1424">
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgLoraLdro">Low Data Rate Optimize (LDRO):</label>
              <select id="cfgLoraLdro" onchange="previewLoraAirtime()">
                <option value="0" selected>Auto (Active when Tsym &ge; 16.0 ms)</option>
                <option value="1">Force Enabled (Always ON)</option>
                <option value="2">Force Disabled (Always OFF)</option>
              </select>
            </div>
            <div class="form-row">
              <label>Active Demodulator LDRO &amp; ToA:</label>
              <div style="display:flex; gap:6px; flex-wrap:wrap; margin-top:6px;">
                <span id="ldroLiveBadge" class="badge-tag idle" style="font-size:11px;">LDRO: INACTIVE</span>
                <span id="toaLiveBadge" class="badge-tag active" style="font-size:11px;">ToA: -- ms</span>
              </div>
            </div>
          </div>
          <div class="form-grid-2">
            <div class="form-row">
              <label for="cfgLbtRetries">LBT Hardware CAD Retries (0-10):</label>
              <input type="number" id="cfgLbtRetries" min="0" max="10" step="1" value="4">
            </div>
            <div class="form-row">
              <label for="cfgLbtBackoff">LBT Base Jitter Backoff (ms):</label>
              <input type="number" id="cfgLbtBackoff" min="10" max="2000" step="10" value="60">
            </div>
          </div>
          <div style="margin-top:6px; padding:8px 12px; border-radius:8px; background:rgba(14,165,233,0.08); border:1px solid rgba(14,165,233,0.2); font-size:0.72rem; font-family:var(--font-mono); color:var(--cyan);" id="cadLiveSummary">
            🛡️ LBT 4-Symbol Hardware CAD (SX126X_CAD_ON_4_SYMB) Active &bull; Guard: -- ms &bull; Deferrals: 0
          </div>
          <div class="modal-actions" style="margin-top:14px;">
            <button class="btn btn-primary" onclick="saveLoraConfig()">APPLY &amp; SAVE LORA RF</button>
          </div>
        </div>

        <!-- Tab 3: Wi-Fi & Network -->
        <div id="tabWifi" class="settings-tab-pane">
          <div class="form-row checkbox-row">
            <label for="cfgAlwaysOn">Auto-Start Web on Boot:</label>
            <input type="checkbox" id="cfgAlwaysOn" checked>
          </div>
          <p class="section-help">When enabled, the Web Server and SoftAP automatically initialize on boot.</p>
          <div class="form-row">
            <label for="cfgStaSsid">Home Wi-Fi (SSID):</label>
            <input type="text" id="cfgStaSsid" placeholder="Link2">
          </div>
          <div class="form-row">
            <label for="cfgStaPass">Home Wi-Fi Password:</label>
            <input type="password" id="cfgStaPass" placeholder="Horvat2017">
          </div>
          <p class="section-help">Access dashboard via <code>http://tanknode.local</code> on your home router.</p>
          <div class="modal-actions" style="margin-top:14px;">
            <button class="btn btn-primary" onclick="saveWifiConfig()">SAVE WI-FI CONFIG</button>
            <button class="btn btn-secondary" onclick="restoreDefaults()">RESTORE DEFAULTS</button>
            <button class="btn btn-danger" onclick="rebootDevice()">REBOOT ESP32</button>
          </div>
        </div>

        <!-- Tab 4: Wireless OTA Flashing -->
        <div id="tabOta" class="settings-tab-pane">
          <p class="section-help">Flash new compiled firmware wirelessly over Wi-Fi without any USB cable connection.</p>
          <div style="background:rgba(0,0,0,0.3); border:1px dashed var(--border); border-radius:10px; padding:16px; text-align:center; margin-bottom:12px;">
            <div style="font-size:1.8rem; margin-bottom:6px;">🚀</div>
            <h4 style="color:#fff; margin-bottom:4px; font-family:var(--font-mono);">Browser Direct Upload</h4>
            <p style="font-size:0.75rem; color:var(--muted); margin-bottom:12px;">Select compiled <code>firmware.bin</code></p>
            <input type="file" id="otaFileInput" accept=".bin" style="margin-bottom:10px;">
            <div class="prog-bar" id="otaProgressBar" style="display:none;"><div class="prog-fill" id="otaProgressFill"></div></div>
            <div id="otaUploadStatus" style="font-family:var(--font-mono); font-size:0.72rem; color:var(--cyan); margin-bottom:8px;"></div>
            <button class="btn btn-primary" style="width:100%;" onclick="uploadOtaFirmware()">FLASH FIRMWARE NOW</button>
          </div>
          <div style="padding:10px; border-radius:8px; background:rgba(0,240,255,0.06); border:1px solid rgba(0,240,255,0.2); font-family:var(--font-mono); font-size:0.72rem;">
            <strong>ArduinoOTA Command-Line Flashing:</strong><br>
            Run: <code>pio run -e esp32s3_Bazin_ota -t upload</code><br>
            Port: <code>3232</code> &bull; Host: <span id="otaHostSta">tanknode.local</span>
          </div>
        </div>

        <!-- Tab 5: Live Event Logs -->
        <div id="tabLogs" class="settings-tab-pane">
          <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
            <div style="display:flex; gap:6px; flex-wrap:wrap;">
              <button class="btn" style="padding:2px 8px;font-size:0.65rem;" onclick="filterLogs('ALL')">ALL</button>
              <button class="btn" style="padding:2px 8px;font-size:0.65rem;" onclick="filterLogs('LORA')">LORA</button>
              <button class="btn" style="padding:2px 8px;font-size:0.65rem;" onclick="filterLogs('PUMP')">PUMP</button>
              <button class="btn" style="padding:2px 8px;font-size:0.65rem;" onclick="filterLogs('HYDR')">HYDR</button>
              <button class="btn" style="padding:2px 8px;font-size:0.65rem;" onclick="filterLogs('WIFI')">WIFI</button>
              <button class="btn" style="padding:2px 8px;font-size:0.65rem;" onclick="filterLogs('OTA')">OTA</button>
            </div>
            <button class="btn btn-danger" style="padding:2px 8px;font-size:0.65rem;" onclick="clearLogs()">Clear</button>
          </div>
          <div id="logConsoleBox" class="log-box"></div>
        </div>
      </div>
    </div>
  </div>

  <!-- Toast Notification Container -->
  <div class="toast-container" id="toastContainer"></div>

  <script>
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


</script>
</body>
</html>
)RAW_HTML";
