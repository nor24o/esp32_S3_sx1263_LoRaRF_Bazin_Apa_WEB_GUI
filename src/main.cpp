/*
 * Tank Node Firmware — ESP32-S3 + SX1262
 * Reads 3 float-switch water levels, controls 3 parallel outflow irrigation pumps,
 * automates a remote pond charging pump via LoRa commands.
 */

#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <SPI.h>
#include <SX126x.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <Update.h>

#include "config.h"
#include "web_assets.h"

// ─── Protocol structs ─────────────────────────────────────────────────────────
enum MsgType : uint8_t { MSG_STATUS = 1, MSG_CMD = 2 };

struct __attribute__((packed)) LoRaHeader {
  uint16_t magic;
  uint8_t target_id;
  uint8_t sender_id;
  uint8_t msg_type;
};

struct __attribute__((packed)) StatusPayload {
  uint8_t flags;       
  uint8_t water_level; 
  int16_t temperature; 
  uint8_t humidity;    
  uint8_t reserved;
  uint8_t run_left[3]; // Remaining minimum runtime in seconds for P1, P2, P3
  uint8_t cd_left[3];  // Remaining cooldown in seconds for P1, P2, P3
};

struct __attribute__((packed)) CmdPayload {
  uint8_t target; 
  uint8_t action; 
  uint8_t value;  
};

struct __attribute__((packed)) LoRaPacket {
  LoRaHeader header;
  union {
    StatusPayload status;
    CmdPayload cmd;
  } payload;
};

// ─── Local config (NVS-persisted) ─────────────────────────────────────────────
struct LocalConfig {
  uint32_t pump_min_runtime_ms;
  uint32_t pump_min_cooldown_ms;
  uint32_t telemetry_interval_ms;
  uint8_t boot_auto_mode;
  bool pump_enabled_auto[3]; 
  bool pond_enabled;         
  bool pump_manual_state[3]; 
  bool last_demand_state;    // Stored filling state latch for the pond charger
  uint8_t debug_mode_enabled; // 1 = start WiFi hotspot on boot, 0 = keep off
  uint8_t web_always_on;      // 1 = Web server always active on boot
  char wifi_sta_ssid[33];     // optional home Wi-Fi SSID
  char wifi_sta_pass[65];     // optional home Wi-Fi password
  // LoRa RF Transceiver Tuning
  uint32_t lora_freq_hz;
  int8_t lora_tx_power_dbm;
  uint8_t lora_sf;
  uint16_t lora_bw_khz;
  uint8_t lora_cr;
  uint8_t lora_ldro;          // 0 = Auto (Symbol >= 16.38ms), 1 = Force ON, 2 = Force OFF
  uint16_t lora_sync_word;
  // Reservoir Hydraulic Capacity & Refill Thresholds
  uint32_t tank_capacity_liters;
  uint8_t refill_trigger_level;
  uint8_t refill_cutoff_level;
  // Protocol network identities
  uint8_t node_id;
  uint8_t pond_id;
  uint8_t gateway_id;
  uint16_t network_magic;
};

struct NodeStats {
  uint32_t runtime_p1_s;
  uint32_t runtime_p2_s;
  uint32_t runtime_p3_s;
  uint32_t boot_count;
};

struct PumpHysteresis {
  bool state = false;
  uint32_t lastOnTime = 0;
  uint32_t lastOffTime = 0;
  bool initialized = false;
};

// ─── Shared system state (protected by xStateMutex) ───────────────────────────
struct SystemState {
  uint8_t waterLevel = 0;
  uint8_t rawWaterLevel = 0;
  uint8_t pcfRaw = 0xFF;
  bool autoMode = false;
  bool pumpState[3] = {};
  bool pumpEnabledAuto[3] = {false, false, false}; 
  bool pond = false;
  uint8_t errorCode = 0;
  int16_t temperature = 0;
  uint8_t humidity = 0;
  int16_t lastRssi = 0;
  int8_t lastSnr = 0;
  bool radioOk = false;
  bool pcfOk = false;
  bool btnMode_edge = false;
  bool btnP1_edge = false;
  bool btnP2_edge = false;
  bool btnP3_edge = false;
  bool btnPond_edge = false; 
  bool rxFlash = false;
  bool txFlash = false;
  bool waterLevelValid = false; 
  bool demandActive = false;          // Mirrors tank charging cycle state
  bool triggerImmediateTelem = false; // Flag to send status telemetry immediately
  bool psramOk = false;
  uint32_t psramTotalBytes = 0;
  uint32_t psramFreeBytes = 0;
  bool loraTimeoutAlarm = false;
  bool tankEmptyAlarm = false;
  uint32_t pumpLastOn[3] = {};
  uint32_t pumpLastOff[3] = {};
  uint8_t pumpOverride[3] = {PUMP_OVERRIDE_AUTO, PUMP_OVERRIDE_AUTO, PUMP_OVERRIDE_AUTO};
  uint32_t pumpTimedUntil[3] = {};
  uint8_t activeRule = 3;
};

struct TelemetryHistoryEntry {
  uint32_t timestamp_s;
  uint8_t waterLevel;
  uint8_t flags;
  int16_t temperature;
  uint8_t humidity;
};

static TelemetryHistoryEntry *gHistory = nullptr;
static uint16_t gHistoryCount = 0;
static uint16_t gHistoryHead = 0;

static LocalConfig gConfig;
static LocalConfig gConfigSaved = {};
static bool gConfigSavedValid = false;
static NodeStats gStats;
static SystemState gState;

static SX126x LoRa;
static SPIClass SPI_2(FSPI);
static Preferences prefs;
static CRGB leds[NUM_LEDS];

static SemaphoreHandle_t xStateMutex = nullptr;
static SemaphoreHandle_t xLogMutex = nullptr;
static QueueHandle_t xRxQueue = nullptr;
static QueueHandle_t xTxQueue = nullptr;
static TaskHandle_t xControlTask = nullptr;

static PumpHysteresis phP1, phP2, phP3;

#if ENABLE_DEBUG_WEB
static WebServer httpServer(DEBUG_HTTP_PORT);
static WebSocketsServer wsServer(DEBUG_WS_PORT);
static DNSServer dnsServer;
static bool gDebugServerRunning = false;
static bool gDebugServerWanted = false;
#endif

// ─── Circular System Event Log Engine ─────────────────────────────────────────
struct SystemLogEntry {
  uint32_t timestamp_s;
  char category[12];
  char message[110];
  uint8_t level; // 0=INFO, 1=WARN, 2=ERR, 3=SUCCESS
};

#define MAX_SYSTEM_LOGS 64
static SystemLogEntry gSystemLogs[MAX_SYSTEM_LOGS];
static uint16_t gLogCount = 0;
static uint16_t gLogHead = 0;

static void logEvent(const char *category, uint8_t level, const char *fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  const char *lvlStr = (level == 1) ? "WARN" : ((level == 2) ? "ERR" : ((level == 3) ? "OK" : "INFO"));
  Serial.printf("[%lu][%s][%s] %s\n", (unsigned long)(millis() / 1000), category, lvlStr, buf);

  if (xLogMutex && xSemaphoreTake(xLogMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    uint16_t idx;
    if (gLogCount < MAX_SYSTEM_LOGS) {
      idx = (gLogHead + gLogCount) % MAX_SYSTEM_LOGS;
      gLogCount++;
    } else {
      idx = gLogHead;
      gLogHead = (gLogHead + 1) % MAX_SYSTEM_LOGS;
    }
    gSystemLogs[idx].timestamp_s = (uint32_t)(millis() / 1000);
    strncpy(gSystemLogs[idx].category, category, sizeof(gSystemLogs[idx].category) - 1);
    gSystemLogs[idx].category[sizeof(gSystemLogs[idx].category) - 1] = '\0';
    strncpy(gSystemLogs[idx].message, buf, sizeof(gSystemLogs[idx].message) - 1);
    gSystemLogs[idx].message[sizeof(gSystemLogs[idx].message) - 1] = '\0';
    gSystemLogs[idx].level = level;
    xSemaphoreGive(xLogMutex);
  }

#if ENABLE_DEBUG_WEB
  if (gDebugServerRunning) {
    char wsBuf[256];
    snprintf(wsBuf, sizeof(wsBuf),
      "{\"type\":\"log_event\",\"ts\":%lu,\"cat\":\"%s\",\"lvl\":%u,\"msg\":\"%s\"}",
      (unsigned long)(millis() / 1000), category, level, buf);
    wsServer.broadcastTXT(wsBuf);
  }
#endif
}

// ─── Config / Stats persistence ───────────────────────────────────────────────
static void resetConfigToDefaults() {
  memset(&gConfig, 0, sizeof(gConfig));
  gConfig.pump_min_runtime_ms = DEFAULT_PUMP_MIN_RUNTIME_MS;
  gConfig.pump_min_cooldown_ms = DEFAULT_PUMP_MIN_COOLDOWN_MS;
  gConfig.telemetry_interval_ms = DEFAULT_TELEMETRY_INTERVAL_MS;
  gConfig.boot_auto_mode = 0;
  gConfig.pump_enabled_auto[0] = true;
  gConfig.pump_enabled_auto[1] = true;
  gConfig.pump_enabled_auto[2] = true;
  gConfig.debug_mode_enabled = DEFAULT_WEB_ALWAYS_ON;
  gConfig.web_always_on = DEFAULT_WEB_ALWAYS_ON;
  strncpy(gConfig.wifi_sta_ssid, DEFAULT_WIFI_STA_SSID, sizeof(gConfig.wifi_sta_ssid) - 1);
  strncpy(gConfig.wifi_sta_pass, DEFAULT_WIFI_STA_PASS, sizeof(gConfig.wifi_sta_pass) - 1);

  gConfig.lora_freq_hz = DEFAULT_LORA_FREQ_HZ;
  gConfig.lora_tx_power_dbm = DEFAULT_LORA_TX_POWER_DBM;
  gConfig.lora_sf = DEFAULT_LORA_SF;
  gConfig.lora_bw_khz = DEFAULT_LORA_BW_KHZ;
  gConfig.lora_cr = DEFAULT_LORA_CR;
  gConfig.lora_ldro = DEFAULT_LORA_LDRO;
  gConfig.lora_sync_word = DEFAULT_LORA_SYNC_WORD;
 
  gConfig.tank_capacity_liters = DEFAULT_TANK_CAPACITY_LITERS;
  gConfig.refill_trigger_level = DEFAULT_REFILL_TRIGGER_LEVEL;
  gConfig.refill_cutoff_level = DEFAULT_REFILL_CUTOFF_LEVEL;
 
  gConfig.node_id = DEFAULT_NODE_ID;
  gConfig.pond_id = DEFAULT_POND_ID;
  gConfig.gateway_id = DEFAULT_GATEWAY_ID;
  gConfig.network_magic = DEFAULT_NETWORK_MAGIC;
}

static bool validateConfig(const LocalConfig &c) {
  return c.pump_min_runtime_ms >= CFG_RUNTIME_MIN_MS &&
         c.pump_min_runtime_ms <= CFG_RUNTIME_MAX_MS &&
         c.pump_min_cooldown_ms >= CFG_COOLDOWN_MIN_MS &&
         c.pump_min_cooldown_ms <= CFG_COOLDOWN_MAX_MS &&
         c.telemetry_interval_ms >= CFG_TELEM_MIN_MS &&
         c.telemetry_interval_ms <= CFG_TELEM_MAX_MS &&
         c.boot_auto_mode <= 1 &&
         c.debug_mode_enabled <= 1 &&
         c.web_always_on <= 1 &&
         c.lora_freq_hz >= 150000000UL && c.lora_freq_hz <= 960000000UL &&
         c.lora_tx_power_dbm >= -9 && c.lora_tx_power_dbm <= 22 &&
         c.lora_sf >= 6 && c.lora_sf <= 12 &&
         (c.lora_bw_khz == 125 || c.lora_bw_khz == 250 || c.lora_bw_khz == 500) &&
         c.lora_cr >= 5 && c.lora_cr <= 8 &&
         c.lora_ldro <= 2 &&
         c.refill_trigger_level <= 3 &&
         c.refill_cutoff_level <= 3 &&
         c.refill_trigger_level < c.refill_cutoff_level;
}

static void saveConfig(bool force = false) {
  if (!force && gConfigSavedValid && memcmp(&gConfig, &gConfigSaved, sizeof(LocalConfig)) == 0) {
    return; // Skip redundant flash writes
  }
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar(NVS_KEY_CFGVER, CONFIG_VERSION);
  prefs.putBytes(NVS_KEY_CFG, &gConfig, sizeof(gConfig));
  prefs.end();
  gConfigSaved = gConfig;
  gConfigSavedValid = true;
}

static void loadConfig() {
  prefs.begin(NVS_NAMESPACE, true);
  uint8_t ver = prefs.getUChar(NVS_KEY_CFGVER, 0);
  size_t sz = prefs.getBytesLength(NVS_KEY_CFG);
  bool ok = (ver == CONFIG_VERSION && sz == sizeof(gConfig));
  if (ok) prefs.getBytes(NVS_KEY_CFG, &gConfig, sizeof(gConfig));
  prefs.end();
  if (!ok || !validateConfig(gConfig)) {
    Serial.println("[Config] Reset to defaults");
    resetConfigToDefaults();
    saveConfig(true);
  } else {
    gConfigSaved = gConfig;
    gConfigSavedValid = true;
  }
}

static void saveStats() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUInt(NVS_KEY_RT1, gStats.runtime_p1_s);
  prefs.putUInt(NVS_KEY_RT2, gStats.runtime_p2_s);
  prefs.putUInt(NVS_KEY_RT3, gStats.runtime_p3_s);
  prefs.putUInt(NVS_KEY_BC, gStats.boot_count);
  prefs.end();
}

