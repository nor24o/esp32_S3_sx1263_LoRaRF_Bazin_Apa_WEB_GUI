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

#include "config.h"

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
  bool demandActive = false;    // Mirrors tank charging cycle state
};

static LocalConfig gConfig;
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
  gConfig = {DEFAULT_PUMP_MIN_RUNTIME_MS, DEFAULT_PUMP_MIN_COOLDOWN_MS,
             DEFAULT_TELEMETRY_INTERVAL_MS, 0, {false, false, false}, false, {false, false, false}, false};
}

static bool validateConfig(const LocalConfig &c) {
  return c.pump_min_runtime_ms >= CFG_RUNTIME_MIN_MS &&
         c.pump_min_runtime_ms <= CFG_RUNTIME_MAX_MS &&
         c.pump_min_cooldown_ms >= CFG_COOLDOWN_MIN_MS &&
         c.pump_min_cooldown_ms <= CFG_COOLDOWN_MAX_MS &&
         c.telemetry_interval_ms >= CFG_TELEM_MIN_MS &&
         c.telemetry_interval_ms <= CFG_TELEM_MAX_MS && c.boot_auto_mode <= 1;
}

static void saveConfig() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar(NVS_KEY_CFGVER, CONFIG_VERSION);
  prefs.putBytes(NVS_KEY_CFG, &gConfig, sizeof(gConfig));
  prefs.end();
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
    saveConfig();
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

// ─── Input sensor task (Core 1, Pri 3) ────────────────────────────────────────
static void Task_InputSensorPoll(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint8_t prev = 0xFF;
  uint32_t retryAt = 0;

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
        xSemaphoreGive(xStateMutex);
      }
      retryAt = millis() + PCF_RETRY_MS;
      continue;
    }
    uint8_t curr = Wire.read();

    uint8_t lvl = 0;
    for (int i = 0; i < PCF_FLOAT_BITS; i++) {
      if (curr & (1 << i)) break;
      lvl++;
    }

    uint8_t edges = (~prev) & curr & PCF_BTN_MASK;
    prev = curr;

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      gState.waterLevel = lvl;
      gState.waterLevelValid = true;
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
      if ((now - lastGwMs) >= DEFAULT_NETWORK_TIMEOUT_MS) errorCode = 3;
      else if (errorCode == 3) errorCode = 0;
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
        xSemaphoreGive(xStateMutex);
      }
      Serial.printf("\n[Ctrl] Mode -> %s\n", autoMode ? "AUTO" : "MANUAL");
    }

    if (bPond) {
      if (!autoMode) { // Direct manual control over pond pump allowed only in Manual Mode
        gConfig.pond_enabled = !gConfig.pond_enabled;
        saveConfig();
        Serial.printf("\n[Ctrl] Manual Pond Request -> %s (Saved)\n", gConfig.pond_enabled ? "ON" : "OFF");
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
            xSemaphoreGive(xStateMutex);
          }
          Serial.printf("[Ctrl] Mode -> %s\n", autoMode ? "AUTO" : "MANUAL");
        }
        
        if (target & CMD_TARGET_POND) {
          if (!autoMode) {
            gConfig.pond_enabled = setVal ? (bool)value : !gConfig.pond_enabled;
            saveConfig();
            Serial.printf("[Ctrl] Remote Pond Switch Command -> %s\n", gConfig.pond_enabled ? "ON" : "OFF");
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
                xSemaphoreGive(xStateMutex);
                saveConfig(); 
                Serial.printf("[Ctrl] Auto P%d selection via LoRa -> %s (Saved)\n", i + 1, gConfig.pump_enabled_auto[i] ? "SELECTED" : "DESELECTED");
              }
            } else {
              setPumpState(*ph[i], want, relayPin[i]);
              gConfig.pump_manual_state[i] = want;
              saveConfig();
              Serial.printf("[Ctrl] Manual P%d via LoRa -> %s (saved)\n", i + 1, want ? "ON" : "OFF");
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
          xSemaphoreGive(xStateMutex);
          saveConfig(); 
          Serial.printf("\n[Ctrl] Auto P%d selection via Button -> %s (Saved)\n", i + 1, gConfig.pump_enabled_auto[i] ? "SELECTED" : "DESELECTED");
        }
      } else {
        bool want = !ph[i]->state;
        if (!want && ph[i]->state) {
          *pRtStats[i] += (millis() - pOnSince[i]) / 1000;
          saveStats();
        } else if (want) {
          pOnSince[i] = millis();
        }
        setPumpState(*ph[i], want, relayPin[i]);
        gConfig.pump_manual_state[i] = want;
        saveConfig();
        Serial.printf("\n[Ctrl] P%d -> %s (saved)\n", i + 1, want ? "ON" : "OFF");
      }
    }

    // ── FIXED: PARALLEL IRRIGATION AUTOMATIC CONTROL ENGINE ─────────────────────
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

      for (int i = 0; i < 3; i++) {
        bool targetState = waterAvailable && allowed[i];

        if (targetState != ph[i]->state) {
          if (!targetState && ph[i]->state) {
            *pRtStats[i] += (millis() - pOnSince[i]) / 1000;
            saveStats();
          } else if (targetState) {
            pOnSince[i] = millis();
          }
          setPumpState(*ph[i], targetState, relayPin[i]);
          Serial.printf("\n[Ctrl][Auto] Outflow Irrigation Pump P%d -> %s (WL=%u)\n", i + 1, targetState ? "ON" : "OFF", waterLevel);
        }
      }
    }

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      for (int i = 0; i < 3; i++) gState.pumpState[i] = ph[i]->state;
      gState.pond = gConfig.pond_enabled;
      gState.demandActive = systemDemandOn;
      xSemaphoreGive(xStateMutex);
    }

    if (radioOk && (now - lastTelem) >= gConfig.telemetry_interval_ms) {
      lastTelem = now;
      LoRaPacket tx = {};
      buildStatus(tx);
      xQueueSend(xTxQueue, &tx, 0);
    }

    // ── PERIODIC DISPATCH BROADCAST FOR THE REMOTE POND CHARGER ──────────
    if (radioOk && (now - lastPondBroadcast >= gConfig.telemetry_interval_ms)) {
      lastPondBroadcast = now;
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

    if (ec == 3) leds[4] = CRGB::Red;
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
          Serial.println("help | status | radio | config | stats | tx | auto | manual | p1 | p2 | p3 | pond | reboot");
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
          Serial.printf("runtime=%lums cooldown=%lums telem=%lums auto=%u mask=%d;%d;%d pond=%d\n",
                        gConfig.pump_min_runtime_ms, gConfig.pump_min_cooldown_ms,
                        gConfig.telemetry_interval_ms, gConfig.boot_auto_mode,
                        gConfig.pump_enabled_auto[0], gConfig.pump_enabled_auto[1], gConfig.pump_enabled_auto[2],
                        gConfig.pond_enabled);
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
        } else if (cmd == "reboot") {
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
  xTaskCreatePinnedToCore(Task_InputSensorPoll, "InputPoll", STACK_INPUT_POLL, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(Task_ControlEngine, "CtrlEng", STACK_CTRL_ENG, NULL, 4, &xControlTask, 1);
  xTaskCreatePinnedToCore(Task_UIAnimation, "LED_UI", STACK_LED_UI, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_ReadSensor, "ReadSensor", STACK_READ_SENSOR, NULL, 3, NULL, 1);
}

void loop() { vTaskDelete(NULL); }