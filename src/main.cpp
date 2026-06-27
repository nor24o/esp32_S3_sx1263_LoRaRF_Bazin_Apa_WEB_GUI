#include <SX126x.h>
#include <SPI.h>

#define LORA_MOSI  11
#define LORA_MISO  13
#define LORA_SCK   18

// --- FIXED NETWORK CONFIGURATION ---
const uint8_t MY_NODE_ID = 0x01;       
const uint8_t TARGET_NODE_ID = 0x02;   
const uint8_t BROADCAST_ID = 0xFF;     

SX126x LoRa;
SPIClass SPI_2(FSPI);

char message[] = "HeLoRa World!";
uint8_t nBytes = sizeof(message) - 1; 
uint8_t counter = 0;

SemaphoreHandle_t loraMutex;

// Forward Declarations
void setForRxLora();
void readMessage();
void sendMessage();
void TaskLoRaRX(void *pvParameters);
void TaskLoRaTX(void *pvParameters);

void setup() {
  // Bumping baud rate to 921600 also helps drain the buffer faster
  Serial.begin(921600);
  delay(3000);

  SPI_2.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
  LoRa.setSPI(SPI_2, 16000000);

  Serial.println("Begin LoRa radio");
  int8_t nssPin = 5, resetPin = 14, busyPin = 17, irqPin = 16, txenPin = -1, rxenPin = -1;
  if (!LoRa.begin(nssPin, resetPin, busyPin, irqPin, txenPin, rxenPin)) {
    Serial.println("Something wrong, can't begin LoRa radio");
    while(1) {
      delay(1000); 
    }
  }

  Serial.println("Set RF module to use TCXO as clock reference");
  LoRa.setDio3TcxoCtrl(SX126X_DIO3_OUTPUT_1_8, SX126X_TCXO_DELAY_10);

  Serial.println("Set frequency to 868.1 Mhz");
  LoRa.setFrequency(868100000);

  Serial.println("Set TX power to +17 dBm");
  LoRa.setTxPower(22, SX126X_TX_POWER_SX1262);

  Serial.println("Set modulation parameters");
  LoRa.setLoRaModulation(9, 125000, 5);
  
  Serial.println("Set syncronize word to 0x1424");
  LoRa.setSyncWord(0x1424);

  Serial.printf("\n-- LORA NODE ID: 0x%X --\n\n", MY_NODE_ID);

  setForRxLora();

  loraMutex = xSemaphoreCreateMutex();

  if (loraMutex != NULL) {
    xTaskCreate(TaskLoRaRX, "LoRa_RX", 4096, NULL, 2, NULL);
    xTaskCreate(TaskLoRaTX, "LoRa_TX", 4096, NULL, 1, NULL);
  }
}

void loop() {
  vTaskDelete(NULL); 
}

// -------------------------------------------------------------
// RTOS TASKS
// -------------------------------------------------------------

void TaskLoRaRX(void *pvParameters) {
  for (;;) {
    if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      readMessage();
      xSemaphoreGive(loraMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(40)); 
  }
}

void TaskLoRaTX(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    if (xSemaphoreTake(loraMutex, portMAX_DELAY) == pdTRUE) {
      sendMessage();
      setForRxLora(); 
      xSemaphoreGive(loraMutex);
    }
  }
}

// -------------------------------------------------------------
// LORA FUNCTIONS
// -------------------------------------------------------------

void sendMessage() {
  // TX Mode: STANDARD IQ (false)
  LoRa.setLoRaPacket(SX126X_HEADER_EXPLICIT, 8, 15, true, false);

  LoRa.beginPacket();
  LoRa.write(TARGET_NODE_ID); 
  LoRa.write(MY_NODE_ID);     
  LoRa.write(counter);        
  LoRa.write((uint8_t*)message, nBytes); 
  LoRa.endPacket();

  // Atomic print prevents chopping
  Serial.printf("Sent message %d to 0x%X\n", counter++, TARGET_NODE_ID);

  LoRa.wait();
}

void setForRxLora() {
  // RX Mode: INVERTED IQ (true)
  LoRa.setLoRaPacket(SX126X_HEADER_EXPLICIT, 8, 15, true, true);
  LoRa.setRxGain(SX126X_RX_GAIN_BOOSTED);
  LoRa.request(SX126X_RX_CONTINUOUS);
}

void readMessage() {
  const uint8_t msgLen = LoRa.available();
  
  if (msgLen >= 3) {
    uint8_t rxTargetID = LoRa.read();
    uint8_t rxSenderID = LoRa.read();
    uint8_t rxCounter  = LoRa.read();
    
    // 1. Self-Rejection
    if (rxSenderID == MY_NODE_ID) {
      while (LoRa.available()) LoRa.read();
      return; 
    }

    // 2. Addressed Filter
    if (rxTargetID != MY_NODE_ID && rxTargetID != BROADCAST_ID) {
      // MUTED TO PREVENT SERIAL BUFFER OVERFLOW FROM RF NOISE
      // Serial.printf("-> Ignored packet meant for node 0x%X\n", rxTargetID);
      while (LoRa.available()) LoRa.read();
      return;
    }

    // 3. Valid Packet Found - Parse Payload
    int payloadLen = msgLen - 3;
    char rxMessage[payloadLen + 1]; 
    uint8_t i = 0;
    
    while (LoRa.available() > 0) {
      rxMessage[i++] = LoRa.read();
    }
    rxMessage[i] = '\0'; 

    // 4. Atomic Block Print
    Serial.printf("-----------------------------------\n");
    Serial.printf("Received from: 0x%X\n", rxSenderID);
    
    if (rxTargetID == BROADCAST_ID) {
      Serial.printf("Targeted to:   BROADCAST\n");
    } else {
      Serial.printf("Targeted to:   0x%X\n", rxTargetID);
    }

    Serial.printf("Msg [%d]:     %s\n", rxCounter, rxMessage);
    Serial.printf("Signal:        RSSI = %d dBm | SNR = %d dB\n", LoRa.packetRssi(), LoRa.snr());
    Serial.printf("-----------------------------------\n\n");
  }
  
  uint8_t status = LoRa.status();
  if (status == SX126X_STATUS_CRC_ERR) Serial.println("CRC error");
  else if (status == SX126X_STATUS_HEADER_ERR) Serial.println("Packet header error");
}