static void loadStats() {
  prefs.begin(NVS_NAMESPACE, true);
  gStats.runtime_p1_s = prefs.getUInt(NVS_KEY_RT1, 0);
  gStats.runtime_p2_s = prefs.getUInt(NVS_KEY_RT2, 0);
  gStats.runtime_p3_s = prefs.getUInt(NVS_KEY_RT3, 0);
  gStats.boot_count   = prefs.getUInt(NVS_KEY_BC,  0);
  prefs.end();
}

// ─── Direct Output Executer (Relay Chatter Guarded) ───────────────────────────
static bool canChangePumpState(const PumpHysteresis &ph, bool wantOn, bool emergencyDryStop,
                               uint32_t minRunMs, uint32_t minCooldownMs) {
  if (!ph.initialized) return true;
  if (ph.state == wantOn) return false;

  // Emergency dry stop bypasses minimum runtime to protect pumps immediately!
  if (!wantOn && emergencyDryStop) {
    return true;
  }

  uint32_t now = millis();
  if (!wantOn && (now - ph.lastOnTime < minRunMs)) {
    return false; // Still within mandatory runtime
  }
  if (wantOn && (now - ph.lastOffTime < minCooldownMs)) {
    return false; // Still within mandatory cooldown
  }
  return true;
}

static void setPumpState(PumpHysteresis &ph, bool on, int pin) {
  if (ph.initialized && ph.state == on) {
    return; 
  }
  if (on && !ph.state) ph.lastOnTime = millis();
  if (!on && ph.state) ph.lastOffTime = millis();
  ph.state = on;
  ph.initialized = true;
  digitalWrite(pin, on ? LOW : HIGH);
}

// ─── LoRa helpers ─────────────────────────────────────────────────────────────
static void setForRx() {
  sx126x_setBufferBaseAddress(0, 0);
  sx126x_clearIrqStatus(0x03FF);
  LoRa.setLoRaPacket(SX126X_HEADER_EXPLICIT, LORA_PREAMBLE_LEN, LORA_MAX_PAYLOAD, true, true);
  LoRa.setRxGain(SX126X_RX_GAIN_BOOSTED);
  LoRa.request(SX126X_RX_CONTINUOUS);
}

static bool isLdroActive() {
  if (gConfig.lora_ldro == 1) return true;
  if (gConfig.lora_ldro == 2) return false;
  // Auto mode: true whenever symbol duration >= 16.0 ms (e.g. SF11/12 at 125kHz, SF12 at 250kHz)
  uint32_t bw = gConfig.lora_bw_khz ? gConfig.lora_bw_khz : 125;
  float symLenMs = (float)(1UL << gConfig.lora_sf) / (float)bw;
  return (symLenMs >= 16.0f);
}

static bool applyLoRaConfig() {
  if (!gState.radioOk) return false;
  uint32_t bwHz = (uint32_t)gConfig.lora_bw_khz * 1000UL;
  bool ldro = isLdroActive();
  LoRa.setFrequency(gConfig.lora_freq_hz);
  LoRa.setTxPower(gConfig.lora_tx_power_dbm, SX126X_TX_POWER_SX1262);
  LoRa.setLoRaModulation(gConfig.lora_sf, bwHz, gConfig.lora_cr, ldro);
  LoRa.setSyncWord(gConfig.lora_sync_word);
  setForRx();
  logEvent("LORA", 3, "Applied RF: %lu Hz, SF%u, BW%u kHz, CR4/%u, LDRO=%s, %d dBm, Sync 0x%04X",
           (unsigned long)gConfig.lora_freq_hz, gConfig.lora_sf, gConfig.lora_bw_khz,
           gConfig.lora_cr, ldro ? "ON" : "OFF", gConfig.lora_tx_power_dbm, gConfig.lora_sync_word);
  return true;
}

static void buildStatus(LoRaPacket &pkt) {
  pkt.header = {gConfig.network_magic, gConfig.gateway_id, gConfig.node_id, MSG_STATUS};
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    uint8_t allowed_mask = (gState.pumpEnabledAuto[0] ? 1 : 0) |
                           (gState.pumpEnabledAuto[1] ? 2 : 0) |
                           (gState.pumpEnabledAuto[2] ? 4 : 0);
    uint32_t now = millis();
    uint8_t rLeft[3] = {0, 0, 0};
    uint8_t cLeft[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
      if (gState.pumpState[i] && gState.pumpLastOn[i] > 0) {
        uint32_t el = now - gState.pumpLastOn[i];
        if (el < gConfig.pump_min_runtime_ms) {
          rLeft[i] = (uint8_t)min(255UL, (gConfig.pump_min_runtime_ms - el) / 1000UL);
        }
      } else if (!gState.pumpState[i] && gState.pumpLastOff[i] > 0) {
        uint32_t el = now - gState.pumpLastOff[i];
        if (el < gConfig.pump_min_cooldown_ms) {
          cLeft[i] = (uint8_t)min(255UL, (gConfig.pump_min_cooldown_ms - el) / 1000UL);
        }
      }
    }
    pkt.payload.status = {
      (uint8_t)((gState.autoMode ? STATUS_BIT_AUTO : 0) |
                (gState.pumpState[0] ? STATUS_BIT_P1 : 0) |
                (gState.pumpState[1] ? STATUS_BIT_P2 : 0) |
                (gState.pumpState[2] ? STATUS_BIT_P3 : 0) |
                ((gConfig.pond_enabled || gState.pond) ? STATUS_BIT_POND : 0)), 
      gState.waterLevel,
      gState.temperature,
      gState.humidity,
      allowed_mask,
      {rLeft[0], rLeft[1], rLeft[2]},
      {cLeft[0], cLeft[1], cLeft[2]}
    };
    xSemaphoreGive(xStateMutex);
  }
}

// ─── LoRa Manager Task (Core 0, Pri 2) ────────────────────────────────────────
static void Task_LoRaManager(void *) {
  uint32_t lastTxEndMs = 0;
  for (;;) {
    uint32_t now = millis();
    // Guard time after transmission before next queue transmission (ensures RX continuous listening window)
    uint32_t guardMs = (gConfig.lora_sf >= 12) ? 1800UL :
                       (gConfig.lora_sf == 11) ? 1200UL :
                       (gConfig.lora_sf == 10) ? 800UL : 400UL;

    LoRaPacket txPkt;
    if ((now - lastTxEndMs >= guardMs) && (xQueueReceive(xTxQueue, &txPkt, 0) == pdTRUE)) {
      uint8_t payLen = (txPkt.header.msg_type == MSG_STATUS) ? (uint8_t)sizeof(StatusPayload)
                                                             : (uint8_t)sizeof(CmdPayload);
      sx126x_setBufferBaseAddress(0, 0);
      LoRa.setLoRaPacket(SX126X_HEADER_EXPLICIT, LORA_PREAMBLE_LEN, payLen, true, false);
      LoRa.beginPacket();
      LoRa.write((uint8_t *)&txPkt, sizeof(LoRaHeader) + payLen);
      LoRa.endPacket();
      LoRa.wait();
      lastTxEndMs = millis();
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        gState.txFlash = true;
        xSemaphoreGive(xStateMutex);
      }
      Serial.printf("\n[LoRa TX] type=%u target=0x%02X (Guard=%lums)\n", txPkt.header.msg_type, txPkt.header.target_id, guardMs);
      setForRx();
      LoRa.purge(LoRa.available());
    }

    uint8_t avail = LoRa.available();
    if (avail >= (uint8_t)sizeof(LoRaHeader)) {
      LoRaPacket rxPkt = {};
      uint8_t *p = (uint8_t *)&rxPkt;
      uint8_t n = 0;
      while (LoRa.available() && n < (uint8_t)sizeof(rxPkt)) p[n++] = LoRa.read();
      while (LoRa.available()) LoRa.read(); 
      
      printf("\n[LoRa RX] type=%u len=%u rssi=%d snr=%d\n", rxPkt.header.msg_type, n, LoRa.packetRssi(), LoRa.snr());
      printf("Packet: ");
      for (int i = 0; i < n; i++) printf("%02X ", p[i]);
      printf("\n");
      
      if (rxPkt.header.magic == gConfig.network_magic &&
          (rxPkt.header.target_id == gConfig.node_id || rxPkt.header.target_id == NODE_BROADCAST)) {
        printf("[LoRa RX] Valid packet for this node (target=%u)\n", rxPkt.header.target_id);

        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          gState.lastRssi = (int16_t)LoRa.packetRssi();
          gState.lastSnr = (int8_t)LoRa.snr();
          gState.rxFlash = true;
          xSemaphoreGive(xStateMutex);
        }
        xQueueSend(xRxQueue, &rxPkt, 0);
        if (xControlTask) xTaskNotifyGive(xControlTask);
      }
    }

    uint8_t st = LoRa.status();
    if (st == SX126X_STATUS_CRC_ERR || st == SX126X_STATUS_HEADER_ERR) setForRx();

    vTaskDelay(pdMS_TO_TICKS(LORA_POLL_MS));
  }
}

// ─── Debug Web / WebSocket Server (Core 0, Pri 1) ─────────────────────────────
#if ENABLE_DEBUG_WEB

static void broadcastTelemetryWs() {
  if (!gDebugServerRunning) return;
  char buf[1280];
  uint8_t wl = 0, raw_wl = 0, pcf_raw = 0xFF, ec = 0;
  bool am = false, pond = false, demand = false, pcfOk = false, wlValid = false, radioOk = false;
  bool ps[3] = {}, pea[3] = {};
  uint32_t lastOn[3] = {}, lastOff[3] = {};
  uint8_t pOverride[3] = {};
  uint32_t pTimedUntil[3] = {};
  uint8_t activeRule = 3;
  int16_t rssi = 0; int8_t snr = 0;
  int16_t temp = 0; uint8_t hum = 0;
  
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    wl = gState.waterLevel;
    raw_wl = gState.rawWaterLevel;
    pcf_raw = gState.pcfRaw;
    wlValid = gState.waterLevelValid;
    am = gState.autoMode;
    pond = gState.pond;
    demand = gState.demandActive;
    ec = gState.errorCode;
    pcfOk = gState.pcfOk;
    radioOk = gState.radioOk;
    temp = gState.temperature;
    hum = gState.humidity;
    for (int i = 0; i < 3; i++) {
      ps[i] = gState.pumpState[i];
      pea[i] = gState.pumpEnabledAuto[i];
      lastOn[i] = gState.pumpLastOn[i];
      lastOff[i] = gState.pumpLastOff[i];
      pOverride[i] = gState.pumpOverride[i];
      pTimedUntil[i] = gState.pumpTimedUntil[i];
    }
    activeRule = gState.activeRule;
    rssi = gState.lastRssi;
    snr = gState.lastSnr;
    xSemaphoreGive(xStateMutex);
  }

  uint32_t now = millis();
  uint32_t runLeft[3] = {}, cdLeft[3] = {}, timedLeft[3] = {};
  for (int i = 0; i < 3; i++) {
    if (ps[i] && lastOn[i] > 0) {
      uint32_t el = now - lastOn[i];
      if (el < gConfig.pump_min_runtime_ms) runLeft[i] = (gConfig.pump_min_runtime_ms - el) / 1000;
    } else if (!ps[i] && lastOff[i] > 0) {
      uint32_t el = now - lastOff[i];
      if (el < gConfig.pump_min_cooldown_ms) cdLeft[i] = (gConfig.pump_min_cooldown_ms - el) / 1000;
    }
    if (pOverride[i] == PUMP_OVERRIDE_FORCE_ON && pTimedUntil[i] > now) {
      timedLeft[i] = (pTimedUntil[i] - now) / 1000;
    }
  }

  const char *errStr = "NONE";
  if (ec == ERR_RADIO) errStr = "RADIO FAULT";
  else if (ec == ERR_PCF_I2C) errStr = "PCF8574 I2C FAULT";
  else if (ec == ERR_GATEWAY_TIMEOUT) errStr = "GATEWAY TIMEOUT";
  else if (ec == ERR_FLOAT_PLAUSIBILITY) errStr = "FLOAT PLAUSIBILITY FAULT";

  uint8_t f1 = (pcf_raw & 0x01) ? 1 : 0; // 0=submerged, 1=dry
  uint8_t f2 = (pcf_raw & 0x02) ? 1 : 0;
  uint8_t f3 = (pcf_raw & 0x04) ? 1 : 0;
  uint8_t bMode = (pcf_raw & 0x08) ? 1 : 0; // 0=pressed, 1=idle
  uint8_t bP1 = (pcf_raw & 0x10) ? 1 : 0;
  uint8_t bP2 = (pcf_raw & 0x20) ? 1 : 0;
  uint8_t bP3 = (pcf_raw & 0x40) ? 1 : 0;
  uint8_t bPond = (pcf_raw & 0x80) ? 1 : 0;

  snprintf(buf, sizeof(buf),
    "{\"type\":\"telemetry\",\"wl\":%u,\"raw_wl\":%u,\"wl_valid\":%d,\"mode\":\"%s\","
    "\"pcf_raw\":%u,\"pcf_ok\":%d,\"floats\":[%u,%u,%u],\"buttons\":[%u,%u,%u,%u,%u],"
    "\"pumps\":[%d,%d,%d],\"pins\":[%d,%d,%d],\"auto_mask\":[%d,%d,%d],"
    "\"run_left\":[%lu,%lu,%lu],\"cd_left\":[%lu,%lu,%lu],"
    "\"overrides\":[%u,%u,%u],\"timed_left\":[%lu,%lu,%lu],\"active_rule\":%u,"
    "\"rt_stats\":[%lu,%lu,%lu],\"pond\":%d,\"demand\":%d,\"err\":%u,\"err_str\":\"%s\","
    "\"temp\":%.1f,\"hum\":%u,\"rssi\":%d,\"snr\":%d,\"radio_ok\":%d,"
    "\"heap\":%lu,\"min_heap\":%lu,\"cpu_mhz\":%u,\"uptime\":%lu,\"boots\":%lu,"
    "\"sta\":%d,\"sta_ip\":\"%s\",\"sta_rssi\":%d,\"ap_clients\":%u,"
    "\"cfg_runtime\":%lu,\"cfg_cooldown\":%lu,\"cfg_telem\":%lu,\"always_on\":%u,"
    "\"tank_capacity\":%lu,\"refill_trigger\":%u,\"refill_cutoff\":%u,"
    "\"lora_freq\":%lu,\"lora_power\":%d,\"lora_sf\":%u,\"lora_bw\":%u,\"lora_cr\":%u,"
    "\"lora_ldro\":%u,\"ldro_active\":%u}",
    wl, raw_wl, wlValid ? 1 : 0, am ? "AUTO" : "MAN",
    pcf_raw, pcfOk ? 1 : 0, f1, f2, f3, bMode, bP1, bP2, bP3, bPond,
    ps[0], ps[1], ps[2], RELAY_P1, RELAY_P2, RELAY_P3, pea[0], pea[1], pea[2],
    (unsigned long)runLeft[0], (unsigned long)runLeft[1], (unsigned long)runLeft[2],
    (unsigned long)cdLeft[0], (unsigned long)cdLeft[1], (unsigned long)cdLeft[2],
    pOverride[0], pOverride[1], pOverride[2],
    (unsigned long)timedLeft[0], (unsigned long)timedLeft[1], (unsigned long)timedLeft[2],
    activeRule,
    (unsigned long)gStats.runtime_p1_s, (unsigned long)gStats.runtime_p2_s, (unsigned long)gStats.runtime_p3_s,
    pond ? 1 : 0, demand ? 1 : 0, ec, errStr,
    temp / 10.0, hum, rssi, snr, radioOk ? 1 : 0,
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(), (unsigned)ESP.getCpuFreqMHz(),
    (unsigned long)(millis() / 1000), (unsigned long)gStats.boot_count,
    WiFi.status() == WL_CONNECTED ? 1 : 0,
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "",
    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
    (unsigned)WiFi.softAPgetStationNum(),
    (unsigned long)gConfig.pump_min_runtime_ms,
    (unsigned long)gConfig.pump_min_cooldown_ms,
    (unsigned long)gConfig.telemetry_interval_ms,
    gConfig.web_always_on,
    (unsigned long)gConfig.tank_capacity_liters,
    gConfig.refill_trigger_level,
    gConfig.refill_cutoff_level,
    (unsigned long)gConfig.lora_freq_hz,
    gConfig.lora_tx_power_dbm,
    gConfig.lora_sf,
    gConfig.lora_bw_khz,
    gConfig.lora_cr,
    gConfig.lora_ldro,
    isLdroActive() ? 1 : 0
  );
  wsServer.broadcastTXT(buf);
}

