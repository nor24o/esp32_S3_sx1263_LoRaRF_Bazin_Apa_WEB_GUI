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
};

static LocalConfig gConfig;
static LocalConfig gConfigSaved = {};
static bool gConfigSavedValid = false;
static NodeStats gStats;
static SystemState gState;

static SX126x LoRa;
static SPIClass SPI_2(FSPI);
static Preferences prefs;
static CRGB leds[NUM_LEDS];

static SemaphoreHandle_t xStateMutex;
static QueueHandle_t xRxQueue;
static QueueHandle_t xTxQueue;
static TaskHandle_t xControlTask = nullptr;

static PumpHysteresis phP1, phP2, phP3;

// ─── Config / Stats persistence ───────────────────────────────────────────────
static void resetConfigToDefaults() {
  memset(&gConfig, 0, sizeof(gConfig));
  gConfig.pump_min_runtime_ms = DEFAULT_PUMP_MIN_RUNTIME_MS;
  gConfig.pump_min_cooldown_ms = DEFAULT_PUMP_MIN_COOLDOWN_MS;
  gConfig.telemetry_interval_ms = DEFAULT_TELEMETRY_INTERVAL_MS;
  gConfig.boot_auto_mode = 0;
  gConfig.debug_mode_enabled = DEFAULT_WEB_ALWAYS_ON;
  gConfig.web_always_on = DEFAULT_WEB_ALWAYS_ON;
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
         c.web_always_on <= 1;
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

static void buildStatus(LoRaPacket &pkt) {
  pkt.header = {MY_NETWORK_MAGIC, NODE_GATEWAY, NODE_TANK, MSG_STATUS};
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    pkt.payload.status = {(uint8_t)((gState.autoMode ? STATUS_BIT_AUTO : 0) |
                                    (gState.pumpState[0] ? STATUS_BIT_P1 : 0) |
                                    (gState.pumpState[1] ? STATUS_BIT_P2 : 0) |
                                    (gState.pumpState[2] ? STATUS_BIT_P3 : 0) |
                                    (gState.pond ? STATUS_BIT_POND : 0)), 
                          gState.waterLevel, gState.temperature, gState.humidity, 0};
    xSemaphoreGive(xStateMutex);
  }
}

