#pragma once

// ─── Hardware — GPIO pins ─────────────────────────────────────────────────────

// LoRa SPI (FSPI bus)
#define LORA_SCK          18
#define LORA_MISO         13
#define LORA_MOSI         11
#define LORA_NSS           5
#define LORA_RESET        14
#define LORA_BUSY         17
#define LORA_IRQ          16

// Pump relays (active-LOW: LOW = pump ON)
#define RELAY_P1          47
#define RELAY_P2          41
#define RELAY_P3          39

// I²C bus — PCF8574 GPIO expander
#define I2C_SDA            1
#define I2C_SCL            2
#define PCF8574_ADDR    0x27

// WS2812B LED strip
#define LED_PIN           48
#define NUM_LEDS          10

// ─── Protocol — network identity ──────────────────────────────────────────────
#define MY_NETWORK_MAGIC  0x5A6B
#define NODE_GATEWAY      0x01
#define NODE_TANK         0x02
#define NODE_POND         0x03
#define NODE_BROADCAST    0xFF
#define MY_NODE_ID        NODE_TANK
#define CONFIG_VERSION    35    // Updated structural configuration version for parallel irrigation logic

// StatusPayload flags byte (TX)
#define STATUS_BIT_AUTO   0x01  // auto mode active
#define STATUS_BIT_P1     0x02  // pump 1 on
#define STATUS_BIT_P2     0x04  // pump 2 on
#define STATUS_BIT_P3     0x08  // pump 3 on
#define STATUS_BIT_POND   0x10  // pond virtual switch active

// CmdPayload target byte (RX) — which outputs to affect
#define CMD_TARGET_MODE   0x01
#define CMD_TARGET_P1     0x02
#define CMD_TARGET_P2     0x04
#define CMD_TARGET_P3     0x08
#define CMD_TARGET_POND   0x10  // pond virtual switch target

// CmdPayload action byte (RX)
#define CMD_ACTION_TOGGLE 0x00
#define CMD_ACTION_SET    0x01

// ─── LoRa RF parameters ───────────────────────────────────────────────────────
#define LORA_SPI_FREQ         16000000UL  // Hz
#define LORA_FREQUENCY       868100000UL  // Hz — 868.1 MHz (EU868 band)
#define LORA_TX_POWER_DBM            22
#define LORA_SF                       9
#define LORA_BW                  125000UL // Hz
#define LORA_CR                       5
#define LORA_SYNC_WORD           0x1424
#define LORA_PREAMBLE_LEN             8
#define LORA_MAX_PAYLOAD            255

// ─── Default runtime config ───────────────────────────────────────────────────
#define DEFAULT_PUMP_MIN_RUNTIME_MS    20000UL  
#define DEFAULT_PUMP_MIN_COOLDOWN_MS   20000UL  
#define DEFAULT_TELEMETRY_INTERVAL_MS  30000UL  // 30s heartbeat (immediate TX on state change)
#define DEFAULT_NETWORK_TIMEOUT_MS     60000UL  

// ─── Debounce & Filtration ────────────────────────────────────────────────────
#define FLOAT_DEBOUNCE_MS               1500UL  // Stable float reading duration to filter water sloshing
#define BTN_DEBOUNCE_MS                   50UL  // Push button debounce time
#define NVS_STATS_SAVE_INTERVAL_MS    600000UL  // Flush pump runtime stats every 10 min to protect flash

// ─── Error codes ──────────────────────────────────────────────────────────────
#define ERR_NONE               0
#define ERR_RADIO              1
#define ERR_PCF_I2C            2
#define ERR_GATEWAY_TIMEOUT    3
#define ERR_FLOAT_PLAUSIBILITY 4

// ─── Config field validation bounds ───────────────────────────────────────────
#define CFG_RUNTIME_MIN_MS       1000UL
#define CFG_RUNTIME_MAX_MS     300000UL  // 5 min maximum
#define CFG_COOLDOWN_MIN_MS      5000UL
#define CFG_COOLDOWN_MAX_MS    600000UL  // 10 min maximum
#define CFG_TELEM_MIN_MS         2000UL
#define CFG_TELEM_MAX_MS       300000UL

// ─── PCF8574 expander bit layout ──────────────────────────────────────────────
#define PCF_FLOAT_BITS         3
#define PCF_BTN_MASK        0xF8   
#define PCF_BTN_BIT_MODE       3
#define PCF_BTN_BIT_P1         4
#define PCF_BTN_BIT_P2         5
#define PCF_BTN_BIT_P3         6
#define PCF_BTN_POND_P4        7
#define PCF_RETRY_MS        5000UL

// ─── LED strip ────────────────────────────────────────────────────────────────
#define LED_BRIGHTNESS        80

// Flash durations
#define FLASH_RX_MS          200UL
#define FLASH_TX_MS          200UL

// Water level bar colors for LEDs 1–3
#define LED_COLOR_LVL1      0,  80, 255   
#define LED_COLOR_LVL2      0,  80, 255   
#define LED_COLOR_LVL3      0,  80, 255   
#define LED_COLOR_DIM       0,   0,   0   

// Status LED (LED 4) flash colors
#define LED_COLOR_RX_FLASH  128,   0, 128 
#define LED_COLOR_TX_FLASH    0, 255, 255 

// Pump indicator off-color (LEDs 7–9)
#define LED_COLOR_PUMP_OFF   30,   0,   0 // dim red

// ─── Serial console ───────────────────────────────────────────────────────────
#define SERIAL_BAUD       921600UL
#define BOOT_DELAY_MS       2000UL
#define CONSOLE_BUF_LEN       64

// ─── RTOS task periods ────────────────────────────────────────────────────────
#define LORA_POLL_MS          20UL   
#define INPUT_POLL_MS         20UL   
#define LED_REFRESH_MS        30UL   
#define SERIAL_POLL_MS        20UL   
#define CTRL_TICK_MS          50UL   
#define STATUS_PRINT_MS     1000UL   
#define SENSOR_READ_MS       1000UL   

// ─── RTOS task stack sizes (bytes) ────────────────────────────────────────────
#define STACK_LORA_MGR      4096
#define STACK_SERIAL_CON    4096
#define STACK_INPUT_POLL    2048
#define STACK_CTRL_ENG      6144
#define STACK_LED_UI        4096
#define STACK_READ_SENSOR   4096

// ─── RTOS queue depths ────────────────────────────────────────────────────────
#define RXQUEUE_DEPTH          8
#define TXQUEUE_DEPTH          4

// ─── NVS namespace and keys ───────────────────────────────────────────────────
#define NVS_NAMESPACE    "tanknode"
#define NVS_KEY_CFGVER   "cfgver"
#define NVS_KEY_CFG      "cfg"
#define NVS_KEY_RT1      "rt1"
#define NVS_KEY_RT2      "rt2"
#define NVS_KEY_RT3      "rt3"
#define NVS_KEY_BC       "bc"