static void sendLogsWs(uint8_t num) {
  if (!xLogMutex) return;
  if (xSemaphoreTake(xLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (uint16_t i = 0; i < gLogCount; i++) {
      uint16_t idx = (gLogHead + i) % MAX_SYSTEM_LOGS;
      char wsBuf[256];
      snprintf(wsBuf, sizeof(wsBuf),
        "{\"type\":\"log_event\",\"ts\":%lu,\"cat\":\"%s\",\"lvl\":%u,\"msg\":\"%s\"}",
        (unsigned long)gSystemLogs[idx].timestamp_s,
        gSystemLogs[idx].category,
        gSystemLogs[idx].level,
        gSystemLogs[idx].message);
      wsServer.sendTXT(num, wsBuf);
    }
    xSemaphoreGive(xLogMutex);
  }
}

static void sendConfigWs(uint8_t num) {
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"type\":\"config\",\"runtime\":%lu,\"cooldown\":%lu,\"telem\":%lu,"
    "\"boot_auto\":%u,\"debug\":%u,\"always_on\":%u,\"sta_ssid\":\"%s\","
    "\"ap_ip\":\"%s\",\"sta_ip\":\"%s\","
    "\"lora_freq\":%lu,\"lora_power\":%d,\"lora_sf\":%u,\"lora_bw\":%u,"
    "\"lora_cr\":%u,\"lora_sync\":\"0x%04X\",\"lora_ldro\":%u,\"ldro_active\":%u,"
    "\"tank_capacity\":%lu,\"refill_trigger\":%u,\"refill_cutoff\":%u}",
    (unsigned long)gConfig.pump_min_runtime_ms,
    (unsigned long)gConfig.pump_min_cooldown_ms,
    (unsigned long)gConfig.telemetry_interval_ms,
    gConfig.boot_auto_mode,
    gConfig.debug_mode_enabled,
    gConfig.web_always_on,
    gConfig.wifi_sta_ssid,
    WiFi.softAPIP().toString().c_str(),
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Disconnected",
    (unsigned long)gConfig.lora_freq_hz,
    gConfig.lora_tx_power_dbm,
    gConfig.lora_sf,
    gConfig.lora_bw_khz,
    gConfig.lora_cr,
    gConfig.lora_sync_word,
    gConfig.lora_ldro,
    isLdroActive() ? 1 : 0,
    (unsigned long)gConfig.tank_capacity_liters,
    gConfig.refill_trigger_level,
    gConfig.refill_cutoff_level
  );
  wsServer.sendTXT(num, buf);
}