// ─── LoRa Manager Task (Core 0, Pri 2) ────────────────────────────────────────
static void Task_LoRaManager(void *) {
  for (;;) {
    LoRaPacket txPkt;
    if (xQueueReceive(xTxQueue, &txPkt, 0) == pdTRUE) {
      uint8_t payLen = (txPkt.header.msg_type == MSG_STATUS) ? (uint8_t)sizeof(StatusPayload)
                                                             : (uint8_t)sizeof(CmdPayload);
      sx126x_setBufferBaseAddress(0, 0);
      LoRa.setLoRaPacket(SX126X_HEADER_EXPLICIT, LORA_PREAMBLE_LEN, payLen, true, false);
      LoRa.beginPacket();
      LoRa.write((uint8_t *)&txPkt, sizeof(LoRaHeader) + payLen);
      LoRa.endPacket();
      LoRa.wait();
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        gState.txFlash = true;
        xSemaphoreGive(xStateMutex);
      }
      Serial.printf("\n[LoRa TX] type=%u target=0x%02X\n", txPkt.header.msg_type, txPkt.header.target_id);
      setForRx();
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
      
      if (rxPkt.header.magic == MY_NETWORK_MAGIC &&
          (rxPkt.header.target_id == NODE_TANK || rxPkt.header.target_id == NODE_BROADCAST)) {
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
static WebServer httpServer(DEBUG_HTTP_PORT);
static WebSocketsServer wsServer(DEBUG_WS_PORT);
static DNSServer dnsServer;
static bool gDebugServerRunning = false;
static bool gDebugServerWanted = false;

static void broadcastTelemetryWs() {
  if (!gDebugServerRunning) return;
  char buf[384];
  uint8_t wl = 0, ec = 0;
  bool am = false, pond = false, demand = false, pcfOk = false;
  bool ps[3] = {}, pea[3] = {};
  int16_t rssi = 0; int8_t snr = 0;
  
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    wl = gState.waterLevel;
    am = gState.autoMode;
    pond = gState.pond;
    demand = gState.demandActive;
    ec = gState.errorCode;
    pcfOk = gState.pcfOk;
    for (int i = 0; i < 3; i++) {
      ps[i] = gState.pumpState[i];
      pea[i] = gState.pumpEnabledAuto[i];
    }
    rssi = gState.lastRssi;
    snr = gState.lastSnr;
    xSemaphoreGive(xStateMutex);
  }

  const char *errStr = "NONE";
  if (ec == ERR_RADIO) errStr = "RADIO FAULT";
  else if (ec == ERR_PCF_I2C) errStr = "PCF8574 I2C FAULT";
  else if (ec == ERR_GATEWAY_TIMEOUT) errStr = "GATEWAY TIMEOUT";
  else if (ec == ERR_FLOAT_PLAUSIBILITY) errStr = "FLOAT PLAUSIBILITY FAULT";

  snprintf(buf, sizeof(buf),
    "{\"type\":\"telemetry\",\"wl\":%u,\"mode\":\"%s\",\"pumps\":[%d,%d,%d],"
    "\"auto_mask\":[%d,%d,%d],\"pond\":%d,\"demand\":%d,\"err\":%u,\"err_str\":\"%s\","
    "\"rssi\":%d,\"snr\":%d,\"heap\":%lu,\"uptime\":%lu,\"boots\":%lu,\"pcf_ok\":%d,"
    "\"sta\":%d,\"sta_ip\":\"%s\"}",
    wl, am ? "AUTO" : "MAN", ps[0], ps[1], ps[2],
    pea[0], pea[1], pea[2], pond, demand, ec, errStr,
    rssi, snr, (unsigned long)ESP.getFreeHeap(), (unsigned long)(millis() / 1000),
    (unsigned long)gStats.boot_count, pcfOk ? 1 : 0,
    WiFi.status() == WL_CONNECTED ? 1 : 0,
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : ""
  );
  wsServer.broadcastTXT(buf);
}

static void sendConfigWs(uint8_t num) {
  char buf[384];
  snprintf(buf, sizeof(buf),
    "{\"type\":\"config\",\"runtime\":%lu,\"cooldown\":%lu,\"telem\":%lu,"
    "\"boot_auto\":%u,\"debug\":%u,\"always_on\":%u,\"sta_ssid\":\"%s\","
    "\"ap_ip\":\"%s\",\"sta_ip\":\"%s\"}",
    (unsigned long)gConfig.pump_min_runtime_ms,
    (unsigned long)gConfig.pump_min_cooldown_ms,
    (unsigned long)gConfig.telemetry_interval_ms,
    gConfig.boot_auto_mode,
    gConfig.debug_mode_enabled,
    gConfig.web_always_on,
    gConfig.wifi_sta_ssid,
    WiFi.softAPIP().toString().c_str(),
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Disconnected"
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

static void startDebugWeb() {
  if (gDebugServerRunning) return;
  LittleFS.begin(true);

  if (strlen(gConfig.wifi_sta_ssid) > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(gConfig.wifi_sta_ssid, gConfig.wifi_sta_pass);
    Serial.printf("\n[DebugWeb] Connecting to Home Wi-Fi SSID '%s'...\n", gConfig.wifi_sta_ssid);
  } else {
    WiFi.mode(WIFI_AP);
  }

  WiFi.softAP(DEBUG_WIFI_SSID, DEBUG_WIFI_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[DebugWeb] mDNS active: http://%s.local\n", MDNS_HOSTNAME);
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

  httpServer.onNotFound([]() {
    httpServer.sendHeader("Location", "/", true);
    httpServer.send(302, "text/plain", "");
  });

  httpServer.begin();
  wsServer.begin();
  wsServer.onEvent(onWebSocketEvent);

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

    // ── AUTOMATIC TANK CHARGING STATE LATCH (Remote Pond Controller) ──
    if (waterLevelValid) {
      if (waterLevel == 0 && !systemDemandOn) {
        systemDemandOn = true;
        Serial.println("\n[Ctrl][Auto] Tank dry (WL=0) -> Pond Charging Latch START");
      } else if (waterLevel >= 3 && systemDemandOn) {
        systemDemandOn = false;
        Serial.println("\n[Ctrl][Auto] Tank full (WL=3) -> Pond Charging Latch STOP");
      }
    }

    // Save cycle latch state to NVS flash if it modified
    if (systemDemandOn != gConfig.last_demand_state) {
      gConfig.last_demand_state = systemDemandOn;
      saveConfig();
    }

    // In automatic mode, the pond output tracking state perfectly mirrors the charging latch
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
      if (!autoMode) { // Direct manual control over pond pump allowed only in Manual Mode
        gConfig.pond_enabled = !gConfig.pond_enabled;
        saveConfig();
        Serial.printf("\n[Ctrl] Manual Pond Request -> %s (Saved)\n", gConfig.pond_enabled ? "ON" : "OFF");
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          gState.triggerImmediateTelem = true;
          xSemaphoreGive(xStateMutex);
        }
      } else {
        Serial.println("\n[Ctrl] Pond toggle ignored. Handled automatically in AUTO mode.");
      }
    }

    LoRaPacket rx;
    while (xQueueReceive(xRxQueue, &rx, 0) == pdTRUE) {
      if (rx.header.sender_id != NODE_GATEWAY) continue;
      lastGwMs = now;
      gwSeen = true;

      if (rx.header.msg_type == MSG_CMD) {
        uint8_t target = rx.payload.cmd.target;
        uint8_t action = rx.payload.cmd.action;
        uint8_t value = rx.payload.cmd.value;
        bool setVal = (action == CMD_ACTION_SET);
        Serial.printf("\n[Ctrl] CMD target=0x%02X action=%u value=%u\n", target, action, value);

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
          if (!autoMode) {
            gConfig.pond_enabled = setVal ? (bool)value : !gConfig.pond_enabled;
            saveConfig();
            Serial.printf("[Ctrl] Remote Pond Switch Command -> %s\n", gConfig.pond_enabled ? "ON" : "OFF");
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
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
              if (want && !canChangePumpState(*ph[i], true, false, gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
                Serial.printf("[Ctrl] Manual P%d via LoRa rejected: cooldown active\n", i + 1);
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

    // ── FIXED: PARALLEL IRRIGATION AUTOMATIC CONTROL ENGINE WITH ANTI-CHATTER ───
    if (autoMode) {
      bool allowed[3] = {true, true, true};
      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        allowed[0] = gState.pumpEnabledAuto[0];
        allowed[1] = gState.pumpEnabledAuto[1];
        allowed[2] = gState.pumpEnabledAuto[2];
        xSemaphoreGive(xStateMutex);
      }

      // Outflow pumps are parallel: they run if water is available anywhere inside the tank (WL > 0)
      bool waterAvailable = (waterLevelValid && waterLevel > 0);
      bool emergencyDry = (waterLevelValid && waterLevel == 0);

      for (int i = 0; i < 3; i++) {
        bool targetState = waterAvailable && allowed[i];

        if (targetState != ph[i]->state) {
          if (canChangePumpState(*ph[i], targetState, emergencyDry,
                                 gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms)) {
            if (!targetState && ph[i]->state) {
              *pRtStats[i] += (millis() - pOnSince[i]) / 1000;
            } else if (targetState) {
              pOnSince[i] = millis();
            }
            setPumpState(*ph[i], targetState, relayPin[i]);
            Serial.printf("\n[Ctrl][Auto] Outflow Irrigation Pump P%d -> %s (WL=%u)\n", i + 1, targetState ? "ON" : "OFF", waterLevel);

            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              gState.triggerImmediateTelem = true;
              xSemaphoreGive(xStateMutex);
            }
          }
        }
      }
    }

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      for (int i = 0; i < 3; i++) gState.pumpState[i] = ph[i]->state;
      gState.pond = gConfig.pond_enabled;
      gState.demandActive = systemDemandOn;
      xSemaphoreGive(xStateMutex);
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
    static bool lastDispatchedPond = false;
    bool pondChanged = (gConfig.pond_enabled != lastDispatchedPond);
    if (radioOk && (pondChanged || (now - lastPondBroadcast >= gConfig.telemetry_interval_ms))) {
      lastPondBroadcast = now;
      lastDispatchedPond = gConfig.pond_enabled;
      LoRaPacket txPkt = {};
      txPkt.header = {MY_NETWORK_MAGIC, NODE_POND, NODE_TANK, MSG_CMD};
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
      leds[1 + i] = (i < (int)wl) ? lvlColor[i] : dimBase;
    }

    if (ec == ERR_GATEWAY_TIMEOUT) leds[4] = CRGB::Red;
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
    LoRa.setFrequency(LORA_FREQUENCY);
    LoRa.setTxPower(LORA_TX_POWER_DBM, SX126X_TX_POWER_SX1262);
    LoRa.setLoRaModulation(LORA_SF, LORA_BW, LORA_CR);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    setForRx();
    Serial.println("[LoRa] OK — listening");
  } else {
    Serial.println("[LoRa] INIT FAILED");
  }

  gState.radioOk = radioOk;
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

  xStateMutex = xSemaphoreCreateMutex();
  xRxQueue = xQueueCreate(RXQUEUE_DEPTH, sizeof(LoRaPacket));
  xTxQueue = xQueueCreate(TXQUEUE_DEPTH, sizeof(LoRaPacket));

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