static void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress ip = wsServer.remoteIP(num);
    Serial.printf("[DebugWS] Client #%u connected from %s\n", num, ip.toString().c_str());
    sendConfigWs(num);
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("[DebugWS] Client #%u disconnected\n", num);
  } else if (type == WStype_TEXT) {
    String msg = String((char *)payload);
    if (msg.indexOf("\"cmd\":\"get_config\"") >= 0) {
      sendConfigWs(num);
    } else if (msg.indexOf("\"cmd\":\"get_logs\"") >= 0) {
      sendLogsWs(num);
    } else if (msg.indexOf("\"cmd\":\"clear_logs\"") >= 0) {
      if (xLogMutex && xSemaphoreTake(xLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        gLogCount = 0;
        gLogHead = 0;
        xSemaphoreGive(xLogMutex);
      }
      logEvent("SYS", 0, "System logs cleared by operator");
      wsServer.sendTXT(num, "{\"type\":\"toast\",\"msg\":\"Event Logs Cleared!\",\"is_err\":false}");
    } else if (msg.indexOf("\"cmd\":\"mode\"") >= 0) {
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        gState.btnMode_edge = true;
        xSemaphoreGive(xStateMutex);
      }
      if (xControlTask) xTaskNotifyGive(xControlTask);
    } else if (msg.indexOf("\"cmd\":\"pond\"") >= 0) {
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        gState.btnPond_edge = true;
        xSemaphoreGive(xStateMutex);
      }
      if (xControlTask) xTaskNotifyGive(xControlTask);
    } else if (msg.indexOf("\"cmd\":\"pump\"") >= 0) {
      int idIdx = msg.indexOf("\"id\":");
      if (idIdx >= 0) {
        int pId = msg.substring(idIdx + 5).toInt();
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          if (pId == 1) gState.btnP1_edge = true;
          else if (pId == 2) gState.btnP2_edge = true;
          else if (pId == 3) gState.btnP3_edge = true;
          xSemaphoreGive(xStateMutex);
        }
        if (xControlTask) xTaskNotifyGive(xControlTask);
      }
    } else if (msg.indexOf("\"cmd\":\"set_pump_override\"") >= 0) {
      int idIdx = msg.indexOf("\"id\":");
      int mIdx = msg.indexOf("\"mode\":");
      int dIdx = msg.indexOf("\"duration\":");
      if (idIdx >= 0 && mIdx >= 0) {
        int pId = msg.substring(idIdx + 5).toInt();
        String mStr = msg.substring(mIdx + 7);
        int quoteStart = mStr.indexOf("\"");
        if (quoteStart >= 0) {
          int quoteEnd = mStr.indexOf("\"", quoteStart + 1);
          if (quoteEnd > quoteStart) mStr = mStr.substring(quoteStart + 1, quoteEnd);
        }
        uint32_t durMin = 0;
        if (dIdx >= 0) durMin = msg.substring(dIdx + 11).toInt();

        if (pId >= 1 && pId <= 3) {
          int idx = pId - 1;
          uint32_t nowMs = millis();
          PumpHysteresis *phArr[3] = {&phP1, &phP2, &phP3};

          if (mStr == "FORCE_ON") {
            uint8_t wl = 0;
            bool wlValid = false;
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              wl = gState.waterLevel;
              wlValid = gState.waterLevelValid;
              xSemaphoreGive(xStateMutex);
            }
            if (wlValid && wl == 0) {
              char toast[128];
              snprintf(toast, sizeof(toast), "{\"type\":\"toast\",\"msg\":\"Cannot start P%d: Tank is EMPTY (Cavitation Protection)\",\"is_err\":true}", pId);
              wsServer.sendTXT(num, toast);
            } else if (!canChangePumpState(*phArr[idx], true, false, gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
              uint32_t el = (phArr[idx]->lastOffTime > 0) ? (nowMs - phArr[idx]->lastOffTime) : 0;
              uint32_t cdRem = (el < gConfig.pump_min_cooldown_ms) ? (gConfig.pump_min_cooldown_ms - el) / 1000 : 0;
              char toast[128];
              snprintf(toast, sizeof(toast), "{\"type\":\"toast\",\"msg\":\"Cannot start P%d: Cooldown active (%lus left)\",\"is_err\":true}", pId, (unsigned long)cdRem);
              wsServer.sendTXT(num, toast);
            } else {
              if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                gState.pumpOverride[idx] = PUMP_OVERRIDE_FORCE_ON;
                gState.pumpTimedUntil[idx] = (durMin > 0) ? (nowMs + durMin * 60000UL) : 0;
                gState.triggerImmediateTelem = true;
                xSemaphoreGive(xStateMutex);
              }
              if (xControlTask) xTaskNotifyGive(xControlTask);
              char toast[160];
              if (durMin > 0) {
                snprintf(toast, sizeof(toast), "{\"type\":\"toast\",\"msg\":\"P%d Forced ON (%lu min timed run)\",\"is_err\":false}", pId, (unsigned long)durMin);
              } else {
                snprintf(toast, sizeof(toast), "{\"type\":\"toast\",\"msg\":\"P%d Forced ON (Manual Override Active)\",\"is_err\":false}", pId);
              }
              wsServer.sendTXT(num, toast);
            }
          } else if (mStr == "FORCE_OFF") {
            if (!canChangePumpState(*phArr[idx], false, false, gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
              uint32_t el = (phArr[idx]->lastOnTime > 0) ? (nowMs - phArr[idx]->lastOnTime) : 0;
              uint32_t runRem = (el < gConfig.pump_min_runtime_ms) ? (gConfig.pump_min_runtime_ms - el) / 1000 : 0;
              char toast[128];
              snprintf(toast, sizeof(toast), "{\"type\":\"toast\",\"msg\":\"Cannot stop P%d: Min runtime active (%lus left)\",\"is_err\":true}", pId, (unsigned long)runRem);
              wsServer.sendTXT(num, toast);
            } else {
              if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                gState.pumpOverride[idx] = PUMP_OVERRIDE_FORCE_OFF;
                gState.pumpTimedUntil[idx] = 0;
                gState.triggerImmediateTelem = true;
                xSemaphoreGive(xStateMutex);
              }
              if (xControlTask) xTaskNotifyGive(xControlTask);
              char toast[128];
              snprintf(toast, sizeof(toast), "{\"type\":\"toast\",\"msg\":\"P%d Forced OFF (Locked Out)\",\"is_err\":false}", pId);
              wsServer.sendTXT(num, toast);
            }
          } else { // "AUTO"
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              gState.pumpOverride[idx] = PUMP_OVERRIDE_AUTO;
              gState.pumpTimedUntil[idx] = 0;
              gState.triggerImmediateTelem = true;
              xSemaphoreGive(xStateMutex);
            }
            if (xControlTask) xTaskNotifyGive(xControlTask);
            char toast[128];
            snprintf(toast, sizeof(toast), "{\"type\":\"toast\",\"msg\":\"P%d Returned to AUTO Control\",\"is_err\":false}", pId);
            wsServer.sendTXT(num, toast);
          }
        }
      }
    } else if (msg.indexOf("\"cmd\":\"set_auto_mask\"") >= 0) {
      int mIdx = msg.indexOf("\"mask\":[");
      if (mIdx >= 0) {
        int p1 = msg.charAt(mIdx + 8) - '0';
        int p2 = msg.charAt(mIdx + 10) - '0';
        int p3 = msg.charAt(mIdx + 12) - '0';
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          gState.pumpEnabledAuto[0] = (p1 == 1);
          gState.pumpEnabledAuto[1] = (p2 == 1);
          gState.pumpEnabledAuto[2] = (p3 == 1);
          gConfig.pump_enabled_auto[0] = gState.pumpEnabledAuto[0];
          gConfig.pump_enabled_auto[1] = gState.pumpEnabledAuto[1];
          gConfig.pump_enabled_auto[2] = gState.pumpEnabledAuto[2];
          gState.triggerImmediateTelem = true;
          xSemaphoreGive(xStateMutex);
          saveConfig();
        }
        if (xControlTask) xTaskNotifyGive(xControlTask);
      }
    } else if (msg.indexOf("\"cmd\":\"set_config\"") >= 0) {
      int rIdx = msg.indexOf("\"runtime\":");
      int cIdx = msg.indexOf("\"cooldown\":");
      int tIdx = msg.indexOf("\"telem\":");
      int bIdx = msg.indexOf("\"boot_auto\":");
      int aIdx = msg.indexOf("\"always_on\":");
      if (rIdx >= 0) {
        uint32_t v = msg.substring(rIdx + 10).toInt();
        if (v >= CFG_RUNTIME_MIN_MS && v <= CFG_RUNTIME_MAX_MS) gConfig.pump_min_runtime_ms = v;
      }
      if (cIdx >= 0) {
        uint32_t v = msg.substring(cIdx + 11).toInt();
        if (v >= CFG_COOLDOWN_MIN_MS && v <= CFG_COOLDOWN_MAX_MS) gConfig.pump_min_cooldown_ms = v;
      }
      if (tIdx >= 0) {
        uint32_t v = msg.substring(tIdx + 8).toInt();
        if (v >= CFG_TELEM_MIN_MS && v <= CFG_TELEM_MAX_MS) gConfig.telemetry_interval_ms = v;
      }
      if (bIdx >= 0) {
        uint8_t v = msg.substring(bIdx + 12).toInt();
        if (v <= 1) gConfig.boot_auto_mode = v;
      }
      if (aIdx >= 0) {
        uint8_t a = msg.substring(aIdx + 12).toInt();
        if (a <= 1) {
          gConfig.web_always_on = a;
          gConfig.debug_mode_enabled = a;
        }
      }
      saveConfig(true);
      wsServer.sendTXT(num, "{\"type\":\"toast\",\"msg\":\"Config Saved to Flash!\",\"is_err\":false}");
      sendConfigWs(num);
    } else if (msg.indexOf("\"cmd\":\"set_lora_config\"") >= 0) {
      int fIdx = msg.indexOf("\"freq\":");
      int pIdx = msg.indexOf("\"power\":");
      int sfIdx = msg.indexOf("\"sf\":");
      int bwIdx = msg.indexOf("\"bw\":");
      int crIdx = msg.indexOf("\"cr\":");
      int ldIdx = msg.indexOf("\"ldro\":");
      int syIdx = msg.indexOf("\"sync\":");
      if (fIdx >= 0) {
        uint32_t f = msg.substring(fIdx + 7).toInt();
        if (f >= 150000000UL && f <= 960000000UL) gConfig.lora_freq_hz = f;
      }
      if (pIdx >= 0) {
        int8_t p = msg.substring(pIdx + 8).toInt();
        if (p >= -9 && p <= 22) gConfig.lora_tx_power_dbm = p;
      }
      if (sfIdx >= 0) {
        uint8_t sf = msg.substring(sfIdx + 5).toInt();
        if (sf >= 6 && sf <= 12) gConfig.lora_sf = sf;
      }
      if (bwIdx >= 0) {
        uint16_t bw = msg.substring(bwIdx + 5).toInt();
        if (bw == 125 || bw == 250 || bw == 500) gConfig.lora_bw_khz = bw;
      }
      if (crIdx >= 0) {
        uint8_t cr = msg.substring(crIdx + 5).toInt();
        if (cr >= 5 && cr <= 8) gConfig.lora_cr = cr;
      }
      if (ldIdx >= 0) {
        uint8_t ld = msg.substring(ldIdx + 7).toInt();
        if (ld <= 2) gConfig.lora_ldro = ld;
      }
      if (syIdx >= 0) {
        int sQuote = msg.indexOf("\"", syIdx + 7);
        if (sQuote >= 0) {
          int sEnd = msg.indexOf("\"", sQuote + 1);
          if (sEnd > sQuote) {
            String sVal = msg.substring(sQuote + 1, sEnd);
            uint16_t sy = (uint16_t)strtoul(sVal.c_str(), NULL, 16);
            if (sy != 0) gConfig.lora_sync_word = sy;
          }
        }
      }
      saveConfig(true);
      applyLoRaConfig();
      sendConfigWs(num);
      wsServer.sendTXT(num, "{\"type\":\"toast\",\"msg\":\"LoRa RF Config Applied & Saved!\",\"is_err\":false}");
    } else if (msg.indexOf("\"cmd\":\"set_hydraulic_config\"") >= 0) {
      int cIdx = msg.indexOf("\"capacity\":");
      int trIdx = msg.indexOf("\"trigger\":");
      int ctIdx = msg.indexOf("\"cutoff\":");
      if (cIdx >= 0) {
        uint32_t c = msg.substring(cIdx + 11).toInt();
        if (c >= 100 && c <= 1000000UL) gConfig.tank_capacity_liters = c;
      }
      if (trIdx >= 0) {
        uint8_t tr = msg.substring(trIdx + 10).toInt();
        if (tr <= 3) gConfig.refill_trigger_level = tr;
      }
      if (ctIdx >= 0) {
        uint8_t ct = msg.substring(ctIdx + 9).toInt();
        if (ct <= 3 && ct > gConfig.refill_trigger_level) gConfig.refill_cutoff_level = ct;
      }
      saveConfig(true);
      sendConfigWs(num);
      logEvent("HYDR", 3, "Updated capacity: %lu L, Refill trigger: L%u, Cutoff: L%u",
               (unsigned long)gConfig.tank_capacity_liters, gConfig.refill_trigger_level, gConfig.refill_cutoff_level);
      wsServer.sendTXT(num, "{\"type\":\"toast\",\"msg\":\"Hydraulic Parameters Saved!\",\"is_err\":false}");
    } else if (msg.indexOf("\"cmd\":\"set_wifi\"") >= 0) {
      int sIdx = msg.indexOf("\"ssid\":\"");
      int pIdx = msg.indexOf("\"pass\":\"");
      int aIdx = msg.indexOf("\"always_on\":");
      if (sIdx >= 0) {
        int sEnd = msg.indexOf("\"", sIdx + 8);
        if (sEnd > sIdx + 8) {
          String s = msg.substring(sIdx + 8, sEnd);
          memset(gConfig.wifi_sta_ssid, 0, sizeof(gConfig.wifi_sta_ssid));
          strncpy(gConfig.wifi_sta_ssid, s.c_str(), sizeof(gConfig.wifi_sta_ssid) - 1);
        } else if (sEnd == sIdx + 8) {
          memset(gConfig.wifi_sta_ssid, 0, sizeof(gConfig.wifi_sta_ssid));
        }
      }
      if (pIdx >= 0) {
        int pEnd = msg.indexOf("\"", pIdx + 8);
        if (pEnd >= pIdx + 8) {
          String p = msg.substring(pIdx + 8, pEnd);
          memset(gConfig.wifi_sta_pass, 0, sizeof(gConfig.wifi_sta_pass));
          strncpy(gConfig.wifi_sta_pass, p.c_str(), sizeof(gConfig.wifi_sta_pass) - 1);
        }
      }
      if (aIdx >= 0) {
        uint8_t a = msg.substring(aIdx + 12).toInt();
        if (a <= 1) {
          gConfig.web_always_on = a;
          gConfig.debug_mode_enabled = a;
        }
      }
      saveConfig(true);
      sendConfigWs(num);
      wsServer.sendTXT(num, "{\"type\":\"toast\",\"msg\":\"Wi-Fi & Startup Config Saved! Reboot to apply STA changes.\",\"is_err\":false}");
    } else if (msg.indexOf("\"cmd\":\"defaults\"") >= 0) {
      resetConfigToDefaults();
      saveConfig(true);
      applyLoRaConfig();
      sendConfigWs(num);
      wsServer.sendTXT(num, "{\"type\":\"toast\",\"msg\":\"Defaults Restored!\",\"is_err\":false}");
    } else if (msg.indexOf("\"cmd\":\"reboot\"") >= 0) {
      saveStats();
      saveConfig(true);
      wsServer.sendTXT(num, "{\"type\":\"toast\",\"msg\":\"Rebooting device...\",\"is_err\":false}");
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
    }
  }
}

static void setupOTA() {
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("tanknode");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {
      type = "filesystem";
    }
    logEvent("OTA", 1, "ArduinoOTA flashing started: %s", type.c_str());
  });
  ArduinoOTA.onEnd([]() {
    logEvent("OTA", 3, "ArduinoOTA flashing finished successfully. Restarting...");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // Keep progress clean
  });
  ArduinoOTA.onError([](ota_error_t error) {
    logEvent("OTA", 2, "ArduinoOTA error code: %u", error);
  });
  ArduinoOTA.begin();
  logEvent("OTA", 3, "ArduinoOTA server active on port 3232");
}

static void startDebugWeb() {
  if (gDebugServerRunning) return;
  LittleFS.begin(true);

  if (strlen(gConfig.wifi_sta_ssid) > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(gConfig.wifi_sta_ssid, gConfig.wifi_sta_pass);
    logEvent("WIFI", 0, "Connecting to Home Wi-Fi SSID '%s'...", gConfig.wifi_sta_ssid);
  } else {
    WiFi.mode(WIFI_AP);
  }

  WiFi.softAP(DEBUG_WIFI_SSID, DEBUG_WIFI_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    logEvent("WIFI", 3, "mDNS active: http://%s.local", MDNS_HOSTNAME);
  }

  httpServer.on("/", HTTP_GET, []() {
    if (LittleFS.exists("/index.html")) {
      File f = LittleFS.open("/index.html", "r");
      httpServer.streamFile(f, "text/html");
      f.close();
    } else {
      httpServer.send_P(200, "text/html", PAGE_INDEX_FALLBACK);
    }
  });

  httpServer.on("/style.css", HTTP_GET, []() {
    if (LittleFS.exists("/style.css")) {
      File f = LittleFS.open("/style.css", "r");
      httpServer.streamFile(f, "text/css");
      f.close();
    } else {
      httpServer.send(200, "text/css", "");
    }
  });

  httpServer.on("/app.js", HTTP_GET, []() {
    if (LittleFS.exists("/app.js")) {
      File f = LittleFS.open("/app.js", "r");
      httpServer.streamFile(f, "application/javascript");
      f.close();
    } else {
      httpServer.send(200, "application/javascript", "");
    }
  });

  httpServer.on("/api/status", HTTP_GET, []() {
    char buf[384];
    uint8_t wl = 0, ec = 0;
    bool am = false, pond = false, demand = false;
    bool ps[3] = {};
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      wl = gState.waterLevel; am = gState.autoMode; pond = gState.pond;
      demand = gState.demandActive; ec = gState.errorCode;
      for (int i=0; i<3; i++) ps[i] = gState.pumpState[i];
      xSemaphoreGive(xStateMutex);
    }
    snprintf(buf, sizeof(buf),
      "{\"wl\":%u,\"mode\":\"%s\",\"pumps\":[%d,%d,%d],\"pond\":%d,\"demand\":%d,\"err\":%u,\"uptime\":%lu}",
      wl, am?"AUTO":"MAN", ps[0], ps[1], ps[2], pond, demand, ec, (unsigned long)(millis()/1000));
    httpServer.send(200, "application/json", buf);
  });

  // Web OTA: Update firmware via web browser
  httpServer.on("/update", HTTP_GET, []() {
    httpServer.sendHeader("Connection", "close");
    httpServer.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta charset='utf-8'><title>TankNode Firmware OTA</title>"
      "<style>body{font-family:sans-serif;background:#050811;color:#f8fafc;padding:30px;text-align:center}"
      ".card{background:rgba(13,22,40,0.95);max-width:480px;margin:40px auto;padding:28px;border-radius:14px;border:1px solid rgba(0,240,255,0.3);box-shadow:0 8px 32px rgba(0,0,0,0.8)}"
      "h2{color:#00f0ff;margin-bottom:12px;font-size:1.3rem}"
      "p{color:#94a3b8;font-size:0.85rem;margin-bottom:20px}"
      "input[type=file]{margin:16px 0;padding:12px;background:#0b1120;color:#94a3b8;border-radius:8px;border:1px solid #1e293b;width:100%}"
      "input[type=submit]{background:linear-gradient(135deg,#00f0ff,#10b981);color:#050811;font-weight:700;border:none;padding:12px 20px;border-radius:8px;cursor:pointer;width:100%;font-size:0.95rem}"
      ".back{display:inline-block;margin-top:16px;color:#00f0ff;text-decoration:none;font-size:0.8rem}"
      "</style></head><body><div class='card'>"
      "<h2>Tank Node Wireless Flashing</h2>"
      "<p>Select compiled <code>firmware.bin</code> to flash over Wi-Fi without UART cable.</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update' accept='.bin' required><br>"
      "<input type='submit' value='Flash Firmware Now'>"
      "</form><a href='/' class='back'>&larr; Return to Dashboard</a></div></body></html>");
  });

  httpServer.on("/update", HTTP_POST, []() {
    httpServer.sendHeader("Connection", "close");
    if (Update.hasError()) {
      logEvent("OTA", 2, "Web OTA Update failed");
      httpServer.send(500, "text/plain", "Web OTA Update Failed");
    } else {
      logEvent("OTA", 3, "Web OTA Update successful. Rebooting...");
      httpServer.send(200, "text/html",
        "<!DOCTYPE html><html><body style='background:#050811;color:#00f0ff;text-align:center;padding:50px;font-family:sans-serif'>"
        "<h2>Update Successful!</h2><p style='color:#94a3b8'>Device is rebooting into the new firmware... Redirecting in 10 seconds.</p>"
        "<script>setTimeout(()=>{window.location.href='/';}, 10000);</script></body></html>");
      vTaskDelay(pdMS_TO_TICKS(600));
      esp_restart();
    }
  }, []() {
    HTTPUpload& upload = httpServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
      logEvent("OTA", 1, "Web OTA upload starting: %s", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        logEvent("OTA", 3, "Web OTA write completed (%u bytes)", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  httpServer.onNotFound([]() {
    httpServer.sendHeader("Location", "/", true);
    httpServer.send(302, "text/plain", "");
  });

  httpServer.begin();
  wsServer.begin();
  wsServer.onEvent(onWebSocketEvent);

  setupOTA();

  gDebugServerRunning = true;
  Serial.printf("\n[DebugWeb] Hotspot online! SSID='%s' Pass='%s' IP=%s\n",
                DEBUG_WIFI_SSID, DEBUG_WIFI_PASS, WiFi.softAPIP().toString().c_str());
  Serial.println("[DebugWeb] Access via http://192.168.4.1 or http://tanknode.local");
}

static void stopDebugWeb() {
  if (!gDebugServerRunning) return;
  MDNS.end();
  wsServer.close();
  httpServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  gDebugServerRunning = false;
  Serial.println("\n[DebugWeb] Hotspot stopped & WiFi powered OFF.");
}

static void Task_DebugWebServer(void *) {
  for (;;) {
    if (gDebugServerWanted != gDebugServerRunning) {
      if (gDebugServerWanted) startDebugWeb();
      else stopDebugWeb();
    }

    if (gDebugServerRunning) {
      ArduinoOTA.handle();
      dnsServer.processNextRequest();
      httpServer.handleClient();
      wsServer.loop();

      uint32_t now = millis();
      static uint32_t lastWsBc = 0;
      if (now - lastWsBc >= WS_BROADCAST_INTERVAL_MS) {
        lastWsBc = now;
        broadcastTelemetryWs();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

// ─── Input sensor task (Core 1, Pri 3) ────────────────────────────────────────
static void Task_InputSensorPoll(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint8_t prevBtns = 0xFF;
  uint32_t retryAt = 0;
  uint8_t candidateLvl = 0xFF;
  uint32_t candidateSince = 0;
  uint8_t lastStableLvl = 0xFF;
  uint32_t lastBtnEdgeTime = 0;

  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_POLL_MS));
    uint32_t now = millis();

    bool pcfOk = false;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      pcfOk = gState.pcfOk;
      xSemaphoreGive(xStateMutex);
    }

    if (!pcfOk) {
      if (now >= retryAt) {
        retryAt = now + PCF_RETRY_MS;
        Wire.beginTransmission(PCF8574_ADDR);
        bool found = (Wire.endTransmission() == 0);
        if (found) {
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            gState.pcfOk = true;
            xSemaphoreGive(xStateMutex);
          }
          Serial.println("\n[PCF8574] Connected");
        }
      }
      continue;
    }

    Wire.requestFrom((uint8_t)PCF8574_ADDR, (uint8_t)1);
    if (!Wire.available()) {
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        gState.pcfOk = false;
        gState.waterLevelValid = false;
        gState.errorCode = ERR_PCF_I2C;
        xSemaphoreGive(xStateMutex);
      }
      retryAt = millis() + PCF_RETRY_MS;
      continue;
    }
    uint8_t curr = Wire.read();

    // ── FLOAT SENSOR PLAUSIBILITY CHECK ──
    // Float switches are bits 0, 1, 2. (Active-LOW: 0 = submerged/closed, 1 = dry/open)
    uint8_t floatBits = curr & 0x07;
    uint8_t rawLvl = 0;
    bool plausibilityOk = false;

    // Allowed continuous physical configurations:
    // 0b111 (7): All dry -> Level 0
    // 0b110 (6): Float 1 submerged, 2&3 dry -> Level 1
    // 0b100 (4): Floats 1&2 submerged, 3 dry -> Level 2
    // 0b000 (0): Floats 1,2&3 submerged -> Level 3
    if (floatBits == 0x07) {
      rawLvl = 0;
      plausibilityOk = true;
    } else if (floatBits == 0x06) {
      rawLvl = 1;
      plausibilityOk = true;
    } else if (floatBits == 0x04) {
      rawLvl = 2;
      plausibilityOk = true;
    } else if (floatBits == 0x00) {
      rawLvl = 3;
      plausibilityOk = true;
    } else {
      // Inconsistent / impossible combination (e.g. 0b101, 0b011, 0b010, 0b001)
      plausibilityOk = false;
    }

    // Debouncing water level transitions to filter wave ripples/sloshing
    bool levelChanged = false;
    if (plausibilityOk) {
      if (rawLvl != candidateLvl) {
        candidateLvl = rawLvl;
        candidateSince = now;
      } else if ((now - candidateSince) >= FLOAT_DEBOUNCE_MS) {
        if (lastStableLvl != candidateLvl) {
          lastStableLvl = candidateLvl;
          levelChanged = true;
        }
      }
    }

    // ── BUTTON DEBOUNCE (50ms) ──
    uint8_t edges = 0;
    if (now - lastBtnEdgeTime >= BTN_DEBOUNCE_MS) {
      edges = (~prevBtns) & curr & PCF_BTN_MASK;
      if (edges) lastBtnEdgeTime = now;
    }
    prevBtns = curr;

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      gState.pcfRaw = curr;
      gState.rawWaterLevel = rawLvl;
      if (!plausibilityOk) {
        gState.waterLevelValid = false;
        gState.errorCode = ERR_FLOAT_PLAUSIBILITY;
      } else {
        if (gState.errorCode == ERR_FLOAT_PLAUSIBILITY || gState.errorCode == ERR_PCF_I2C) {
          gState.errorCode = ERR_NONE;
        }
        if (lastStableLvl != 0xFF) {
          gState.waterLevel = lastStableLvl;
          gState.waterLevelValid = true;
          if (levelChanged) {
            gState.triggerImmediateTelem = true;
          }
        }
      }

      if (edges & (1 << PCF_BTN_BIT_MODE))   gState.btnMode_edge = true;
      if (edges & (1 << PCF_BTN_BIT_P1))     gState.btnP1_edge = true;
      if (edges & (1 << PCF_BTN_BIT_P2))     gState.btnP2_edge = true;
      if (edges & (1 << PCF_BTN_BIT_P3))     gState.btnP3_edge = true;
      if (edges & (1 << PCF_BTN_POND_P4))    gState.btnPond_edge = true;
      xSemaphoreGive(xStateMutex);
    }
  }
}

// ─── Control engine task (Core 1, Pri 4) ──────────────────────────────────────
static void Task_ControlEngine(void *) {
  const int relayPin[3] = {RELAY_P1, RELAY_P2, RELAY_P3};
  PumpHysteresis *ph[3] = {&phP1, &phP2, &phP3};
  uint32_t *pRtStats[3] = {&gStats.runtime_p1_s, &gStats.runtime_p2_s, &gStats.runtime_p3_s};
  uint32_t pOnSince[3] = {};
  uint32_t lastTelem = 0;
  uint32_t lastPondBroadcast = 0; 
  uint32_t lastStatusPrint = 0;
  uint32_t lastGwMs = 0;
  bool gwSeen = false;
  bool autoMode = false; 

  // Persistent tank dual-threshold charge latch (Pond Pump Driver)
  static bool systemDemandOn = gConfig.last_demand_state;
  static bool pondManualOverride = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CTRL_TICK_MS));
    uint32_t now = millis();

    uint8_t waterLevel = 0, errorCode = 0;
    bool radioOk = false, waterLevelValid = false;
    bool btnMode = false, bp1 = false, bp2 = false, bp3 = false, bPond = false;

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      autoMode = gState.autoMode;       
      waterLevel = gState.waterLevel;
      waterLevelValid = gState.waterLevelValid;
      errorCode = gState.errorCode;
      radioOk = gState.radioOk;
      btnMode = gState.btnMode_edge;   gState.btnMode_edge = false;
      bp1 = gState.btnP1_edge;         gState.btnP1_edge = false;
      bp2 = gState.btnP2_edge;         gState.btnP2_edge = false;
      bp3 = gState.btnP3_edge;         gState.btnP3_edge = false;
      bPond = gState.btnPond_edge;     gState.btnPond_edge = false; 
      xSemaphoreGive(xStateMutex);
    }

    // ── AUTOMATIC TANK CHARGING HYSTERESIS (Remote Pond Controller) ──
    if (autoMode && waterLevelValid) {
      // Auto refill starts when water level drops to or below trigger level
      if (waterLevel <= gConfig.refill_trigger_level && !systemDemandOn) {
        systemDemandOn = true;
        gConfig.pond_enabled = true;
        pondManualOverride = false; // Normal automated cycle
        saveConfig();
        logEvent("HYDR", 1, "Water low (%u/3 <= L%u) -> Auto Refill Demand ON", waterLevel, gConfig.refill_trigger_level);
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          gState.triggerImmediateTelem = true;
          xSemaphoreGive(xStateMutex);
        }
      } else if (systemDemandOn) {
        if (pondManualOverride) {
          // Operator manual boost in AUTO mode: ONLY cut off if reservoir is 100% full (waterLevel >= 3) to prevent overflow
          if (waterLevel >= 3) {
            systemDemandOn = false;
            gConfig.pond_enabled = false;
            pondManualOverride = false;
            saveConfig();
            logEvent("HYDR", 3, "Water full (3/3) -> Manual Pond Boost Safeguard Cutoff OFF");
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              gState.triggerImmediateTelem = true;
              xSemaphoreGive(xStateMutex);
            }
          }
        } else {
          // Standard auto refill cycle: cutoff at configured level
          if (waterLevel >= gConfig.refill_cutoff_level) {
            systemDemandOn = false;
            gConfig.pond_enabled = false;
            saveConfig();
            logEvent("HYDR", 3, "Water full (%u/3 >= L%u) -> Auto Refill Demand OFF", waterLevel, gConfig.refill_cutoff_level);
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              gState.triggerImmediateTelem = true;
              xSemaphoreGive(xStateMutex);
            }
          }
        }
      }
    } else if (!autoMode && waterLevelValid) {
      // MANUAL MODE overflow safeguard: if tank reaches 100% full (3/3), safely cut off refill
      if (gConfig.pond_enabled && waterLevel >= 3) {
        gConfig.pond_enabled = false;
        systemDemandOn = false;
        pondManualOverride = false;
        saveConfig();
        logEvent("HYDR", 3, "Water full (3/3) -> Manual Mode Overflow Safeguard Cutoff OFF");
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          gState.triggerImmediateTelem = true;
          xSemaphoreGive(xStateMutex);
        }
      }
    }

    // Save cycle latch state to NVS flash if it modified
    if (systemDemandOn != gConfig.last_demand_state) {
      gConfig.last_demand_state = systemDemandOn;
      saveConfig();
    }

    // In automatic mode, pond enabled mirrors system demand latch
    if (autoMode) {
      gConfig.pond_enabled = systemDemandOn;
    }

    // ── Single-Line Diagnostic Display Overwriting via \r ────────────────────
    if (now - lastStatusPrint >= STATUS_PRINT_MS) {
      lastStatusPrint = now;
      char pumpStr[120] = {0};
      
      for (int i = 0; i < 3; i++) {
        if (ph[i]->state) sprintf(pumpStr + strlen(pumpStr), "P%d:ON ", i + 1);
        else sprintf(pumpStr + strlen(pumpStr), "P%d:OFF ", i + 1);
      }

      uint32_t gwAgo = gwSeen ? (now - lastGwMs) / 1000 : 0;
      Serial.printf("\r[Status] Up:%lus WL:%u Mode:%s Charge Latch:%d Pond:%d | %s| GW:%lus Err:%u            ", 
                    now / 1000, waterLevel, autoMode ? "AUTO" : "MAN", systemDemandOn, gConfig.pond_enabled,
                    pumpStr, gwAgo, errorCode);
    }

    if (radioOk && gwSeen) {
      if ((now - lastGwMs) >= DEFAULT_NETWORK_TIMEOUT_MS) errorCode = ERR_GATEWAY_TIMEOUT;
      else if (errorCode == ERR_GATEWAY_TIMEOUT) errorCode = ERR_NONE;
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        gState.errorCode = errorCode;
        xSemaphoreGive(xStateMutex);
      }
    }

    if (btnMode) {
      autoMode = !autoMode;
      gConfig.boot_auto_mode = autoMode ? 1 : 0;
      saveConfig();
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        gState.autoMode = autoMode;
        gState.triggerImmediateTelem = true;
        xSemaphoreGive(xStateMutex);
      }
      Serial.printf("\n[Ctrl] Mode -> %s\n", autoMode ? "AUTO" : "MANUAL");
    }

    if (bPond) {
      bool newPond = !gConfig.pond_enabled;
      gConfig.pond_enabled = newPond;
      systemDemandOn = newPond;
      pondManualOverride = newPond;
      saveConfig();
      Serial.printf("\n[Ctrl] %s Pond Request -> %s (Saved, Override=%d)\n", autoMode ? "Auto Override" : "Manual", gConfig.pond_enabled ? "ON" : "OFF", pondManualOverride);
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        gState.demandActive = gConfig.pond_enabled;
        gState.triggerImmediateTelem = true;
        xSemaphoreGive(xStateMutex);
      }
    }

    LoRaPacket rx;
    while (xQueueReceive(xRxQueue, &rx, 0) == pdTRUE) {
      if (rx.header.sender_id != gConfig.gateway_id && rx.header.sender_id != gConfig.pond_id) continue;

      if (rx.header.sender_id == gConfig.gateway_id || rx.header.sender_id == gConfig.pond_id) {
        lastGwMs = now;
        gwSeen = true;
      }
      
      if (rx.header.sender_id == gConfig.pond_id && rx.header.msg_type == MSG_STATUS) {
        bool pondRunning = bool(rx.payload.status.flags & STATUS_BIT_POND);
        bool changed = false;
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          if (gState.pond != pondRunning) {
            gState.pond = pondRunning;
            changed = true;
          }
          if (changed) {
            gState.triggerImmediateTelem = true; // Instantly broadcast back to reach mutual accord!
          }
          xSemaphoreGive(xStateMutex);
        }
        Serial.printf("\n[Ctrl] Pond Node 0x03 Status over LoRa -> Pump %s (Accord telem: %s)\n", 
                      pondRunning ? "RUNNING" : "STOPPED", changed ? "YES" : "NO");
      }

      if (rx.header.msg_type == MSG_CMD) {
        uint8_t target = rx.payload.cmd.target;
        uint8_t action = rx.payload.cmd.action;
        uint8_t value = rx.payload.cmd.value;
        bool setVal = (action == CMD_ACTION_SET);
        Serial.printf("\n[Ctrl] CMD from 0x%02X (%s) -> target=0x%02X action=%u value=%u\n", 
                      rx.header.sender_id, (rx.header.sender_id == gConfig.pond_id ? "POND" : (rx.header.sender_id == gConfig.gateway_id ? "GW" : "NODE")),
                      target, action, value);

        // Security boundary: Pond cannot alter Tank Mode (Auto/Manual) or Outflow Pumps (P1-P3)!
        if (rx.header.sender_id == gConfig.pond_id) {
          if (target & (CMD_TARGET_MODE | CMD_TARGET_P1 | CMD_TARGET_P2 | CMD_TARGET_P3)) {
            Serial.printf("[Ctrl] Blocked unauthorized command from Pond on Tank core controls!\n");
            continue;
          }
        }

        if (target & CMD_TARGET_MODE) {
          autoMode = setVal ? (bool)value : !autoMode;
          gConfig.boot_auto_mode = autoMode;
          saveConfig();
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            gState.autoMode = autoMode;
            gState.triggerImmediateTelem = true;
            xSemaphoreGive(xStateMutex);
          }
          Serial.printf("[Ctrl] Mode -> %s\n", autoMode ? "AUTO" : "MANUAL");
        }
        
        if (target & CMD_TARGET_POND) {
          bool newPond = setVal ? (bool)value : !gConfig.pond_enabled;
          if (gConfig.pond_enabled != newPond) {
            gConfig.pond_enabled = newPond;
            systemDemandOn = newPond;
            pondManualOverride = newPond;
            saveConfig();
            Serial.printf("[Ctrl] Remote Pond Switch Command -> %s (Override=%d)\n", gConfig.pond_enabled ? "ON" : "OFF", pondManualOverride);
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              gState.demandActive = newPond;
              gState.triggerImmediateTelem = true;
              xSemaphoreGive(xStateMutex);
            }
          }
        }

        for (int i = 0; i < 3; i++) {
          uint8_t pumpTargetMask = (i == 0) ? CMD_TARGET_P1 : ((i == 1) ? CMD_TARGET_P2 : CMD_TARGET_P3);
          if (target & pumpTargetMask) {
            bool want = setVal ? (bool)value : !ph[i]->state;
            if (autoMode) {
              if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                gState.pumpEnabledAuto[i] = want;
                gConfig.pump_enabled_auto[i] = want; 
                gState.triggerImmediateTelem = true;
                xSemaphoreGive(xStateMutex);
                saveConfig(); 
                Serial.printf("[Ctrl] Auto P%d selection via LoRa -> %s (Saved)\n", i + 1, gConfig.pump_enabled_auto[i] ? "SELECTED" : "DESELECTED");
              }
            } else {
              uint32_t nowMs = millis();
              if (want && !canChangePumpState(*ph[i], true, false, gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
                uint32_t el = (ph[i]->lastOffTime > 0) ? (nowMs - ph[i]->lastOffTime) : 0;
                uint32_t cdRem = (el < gConfig.pump_min_cooldown_ms) ? (gConfig.pump_min_cooldown_ms - el) / 1000 : 0;
                Serial.printf("[Ctrl] Manual P%d via LoRa rejected: cooldown active (%lus left)\n", i + 1, cdRem);
                if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                  gState.triggerImmediateTelem = true;
                  xSemaphoreGive(xStateMutex);
                }
              } else if (!want && !canChangePumpState(*ph[i], false, false, gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
                uint32_t el = (ph[i]->lastOnTime > 0) ? (nowMs - ph[i]->lastOnTime) : 0;
                uint32_t runRem = (el < gConfig.pump_min_runtime_ms) ? (gConfig.pump_min_runtime_ms - el) / 1000 : 0;
                Serial.printf("[Ctrl] Manual P%d via LoRa rejected: min runtime active (%lus left)\n", i + 1, runRem);
                if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                  gState.triggerImmediateTelem = true;
                  xSemaphoreGive(xStateMutex);
                }
              } else {
                if (!want && ph[i]->state) {
                  *pRtStats[i] += (millis() - pOnSince[i]) / 1000;
                } else if (want) {
                  pOnSince[i] = millis();
                }
                setPumpState(*ph[i], want, relayPin[i]);
                gConfig.pump_manual_state[i] = want;
                saveConfig();
                Serial.printf("[Ctrl] Manual P%d via LoRa -> %s (saved)\n", i + 1, want ? "ON" : "OFF");
                if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                  gState.triggerImmediateTelem = true;
                  xSemaphoreGive(xStateMutex);
                }
              }
            }
          }
        }
      }
    }

    bool btns[3] = {bp1, bp2, bp3};
    for (int i = 0; i < 3; i++) {
      if (!btns[i]) continue;

      if (autoMode) {
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          gState.pumpEnabledAuto[i] = !gState.pumpEnabledAuto[i];
          gConfig.pump_enabled_auto[i] = gState.pumpEnabledAuto[i]; 
          gState.triggerImmediateTelem = true;
          xSemaphoreGive(xStateMutex);
          saveConfig(); 
          Serial.printf("\n[Ctrl] Auto P%d selection via Button -> %s (Saved)\n", i + 1, gConfig.pump_enabled_auto[i] ? "SELECTED" : "DESELECTED");
        }
      } else {
        bool want = !ph[i]->state;
        if (want && !canChangePumpState(*ph[i], true, false, gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
          Serial.printf("\n[Ctrl] P%d toggle rejected: cooldown active\n", i + 1);
        } else {
          if (!want && ph[i]->state) {
            *pRtStats[i] += (millis() - pOnSince[i]) / 1000;
          } else if (want) {
            pOnSince[i] = millis();
          }
          setPumpState(*ph[i], want, relayPin[i]);
          gConfig.pump_manual_state[i] = want;
          saveConfig();
          Serial.printf("\n[Ctrl] P%d -> %s (saved)\n", i + 1, want ? "ON" : "OFF");
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            gState.triggerImmediateTelem = true;
            xSemaphoreGive(xStateMutex);
          }
        }
      }
    }

    // ── PER-PUMP TIMED RUN CHECK & HYDRAULIC AUTOMATION RULES ─────────────────
    uint8_t pOverride[3] = {};
    uint32_t pTimedUntil[3] = {};
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      for (int i = 0; i < 3; i++) {
        pOverride[i] = gState.pumpOverride[i];
        pTimedUntil[i] = gState.pumpTimedUntil[i];
        if (pOverride[i] == PUMP_OVERRIDE_FORCE_ON && pTimedUntil[i] > 0 && now >= pTimedUntil[i]) {
          gState.pumpOverride[i] = PUMP_OVERRIDE_AUTO;
          gState.pumpTimedUntil[i] = 0;
          pOverride[i] = PUMP_OVERRIDE_AUTO;
          pTimedUntil[i] = 0;
          gState.triggerImmediateTelem = true;
          logEvent("RELAY", 1, "P%d Timed Run Finished -> Returned to AUTO", i + 1);
        }
      }

      // Calculate active operational rule for SCADA matrix
      uint8_t currentRule = 3;
      if (waterLevelValid && waterLevel == 0) {
        currentRule = 2; // Rule 2: Emergency Dry Trip (Cavitation Safeguard)
      } else if (autoMode && waterLevelValid && waterLevel <= gConfig.refill_trigger_level) {
        currentRule = 1; // Rule 1: Tank Low (Auto-Refill Demand ON)
      } else if (waterLevelValid && waterLevel >= gConfig.refill_cutoff_level) {
        currentRule = 4; // Rule 4: Tank Full (Refill Satisfied / Cutoff)
      } else {
        currentRule = 3; // Rule 3: Nominal Water Available (Parallel Outflow Active)
      }
      gState.activeRule = currentRule;

      xSemaphoreGive(xStateMutex);
    }

    // ── PARALLEL IRRIGATION CONTROL ENGINE WITH INDEPENDENT OVERRIDES & ANTI-INRUSH STAGGER ──
    bool allowed[3] = {true, true, true};
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      allowed[0] = gState.pumpEnabledAuto[0];
      allowed[1] = gState.pumpEnabledAuto[1];
      allowed[2] = gState.pumpEnabledAuto[2];
      xSemaphoreGive(xStateMutex);
    }

    bool waterAvailable = (waterLevelValid && waterLevel > 0);
    bool emergencyDry = (waterLevelValid && waterLevel == 0);
    static uint32_t lastMotorStartMs = 0;

    for (int i = 0; i < 3; i++) {
      bool targetState = false;

      // Independent per-pump override evaluation
      if (pOverride[i] == PUMP_OVERRIDE_FORCE_OFF) {
        targetState = false; // Forced OFF (Hard Lockout)
      } else if (pOverride[i] == PUMP_OVERRIDE_FORCE_ON) {
        // Forced ON (Hand): Runs directly in AUTO or MANUAL, but dry-run empty tank shuts down to protect pump
        targetState = waterAvailable;
      } else { // PUMP_OVERRIDE_AUTO
        if (autoMode) {
          targetState = waterAvailable && allowed[i];
        } else {
          targetState = gConfig.pump_manual_state[i];
        }
      }

      if (targetState != ph[i]->state) {
        // Anti-inrush stagger check: delay simultaneous motor starts by MOTOR_STAGGER_MS
        if (targetState && (now - lastMotorStartMs < MOTOR_STAGGER_MS)) {
          continue; // Wait for stagger interval before starting next motor
        }

        if (canChangePumpState(*ph[i], targetState, emergencyDry,
                               gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
          if (!targetState && ph[i]->state) {
            *pRtStats[i] += (millis() - pOnSince[i]) / 1000;
          } else if (targetState) {
            pOnSince[i] = millis();
            lastMotorStartMs = now;
          }
          setPumpState(*ph[i], targetState, relayPin[i]);
          Serial.printf("\n[Ctrl] Outflow Pump P%d -> %s (WL=%u, Override=%u)\n", 
                        i + 1, targetState ? "ON" : "OFF", waterLevel, pOverride[i]);

          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            gState.triggerImmediateTelem = true;
            xSemaphoreGive(xStateMutex);
          }
        }
      }
    }

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      for (int i = 0; i < 3; i++) {
        gState.pumpState[i] = ph[i]->state;
        gState.pumpLastOn[i] = ph[i]->lastOnTime;
        gState.pumpLastOff[i] = ph[i]->lastOffTime;
      }
      gState.demandActive = systemDemandOn;
      gState.tankEmptyAlarm = (autoMode && waterLevelValid && waterLevel == 0);
      gState.loraTimeoutAlarm = (gwSeen && (now - lastGwMs >= LORA_WATCHDOG_TIMEOUT_MS));
      if (gState.psramOk) {
        gState.psramFreeBytes = ESP.getFreePsram();
      }
      xSemaphoreGive(xStateMutex);
    }

    // ── PERIODIC PSRAM IN-MEMORY TELEMETRY SNAPSHOT ─────────────────────────
    static uint32_t lastHistoryLog = 0;
    if (gHistory && (now - lastHistoryLog >= HISTORY_SAMPLE_INTERVAL_MS)) {
      lastHistoryLog = now;
      uint16_t idx = (gHistoryHead + gHistoryCount) % PSRAM_HISTORY_CAPACITY;
      if (gHistoryCount == PSRAM_HISTORY_CAPACITY) {
        gHistoryHead = (gHistoryHead + 1) % PSRAM_HISTORY_CAPACITY;
      } else {
        gHistoryCount++;
      }
      uint8_t flags = (autoMode ? STATUS_BIT_AUTO : 0) |
                      (ph[0]->state ? STATUS_BIT_P1 : 0) |
                      (ph[1]->state ? STATUS_BIT_P2 : 0) |
                      (ph[2]->state ? STATUS_BIT_P3 : 0) |
                      (gConfig.pond_enabled ? STATUS_BIT_POND : 0);
      gHistory[idx] = { now / 1000, waterLevel, flags, gState.temperature, gState.humidity };
    }

    // ── PERIODIC NVS STATS FLUSH (Protects Flash Endurance) ─────────────────
    static uint32_t lastStatsSave = 0;
    if (now - lastStatsSave >= NVS_STATS_SAVE_INTERVAL_MS) {
      lastStatsSave = now;
      for (int i = 0; i < 3; i++) {
        if (ph[i]->state && pOnSince[i] > 0) {
          uint32_t elapsed = (now - pOnSince[i]) / 1000;
          *pRtStats[i] += elapsed;
          pOnSince[i] = now;
        }
      }
      saveStats();
      Serial.println("\n[NVS] Periodic pump stats saved to flash");
    }

    // ── EVENT-DRIVEN & PERIODIC TELEMETRY DISPATCH ──────────────────────────
    bool triggerTelem = false;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      triggerTelem = gState.triggerImmediateTelem;
      gState.triggerImmediateTelem = false;
      xSemaphoreGive(xStateMutex);
    }

    if (radioOk && (triggerTelem || ((now - lastTelem) >= gConfig.telemetry_interval_ms))) {
      lastTelem = now;
      LoRaPacket tx = {};
      buildStatus(tx);
      xQueueSend(xTxQueue, &tx, 0);
    }

    // ── DISPATCH BROADCAST FOR THE REMOTE POND CHARGER ──────────────────────
    static uint32_t lastPondBroadcast = 0;
    static uint8_t pondRetryCount = 0;
    static bool lastDispatchedPond = false;
    bool pondChanged = (gConfig.pond_enabled != lastDispatchedPond);
    bool pondAccordSynced = false;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      pondAccordSynced = (gState.pond == gConfig.pond_enabled);
      xSemaphoreGive(xStateMutex);
    }
    uint32_t accordRetryInterval = (gConfig.lora_sf >= 12) ? 6000UL :
                                   (gConfig.lora_sf == 11) ? 4500UL : 3000UL;
    bool needRetry = (!pondAccordSynced) && (pondRetryCount < 5) && ((now - lastPondBroadcast) >= accordRetryInterval);
    bool periodicBroadcast = (now - lastPondBroadcast >= gConfig.telemetry_interval_ms);

    if (radioOk && (pondChanged || needRetry || periodicBroadcast)) {
      lastPondBroadcast = now;
      if (pondChanged) {
        lastDispatchedPond = gConfig.pond_enabled;
        pondRetryCount = 0;
      } else if (needRetry) {
        pondRetryCount++;
        Serial.printf("\n[Ctrl] Awaiting Pond Accord ACK (Retry #%u/5)...\n", pondRetryCount);
      }
      LoRaPacket txPkt = {};
      txPkt.header = {gConfig.network_magic, gConfig.pond_id, gConfig.node_id, MSG_CMD};
      txPkt.payload.cmd = {CMD_TARGET_POND, CMD_ACTION_SET, gConfig.pond_enabled ? (uint8_t)1 : (uint8_t)0}; 
      xQueueSend(xTxQueue, &txPkt, 0);
      Serial.printf("\n[Ctrl] Dispatched remote Pond charge state broadcast via LoRa -> %d\n", gConfig.pond_enabled);
    }
  }
}

// ─── Sensor read task (Core 1, Pri 3) ───────────────────────────────────────
static void Task_ReadSensor(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(SENSOR_READ_MS));
    int16_t temp = 63; 
    uint8_t hum = 42;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      gState.temperature = temp;
      gState.humidity = hum;
      xSemaphoreGive(xStateMutex);
    }
  }
}

// ─── LED animation task (Core 1, Pri 1) ───────────────────────────────────────
static void Task_UIAnimation(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t rxEnd = 0, txEnd = 0;

  static const CRGB lvlColor[3] = {CRGB(LED_COLOR_LVL1), CRGB(LED_COLOR_LVL2), CRGB(LED_COLOR_LVL3)};
  static const CRGB dimBase = CRGB(LED_COLOR_DIM);

  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_REFRESH_MS));
    uint32_t now = millis();

    uint8_t wl = 0, ec = 0;
    bool am = false, pond = false, ps[3] = {}, pea[3] = {}, rxF = false, txF = false, demand = false;
    bool emptyAlarm = false, loraAlarm = false;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      wl = gState.waterLevel;
      am = gState.autoMode;
      ps[0]  = gState.pumpState[0];
      ps[1]  = gState.pumpState[1];
      ps[2]  = gState.pumpState[2];
      pea[0] = gState.pumpEnabledAuto[0];
      pea[1] = gState.pumpEnabledAuto[1];
      pea[2] = gState.pumpEnabledAuto[2];
      demand = gState.demandActive;
      pond   = gState.pond;
      ec     = gState.errorCode;
      emptyAlarm = gState.tankEmptyAlarm;
      loraAlarm  = gState.loraTimeoutAlarm;
      rxF    = gState.rxFlash;        gState.rxFlash = false;
      txF    = gState.txFlash;        gState.txFlash = false;
      xSemaphoreGive(xStateMutex);
    }
    if (rxF) rxEnd = now + FLASH_RX_MS;
    if (txF) txEnd = now + FLASH_TX_MS;

    fill_solid(leds, NUM_LEDS, CRGB::Black);

    bool anyPumpArmed = pea[0] || pea[1] || pea[2];
    if (!am || !anyPumpArmed) leds[0] = CRGB::Red;                      
    else if (demand) leds[0] = CRGB(255, 80, 0);  // Orange: Tank charging cycle running             
    else leds[0] = CRGB(0, 80, 255);              // Blue: Tank satisfied/full 

    for (int i = 0; i < 3; i++) {
      if (emptyAlarm) {
        // Alarm: Tank empty in Auto -> Rapid red alert strobe on water indicators
        leds[1 + i] = (now % 400 < 200) ? CRGB::Red : CRGB::Black;
      } else {
        leds[1 + i] = (i < (int)wl) ? lvlColor[i] : dimBase;
      }
    }

    if (loraAlarm) {
      // Alarm: LoRa signal loss watchdog -> Amber pulse warning on status LED
      leds[4] = (now % 800 < 400) ? CRGB(255, 120, 0) : CRGB::Black;
    }
    else if (ec == ERR_GATEWAY_TIMEOUT) leds[4] = CRGB::Red;
    else if (ec == ERR_FLOAT_PLAUSIBILITY) leds[4] = CRGB::Magenta;
    else if (ec == ERR_PCF_I2C) leds[4] = CRGB::Yellow;
    else if (now < rxEnd) leds[4] = CRGB(LED_COLOR_RX_FLASH);
    else if (now < txEnd) leds[4] = CRGB(LED_COLOR_TX_FLASH);
    else if (am) leds[4] = CRGB::Green;
    else leds[4] = CRGB::Orange;

    leds[5] = pond ? CRGB::Green : CRGB::Red;
    leds[6] = am ? CRGB::Green : CRGB::Blue;

    // ── FIXED: PREDICTIVE PARALLEL WATER CAPICITY LED FADING SCHEME ──────────
    // Reverted UI layout indexing: P1=LED9, P2=LED8, P3=LED7
    for (int i = 0; i < 3; i++) {
      int ledIdx = 9 - i; 

      if (ps[i]) { // Outflow pump is active
        if (am) {
          // Fades based on water level proximity to shutdown threshold
          // WL=3 -> Max brightness (Safest position); WL=1 -> Dimmest (Close to running dry)
          uint8_t bri = 255;
          if (wl == 2) bri = 170;
          else if (wl == 1) bri = 85;
          else if (wl == 0) bri = 0;
          leds[ledIdx] = CRGB(0, bri, 0);
        } else {
          leds[ledIdx] = CRGB::Green; 
        }
      } else { // Outflow pump is resting
        if (am && pea[i] && wl == 0) {
          // Tank is empty and pump is waiting for the pond pump to recharge.
          // Pulse cyan to indicate it is armed and waiting for capacity to recover.
          uint8_t breathe = scale8(sin8(now / 3), 180) + 55;
          leds[ledIdx] = CRGB(0, breathe, breathe / 4); 
        } else {
          leds[ledIdx] = CRGB(LED_COLOR_PUMP_OFF); 
        }
      }
    }

    FastLED.show();
  }
}

// ─── Serial console task (Core 0, Pri 1) ──────────────────────────────────────
static void Task_SerialConsole(void *) {
  char buf[CONSOLE_BUF_LEN];
  uint8_t n = 0;

  for (;;) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        if (!n) continue;
        buf[n] = '\0';
        n = 0;
        String cmd(buf);
        cmd.trim();
        Serial.println();

        if (cmd == "help") {
          Serial.println("Commands:");
          Serial.println("  status                  - Display real-time telemetry and state");
          Serial.println("  radio                   - Display RSSI, SNR, LoRa status");
          Serial.println("  config                  - Display active configuration");
          Serial.println("  stats                   - Display pump runtime stats and boot count");
          Serial.println("  tx                      - Enqueue manual LoRa status packet");
          Serial.println("  auto | manual           - Switch operation mode");
          Serial.println("  p1 | p2 | p3 | pond     - Pulse manual toggle buttons");
          Serial.println("  debug [on|off]          - Enable or disable WiFi AP & WebSocket dashboard");
          Serial.println("  web always [on|off]     - Set Web dashboard to always auto-start on boot");
          Serial.println("  set wifi <ssid> <pass>  - Configure Home Wi-Fi credentials (STA)");
          Serial.println("  set wifi clear          - Clear Home Wi-Fi credentials");
          Serial.println("  set runtime <ms>        - Set pump min runtime (1000 - 300000 ms)");
          Serial.println("  set cooldown <ms>       - Set pump min cooldown (5000 - 600000 ms)");
          Serial.println("  set telem <ms>          - Set telemetry broadcast interval (2000 - 300000 ms)");
          Serial.println("  set mask <p1> <p2> <p3> - Enable/disable auto pumps (e.g. set mask 1 1 0)");
          Serial.println("  set pond <0|1>          - Enable/disable pond pump");
          Serial.println("  set mode <auto|manual>  - Set operating mode and default boot mode");
          Serial.println("  set defaults            - Restore factory default configuration");
          Serial.println("  save                    - Persist configuration and stats to NVS flash");
          Serial.println("  reboot                  - Save state and restart ESP32-S3");
        } else if (cmd == "status") {
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.printf("wl=%u auto=%d P1=%d P2=%d P3=%d (Allowed:%d;%d;%d) pond=%d err=%u rssi=%d snr=%d radio=%d pcf=%d\n",
                          gState.waterLevel, gState.autoMode, gState.pumpState[0],
                          gState.pumpState[1], gState.pumpState[2],
                          gState.pumpEnabledAuto[0], gState.pumpEnabledAuto[1], gState.pumpEnabledAuto[2],
                          gState.pond, gState.errorCode, gState.lastRssi, gState.lastSnr, gState.radioOk, gState.pcfOk);
            xSemaphoreGive(xStateMutex);
          }
        } else if (cmd == "radio") {
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.printf("rssi=%d snr=%d ok=%d\n", gState.lastRssi, gState.lastSnr, gState.radioOk);
            xSemaphoreGive(xStateMutex);
          }
        } else if (cmd == "config") {
          Serial.printf("runtime=%lums cooldown=%lums telem=%lums auto=%u mask=%d;%d;%d pond=%d debug=%u always_on=%u sta='%s'\n",
                        gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms,
                        gConfig.telemetry_interval_ms, gConfig.boot_auto_mode,
                        gConfig.pump_enabled_auto[0], gConfig.pump_enabled_auto[1], gConfig.pump_enabled_auto[2],
                        gConfig.pond_enabled, gConfig.debug_mode_enabled, gConfig.web_always_on, gConfig.wifi_sta_ssid);
        } else if (cmd == "stats") {
          Serial.printf("P1=%lus P2=%lus P3=%lus boots=%lu\n", gStats.runtime_p1_s,
                        gStats.runtime_p2_s, gStats.runtime_p3_s, gStats.boot_count);
        } else if (cmd == "tx") {
          bool radioOk = false;
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            radioOk = gState.radioOk;
            xSemaphoreGive(xStateMutex);
          }
          if (!radioOk) {
            Serial.println("Radio not available");
          } else {
            LoRaPacket tx = {};
            buildStatus(tx);
            if (xQueueSend(xTxQueue, &tx, pdMS_TO_TICKS(100)) == pdTRUE) Serial.println("TX enqueued");
          }
        } else if (cmd == "auto") {
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            gState.autoMode = true;
            xSemaphoreGive(xStateMutex);
          }
          gConfig.boot_auto_mode = 1;
          saveConfig();
          if (xControlTask) xTaskNotifyGive(xControlTask);
          Serial.println("Mode: AUTO");
        } else if (cmd == "manual") {
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            gState.autoMode = false;
            xSemaphoreGive(xStateMutex);
          }
          gConfig.boot_auto_mode = 0;
          saveConfig();
          if (xControlTask) xTaskNotifyGive(xControlTask);
          Serial.println("Mode: MANUAL");
        } else if (cmd == "p1" || cmd == "p2" || cmd == "p3" || cmd == "pond") { 
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cmd == "pond") gState.btnPond_edge = true;
            else if (cmd == "p1") gState.btnP1_edge = true;
            else if (cmd == "p2") gState.btnP2_edge = true;
            else gState.btnP3_edge = true;
            xSemaphoreGive(xStateMutex);
          }
          if (xControlTask) xTaskNotifyGive(xControlTask);
          Serial.printf("Injected %s edge\n", cmd.c_str());
        } else if (cmd == "debug" || cmd == "debug on" || cmd == "debug 1") {
#if ENABLE_DEBUG_WEB
          gDebugServerWanted = true;
          gConfig.debug_mode_enabled = 1;
          saveConfig(true);
          Serial.println("Debug mode: ENABLED (AP: TankNode-Debug, IP: 192.168.4.1)");
#else
          Serial.println("Debug web interface disabled in build.");
#endif
        } else if (cmd == "debug off" || cmd == "debug 0") {
#if ENABLE_DEBUG_WEB
          gDebugServerWanted = false;
          gConfig.debug_mode_enabled = 0;
          saveConfig(true);
          Serial.println("Debug mode: DISABLED (WiFi powered OFF)");
#else
          Serial.println("Debug web interface disabled in build.");
#endif
        } else if (cmd == "web always on" || cmd == "web always 1" || cmd == "set web always 1") {
#if ENABLE_DEBUG_WEB
          gConfig.web_always_on = 1;
          gConfig.debug_mode_enabled = 1;
          gDebugServerWanted = true;
          saveConfig(true);
          Serial.println("Web dashboard: ALWAYS ON (Autostarts on boot).");
#else
          Serial.println("Debug web interface disabled in build.");
#endif
        } else if (cmd == "web always off" || cmd == "web always 0" || cmd == "set web always 0") {
#if ENABLE_DEBUG_WEB
          gConfig.web_always_on = 0;
          saveConfig(true);
          Serial.println("Web dashboard: ON-DEMAND only (Manual start via 'debug on').");
#else
          Serial.println("Debug web interface disabled in build.");
#endif
        } else if (cmd == "set wifi clear") {
          memset(gConfig.wifi_sta_ssid, 0, sizeof(gConfig.wifi_sta_ssid));
          memset(gConfig.wifi_sta_pass, 0, sizeof(gConfig.wifi_sta_pass));
          saveConfig(true);
          Serial.println("Home Wi-Fi credentials cleared. SoftAP mode only.");
        } else if (cmd.startsWith("set wifi ")) {
          char ssid[33] = {};
          char pass[65] = {};
          if (sscanf(cmd.c_str() + 9, "%32s %64s", ssid, pass) >= 1) {
            strncpy(gConfig.wifi_sta_ssid, ssid, sizeof(gConfig.wifi_sta_ssid) - 1);
            strncpy(gConfig.wifi_sta_pass, pass, sizeof(gConfig.wifi_sta_pass) - 1);
            saveConfig(true);
            Serial.printf("Saved Home Wi-Fi credentials: SSID='%s'\n", ssid);
            Serial.println("Reboot or restart debug web server to connect.");
          } else {
            Serial.println("Usage: set wifi <ssid> <pass> (or: set wifi clear)");
          }
        } else if (cmd.startsWith("set runtime ")) {
          uint32_t val = cmd.substring(12).toInt();
          if (val >= CFG_RUNTIME_MIN_MS && val <= CFG_RUNTIME_MAX_MS) {
            gConfig.pump_min_runtime_ms = val;
            saveConfig(true);
            Serial.printf("Config: pump_min_runtime_ms set to %lu ms\n", (unsigned long)val);
          } else {
            Serial.printf("Invalid runtime (%lu ms). Range: %lu - %lu ms\n", (unsigned long)val, CFG_RUNTIME_MIN_MS, CFG_RUNTIME_MAX_MS);
          }
        } else if (cmd.startsWith("set cooldown ")) {
          uint32_t val = cmd.substring(13).toInt();
          if (val >= CFG_COOLDOWN_MIN_MS && val <= CFG_COOLDOWN_MAX_MS) {
            gConfig.pump_min_cooldown_ms = val;
            saveConfig(true);
            Serial.printf("Config: pump_min_cooldown_ms set to %lu ms\n", (unsigned long)val);
          } else {
            Serial.printf("Invalid cooldown (%lu ms). Range: %lu - %lu ms\n", (unsigned long)val, CFG_COOLDOWN_MIN_MS, CFG_COOLDOWN_MAX_MS);
          }
        } else if (cmd.startsWith("set telem ")) {
          uint32_t val = cmd.substring(10).toInt();
          if (val >= CFG_TELEM_MIN_MS && val <= CFG_TELEM_MAX_MS) {
            gConfig.telemetry_interval_ms = val;
            saveConfig(true);
            Serial.printf("Config: telemetry_interval_ms set to %lu ms\n", (unsigned long)val);
          } else {
            Serial.printf("Invalid telemetry interval (%lu ms). Range: %lu - %lu ms\n", (unsigned long)val, CFG_TELEM_MIN_MS, CFG_TELEM_MAX_MS);
          }
        } else if (cmd.startsWith("set mask ")) {
          int p1 = -1, p2 = -1, p3 = -1;
          if (sscanf(cmd.c_str() + 9, "%d %d %d", &p1, &p2, &p3) == 3 &&
              (p1 == 0 || p1 == 1) && (p2 == 0 || p2 == 1) && (p3 == 0 || p3 == 1)) {
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              gState.pumpEnabledAuto[0] = (p1 == 1);
              gState.pumpEnabledAuto[1] = (p2 == 1);
              gState.pumpEnabledAuto[2] = (p3 == 1);
              gConfig.pump_enabled_auto[0] = gState.pumpEnabledAuto[0];
              gConfig.pump_enabled_auto[1] = gState.pumpEnabledAuto[1];
              gConfig.pump_enabled_auto[2] = gState.pumpEnabledAuto[2];
              gState.triggerImmediateTelem = true;
              xSemaphoreGive(xStateMutex);
              saveConfig(true);
            }
            if (xControlTask) xTaskNotifyGive(xControlTask);
            Serial.printf("Auto pump mask updated: P1=%d P2=%d P3=%d\n", p1, p2, p3);
          } else {
            Serial.println("Usage: set mask <0|1> <0|1> <0|1> (e.g. set mask 1 1 0)");
          }
        } else if (cmd.startsWith("set pond ")) {
          int pond = cmd.substring(9).toInt();
          if (pond == 0 || pond == 1) {
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              gState.pond = (pond == 1);
              gConfig.pond_enabled = gState.pond;
              gState.triggerImmediateTelem = true;
              xSemaphoreGive(xStateMutex);
              saveConfig(true);
            }
            if (xControlTask) xTaskNotifyGive(xControlTask);
            Serial.printf("Pond enabled: %d\n", pond);
          } else {
            Serial.println("Usage: set pond <0|1>");
          }
        } else if (cmd.startsWith("set mode ")) {
          String m = cmd.substring(9);
          m.toLowerCase();
          if (m == "auto" || m == "1") {
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              gState.autoMode = true;
              xSemaphoreGive(xStateMutex);
            }
            gConfig.boot_auto_mode = 1;
            saveConfig(true);
            if (xControlTask) xTaskNotifyGive(xControlTask);
            Serial.println("Mode: AUTO");
          } else if (m == "manual" || m == "man" || m == "0") {
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              gState.autoMode = false;
              xSemaphoreGive(xStateMutex);
            }
            gConfig.boot_auto_mode = 0;
            saveConfig(true);
            if (xControlTask) xTaskNotifyGive(xControlTask);
            Serial.println("Mode: MANUAL");
          } else {
            Serial.println("Usage: set mode <auto|manual>");
          }
        } else if (cmd == "psram") {
          Serial.printf("[PSRAM] Status: %s | Total: %lu KB | Free: %lu KB | History entries: %u / %u\n",
                        gState.psramOk ? "OK (ACTIVE)" : "NOT FOUND",
                        (unsigned long)(ESP.getPsramSize() / 1024),
                        (unsigned long)(ESP.getFreePsram() / 1024),
                        gHistoryCount, PSRAM_HISTORY_CAPACITY);
        } else if (cmd == "history") {
          Serial.printf("=== Telemetry History Log (%u entries) ===\n", gHistoryCount);
          for (uint16_t i = 0; i < gHistoryCount && i < 20; i++) {
            uint16_t idx = (gHistoryHead + i) % PSRAM_HISTORY_CAPACITY;
            Serial.printf("  #%u: T=%lus WL=%u Flags=0x%02X Temp=%.1f°C Hum=%u%%\n",
                          i + 1, (unsigned long)gHistory[idx].timestamp_s, gHistory[idx].waterLevel,
                          gHistory[idx].flags, gHistory[idx].temperature / 10.0f, gHistory[idx].humidity);
          }
        } else if (cmd == "set defaults" || cmd == "defaults") {
          resetConfigToDefaults();
          saveConfig(true);
          Serial.println("Config reset to defaults and saved to NVS.");
        } else if (cmd == "save") {
          saveConfig(true);
          saveStats();
          Serial.println("Config and statistics committed to NVS flash.");
        } else if (cmd == "reboot") {
          saveStats();
          saveConfig(true);
          Serial.println("Flushed NVS and restarting...");
          Serial.flush();
          esp_restart();
        } else {
          Serial.printf("Unknown command: %s\n", buf);
        }
      } else {
        if (n < CONSOLE_BUF_LEN - 1) buf[n++] = c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(SERIAL_POLL_MS));
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(BOOT_DELAY_MS);

  xStateMutex = xSemaphoreCreateMutex();
  xLogMutex = xSemaphoreCreateMutex();
  xRxQueue = xQueueCreate(RXQUEUE_DEPTH, sizeof(LoRaPacket));
  xTxQueue = xQueueCreate(TXQUEUE_DEPTH, sizeof(LoRaPacket));

  logEvent("SYS", 0, "Tank Node Booting (Firmware build v%u)...", CONFIG_VERSION);

  if (psramInit()) {
    gState.psramOk = true;
    gState.psramTotalBytes = ESP.getPsramSize();
    gState.psramFreeBytes = ESP.getFreePsram();
    Serial.printf("[PSRAM] Initialized: %lu KB total, %lu KB free\n",
                  (unsigned long)(gState.psramTotalBytes / 1024),
                  (unsigned long)(gState.psramFreeBytes / 1024));
    gHistory = (TelemetryHistoryEntry *)ps_malloc(sizeof(TelemetryHistoryEntry) * PSRAM_HISTORY_CAPACITY);
    if (gHistory) {
      memset(gHistory, 0, sizeof(TelemetryHistoryEntry) * PSRAM_HISTORY_CAPACITY);
      Serial.printf("[PSRAM] In-memory telemetry buffer allocated (%u entries, %u bytes)\n",
                    PSRAM_HISTORY_CAPACITY, (unsigned int)(sizeof(TelemetryHistoryEntry) * PSRAM_HISTORY_CAPACITY));
    }
  } else {
    Serial.println("[PSRAM] Not available or not detected in build");
  }

  loadConfig();
  loadStats();
  gStats.boot_count++;
  saveStats();
  Serial.printf("[Boot] #%lu autoMode=%u\n", gStats.boot_count, gConfig.boot_auto_mode);

  const int relays[] = {RELAY_P1, RELAY_P2, RELAY_P3};
  for (int pin : relays) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.beginTransmission(PCF8574_ADDR);
  gState.pcfOk = (Wire.endTransmission() == 0);
  Serial.printf("[PCF8574] %s\n", gState.pcfOk ? "found" : "not found — will retry");

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  SPI_2.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
  LoRa.setSPI(SPI_2, LORA_SPI_FREQ);
  bool radioOk = LoRa.begin(LORA_NSS, LORA_RESET, LORA_BUSY, LORA_IRQ, -1, -1);
  if (radioOk) {
    LoRa.setDio3TcxoCtrl(SX126X_DIO3_OUTPUT_1_8, SX126X_TCXO_DELAY_10);
    gState.radioOk = true;
    applyLoRaConfig();
    Serial.println("[LoRa] OK — listening");
  } else {
    gState.radioOk = false;
    Serial.println("[LoRa] INIT FAILED");
  }

  gState.autoMode = (bool)gConfig.boot_auto_mode;
  
  gState.pumpEnabledAuto[0] = gConfig.pump_enabled_auto[0];
  gState.pumpEnabledAuto[1] = gConfig.pump_enabled_auto[1];
  gState.pumpEnabledAuto[2] = gConfig.pump_enabled_auto[2];
  gState.pond = gConfig.pond_enabled;

  if (!gState.autoMode) {
    const int rPin[3] = {RELAY_P1, RELAY_P2, RELAY_P3};
    PumpHysteresis *phArr[3] = {&phP1, &phP2, &phP3};
    for (int i = 0; i < 3; i++) {
      if (gConfig.pump_manual_state[i]) {
        setPumpState(*phArr[i], true, rPin[i]);
        gState.pumpState[i] = true;
        Serial.printf("[Boot] Restored P%d ON (manual state)\n", i + 1);
      }
    }
  }

  xTaskCreatePinnedToCore(Task_LoRaManager, "LoRaMgr", STACK_LORA_MGR, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(Task_SerialConsole, "SerialCon", STACK_SERIAL_CON, NULL, 1, NULL, 0);
#if ENABLE_DEBUG_WEB
  gDebugServerWanted = (gConfig.debug_mode_enabled || gConfig.web_always_on);
  xTaskCreatePinnedToCore(Task_DebugWebServer, "DebugWeb", STACK_DEBUG_WEB, NULL, 1, NULL, 0);
  if (gDebugServerWanted) {
    Serial.println("[Boot] WebServer auto-started by config (Always Available).");
  }
#endif
  xTaskCreatePinnedToCore(Task_InputSensorPoll, "InputPoll", STACK_INPUT_POLL, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(Task_ControlEngine, "CtrlEng", STACK_CTRL_ENG, NULL, 4, &xControlTask, 1);
  xTaskCreatePinnedToCore(Task_UIAnimation, "LED_UI", STACK_LED_UI, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_ReadSensor, "ReadSensor", STACK_READ_SENSOR, NULL, 3, NULL, 1);
}

void loop() { vTaskDelete(NULL); }