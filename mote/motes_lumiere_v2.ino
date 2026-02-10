/*********
  ESP32 MOTE Node v2 - Enhanced Telemetry + LED Status
  
  Envoie des données système détaillées au SINK via ESP-NOW:
    🌡️ Température interne ESP32
    🧠 Mémoire libre (Free Heap)
    ⚡ Fréquence CPU (MHz)
    📶 WiFi RSSI (dBm)
    ⏱️ Uptime (ms depuis boot)
    🛤️ Chemin du message (ex: "mote3→mote0→sink")
    📊 Compteur de messages

  LEDs de statut:
    🔴 Rouge (IO25) : Échec d'envoi ESP-NOW
    🟡 Jaune (IO26) : Aucun appareil BLE détecté
    🟢 Vert  (IO27) : Envoi réussi au SINK
    🔵 Bleu  (IO13) : Appareil BLE "reseau1" détecté

  TODO: Change MOTE_ID below to your mote number (0-6)
*********/
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <vector>

// ============================================
// CONFIGURATION - MODIFY FOR EACH MOTE
// ============================================
#define MOTE_ID 0

// WiFi SSID (needed for channel detection)
const char *ssid = "iot";

// BLE Configuration
#define BLE_SCAN_TIME 5           // Scan duration in seconds
#define BLE_SCAN_INTERVAL 30000   // Scan every 30 seconds
const char* BLE_FILTER = "reseau1";  // BLE name prefix filter

// Telemetry interval
#define TELEMETRY_INTERVAL 30000  // Send system telemetry every 30 seconds

// ============================================
// LED PINS - Status feedback
// ============================================
#define LED_ROUGE  25   // Échec d'envoi
#define LED_JAUNE  26   // Aucun BLE détecté
#define LED_VERT   27   // Envoi réussi
#define LED_BLEU   13   // BLE détecté

// ============================================
// ROUTING TABLE
// ============================================
#define MOTE_COUNT 7

uint8_t sinkAddress[6] = {0xEC, 0x62, 0x60, 0x11, 0xA2, 0x3C};

uint8_t moteAddress[MOTE_COUNT][6] = {
  {0xEC, 0x62, 0x60, 0x11, 0x97, 0xA0},  // Mote 0
  {0x24, 0xDC, 0xC3, 0x14, 0x37, 0x98},  // Mote 1
  {0xC4, 0xDE, 0xE2, 0xB1, 0x3E, 0xC8},  // Mote 2
  {0x08, 0xF9, 0xE0, 0x01, 0x0D, 0x00},  // Mote 3
  {0xEC, 0x62, 0x60, 0x5B, 0x35, 0x08},  // Mote 4
  {0x08, 0xF9, 0xE0, 0x00, 0xE2, 0x60},  // Mote 5
  {0x24, 0xDC, 0xC3, 0x14, 0x38, 0x24}   // Mote 6
};

// Mote names for path building
const char* moteNames[] = {"mote0","mote1","mote2","mote3","mote4","mote5","mote6"};

typedef struct routing_entry {
  uint8_t destMAC[6];
  uint8_t nextHopMAC[6];
  uint8_t hopCount;
} routing_entry;

routing_entry routingTable[MOTE_COUNT] = {
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0xA2,0x3C}, 1 },  // Mote 0: direct
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0xA2,0x3C}, 1 },  // Mote 1: direct
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0xA2,0x3C}, 1 },  // Mote 2: direct
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0x97,0xA0}, 2 },  // Mote 3: via Mote 0
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0x97,0xA0}, 2 },  // Mote 4: via Mote 0
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0x24,0xDC,0xC3,0x14,0x37,0x98}, 2 },  // Mote 5: via Mote 1
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xC4,0xDE,0xE2,0xB1,0x3E,0xC8}, 2 },  // Mote 6: via Mote 2
};

#define myRoute routingTable[MOTE_ID]

// ESP-NOW peer info
esp_now_peer_info_t sinkPeerInfo;
esp_now_peer_info_t motePeerInfo[MOTE_COUNT];

// ============================================
// DATA STRUCTURES
// ============================================
#define MSG_TYPE_DATA      0
#define MSG_TYPE_BLE_SCAN  1
#define MSG_TYPE_COMMAND   2
#define MSG_TYPE_HEARTBEAT 3
#define MSG_TYPE_TELEMETRY 4

typedef struct struct_mote2sinkMessage {
  uint8_t originMAC[6];
  uint8_t destMAC[6];
  uint8_t hopCount;
  uint8_t msgType;
  int boardId;
  int readingId;
  uint32_t timestamp;
  float data0;    // temperature (°C)
  float data1;    // freeHeap (bytes)
  float data2;    // cpuFreq (MHz)
  float data3;    // WiFi RSSI (dBm)
  bool bool0;     // BLE found flag
  bool bool1;     // reserved
  char text[200]; // message path or BLE JSON
} struct_mote2sinkMessage;

typedef struct struct_sink2moteMessage {
  uint8_t destMAC[6];
  uint8_t msgType;
  int boardId;
  float data0;
  float data1;
  bool bool0;
  bool bool1;
  char text[64];
} struct_sink2moteMessage;

struct_mote2sinkMessage outgoingMessage;
struct_sink2moteMessage incomingCommand;

// ============================================
// SYSTEM TELEMETRY FUNCTIONS
// ============================================
float getESP32Temperature() {
  #ifdef __cplusplus
    return temperatureRead();  // Internal temperature sensor
  #else
    return 0.0;
  #endif
}

float getFreeHeapKB() {
  return (float)ESP.getFreeHeap() / 1024.0;  // Convert to KB
}

float getCPUFreq() {
  return (float)getCpuFrequencyMhz();
}

float getWiFiRSSI() {
  return (float)WiFi.RSSI();
}

// Build the message path string: "moteX→moteY→sink"
String buildMessagePath() {
  String path = String(moteNames[MOTE_ID]);
  
  // If multi-hop, find the next hop name
  if (myRoute.hopCount > 1) {
    // Find which mote is the next hop
    for (int i = 0; i < MOTE_COUNT; i++) {
      if (memcmp(myRoute.nextHopMAC, moteAddress[i], 6) == 0) {
        path += "→" + String(moteNames[i]);
        break;
      }
    }
  }
  path += "→sink";
  return path;
}

// Fill outgoing message with system telemetry
void fillTelemetryData(struct_mote2sinkMessage &msg) {
  msg.data0 = getESP32Temperature();    // Temperature °C
  msg.data1 = getFreeHeapKB();           // Free heap KB
  msg.data2 = getCPUFreq();             // CPU MHz
  msg.data3 = getWiFiRSSI();            // WiFi RSSI dBm
  msg.timestamp = millis();              // Uptime
}

// ============================================
// LED CONTROL
// ============================================
void allLedsOff() {
  digitalWrite(LED_ROUGE, LOW);
  digitalWrite(LED_JAUNE, LOW);
  digitalWrite(LED_VERT, LOW);
  digitalWrite(LED_BLEU, LOW);
}

void setLedSendSuccess() {
  digitalWrite(LED_VERT, HIGH);
  digitalWrite(LED_ROUGE, LOW);
  Serial.println("   🟢 LED VERT: Envoi réussi");
}

void setLedSendFail() {
  digitalWrite(LED_ROUGE, HIGH);
  digitalWrite(LED_VERT, LOW);
  Serial.println("   🔴 LED ROUGE: Échec d'envoi");
}

void setLedBleFound() {
  digitalWrite(LED_BLEU, HIGH);
  digitalWrite(LED_JAUNE, LOW);
  Serial.println("   🔵 LED BLEU: Appareil BLE détecté");
}

void setLedBleNotFound() {
  digitalWrite(LED_JAUNE, HIGH);
  digitalWrite(LED_BLEU, LOW);
  Serial.println("   🟡 LED JAUNE: Aucun appareil BLE détecté");
}

// ============================================
// BLE SCANNER
// ============================================
BLEScan* pBLEScan;
std::vector<String> filteredBLEDevices;
unsigned long lastBLEScan = 0;
int readingCounter = 0;
int bleFoundCount = 0;  // Track BLE devices found in last scan

class BLEScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveName()) {
      String deviceName = advertisedDevice.getName().c_str();
      if (deviceName.startsWith(BLE_FILTER)) {
        String deviceInfo = "{\"name\":\"" + deviceName + "\",";
        deviceInfo += "\"mac\":\"" + String(advertisedDevice.getAddress().toString().c_str()) + "\",";
        deviceInfo += "\"rssi\":" + String(advertisedDevice.getRSSI()) + "}";
        filteredBLEDevices.push_back(deviceInfo);
        Serial.printf("   📱 BLE MATCH: %s [RSSI: %d]\n", 
                      deviceName.c_str(), advertisedDevice.getRSSI());
      }
    }
  }
};

// ============================================
// UTILITY FUNCTIONS
// ============================================
String macToString(const uint8_t *mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

void printRoutingTable() {
  Serial.println("\n=== FULL ROUTING TABLE ===");
  Serial.printf("My ID: Mote %d | My MAC: %s\n", MOTE_ID, macToString(moteAddress[MOTE_ID]).c_str());
  Serial.println("---------------------------");
  for (int i = 0; i < MOTE_COUNT; i++) {
    Serial.printf("Mote %d -> nextHop: %s (hops: %d)%s\n",
      i,
      macToString(routingTable[i].nextHopMAC).c_str(),
      routingTable[i].hopCount,
      (i == MOTE_ID) ? " << ME" : "");
  }
  Serial.println("===========================\n");
}

void printTelemetry() {
  Serial.println("\n--- System Telemetry ---");
  Serial.printf("🌡️ Temperature: %.1f °C\n", getESP32Temperature());
  Serial.printf("🧠 Free Heap: %.1f KB\n", getFreeHeapKB());
  Serial.printf("⚡ CPU Freq: %.0f MHz\n", getCPUFreq());
  Serial.printf("📶 WiFi RSSI: %.0f dBm\n", getWiFiRSSI());
  Serial.printf("⏱️ Uptime: %lu s\n", millis() / 1000);
  Serial.printf("🛤️ Path: %s\n", buildMessagePath().c_str());
  Serial.printf("📊 Messages sent: %d\n", readingCounter);
  Serial.printf("📱 BLE found last scan: %d\n", bleFoundCount);
  Serial.println("------------------------\n");
}

// ============================================
// ESP-NOW CALLBACKS
// ============================================
void onDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  Serial.printf("\n📥 Received from: %s, length: %d\n", 
                macToString(mac_addr).c_str(), len);
  
  if (len == sizeof(struct_sink2moteMessage)) {
    memcpy(&incomingCommand, incomingData, sizeof(incomingCommand));
    Serial.printf("   Command for Board ID: %d\n", incomingCommand.boardId);
    
    if (incomingCommand.boardId == MOTE_ID) {
      Serial.println("   ✅ Command is for this mote!");
      if (incomingCommand.msgType == MSG_TYPE_COMMAND) {
        if (incomingCommand.bool0) {
          Serial.println("   💡 Command: ON");
        } else {
          Serial.println("   💡 Command: OFF");
        }
        if (strlen(incomingCommand.text) > 0) {
          Serial.printf("   📝 Text: %s\n", incomingCommand.text);
        }
      }
    } else {
      Serial.printf("   ↪️ Forwarding to Mote %d\n", incomingCommand.boardId);
    }
  } 
  else if (len == sizeof(struct_mote2sinkMessage)) {
    struct_mote2sinkMessage relayMessage;
    memcpy(&relayMessage, incomingData, sizeof(relayMessage));
    
    if (memcmp(relayMessage.destMAC, sinkAddress, 6) == 0) {
      Serial.println("   ↪️ Relaying message to SINK");
      relayMessage.hopCount++;
      
      // Append our name to the path
      String currentPath = String(relayMessage.text);
      // Only modify path for telemetry/data, not BLE scan
      if (relayMessage.msgType != MSG_TYPE_BLE_SCAN) {
        // Insert our name before →sink
        int sinkPos = currentPath.indexOf("→sink");
        if (sinkPos > 0) {
          String newPath = currentPath.substring(0, sinkPos) + "→" + String(moteNames[MOTE_ID]) + "→sink";
          strncpy(relayMessage.text, newPath.c_str(), sizeof(relayMessage.text) - 1);
        }
      }
      
      esp_err_t result = esp_now_send(myRoute.nextHopMAC, 
                                       (uint8_t *)&relayMessage, 
                                       sizeof(relayMessage));
      if (result == ESP_OK) setLedSendSuccess();
      else setLedSendFail();
    }
  }
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("📤 Send status: %s\n", 
                status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
  if (status == ESP_NOW_SEND_SUCCESS) setLedSendSuccess();
  else setLedSendFail();
}

// ============================================
// SEND FUNCTIONS
// ============================================
void sendToSink(uint8_t msgType, const char* textData = nullptr) {
  outgoingMessage = {};
  memcpy(outgoingMessage.originMAC, moteAddress[MOTE_ID], 6);
  memcpy(outgoingMessage.destMAC, sinkAddress, 6);
  outgoingMessage.hopCount = myRoute.hopCount;
  outgoingMessage.msgType = msgType;
  outgoingMessage.boardId = MOTE_ID;
  outgoingMessage.readingId = readingCounter++;
  
  // Fill system telemetry
  fillTelemetryData(outgoingMessage);
  outgoingMessage.bool0 = (bleFoundCount > 0);
  
  // Set message path or custom text
  if (textData != nullptr && msgType == MSG_TYPE_BLE_SCAN) {
    strncpy(outgoingMessage.text, textData, sizeof(outgoingMessage.text) - 1);
  } else {
    String path = buildMessagePath();
    strncpy(outgoingMessage.text, path.c_str(), sizeof(outgoingMessage.text) - 1);
  }
  
  esp_err_t result = esp_now_send(myRoute.nextHopMAC, 
                                   (uint8_t *)&outgoingMessage, 
                                   sizeof(outgoingMessage));
  
  if (result == ESP_OK) setLedSendSuccess();
  else setLedSendFail();
  
  Serial.printf("📤 Sending [type=%d] to SINK via %s - %s\n", 
                msgType,
                macToString(myRoute.nextHopMAC).c_str(),
                result == ESP_OK ? "OK" : "FAIL");
}

void sendBLEData(String& deviceInfo) {
  outgoingMessage = {};
  memcpy(outgoingMessage.originMAC, moteAddress[MOTE_ID], 6);
  memcpy(outgoingMessage.destMAC, sinkAddress, 6);
  outgoingMessage.hopCount = myRoute.hopCount;
  outgoingMessage.msgType = MSG_TYPE_BLE_SCAN;
  outgoingMessage.boardId = MOTE_ID;
  outgoingMessage.readingId = readingCounter++;
  outgoingMessage.timestamp = millis();
  
  // Still include telemetry in BLE messages
  outgoingMessage.data0 = getESP32Temperature();
  outgoingMessage.data1 = getFreeHeapKB();
  outgoingMessage.data2 = getCPUFreq();
  outgoingMessage.data3 = getWiFiRSSI();
  outgoingMessage.bool0 = true;  // BLE found
  
  strncpy(outgoingMessage.text, deviceInfo.c_str(), sizeof(outgoingMessage.text) - 1);
  
  esp_err_t result = esp_now_send(myRoute.nextHopMAC, 
                                   (uint8_t *)&outgoingMessage, 
                                   sizeof(outgoingMessage));
  
  if (result == ESP_OK) setLedSendSuccess();
  else setLedSendFail();
  
  Serial.printf("📱 BLE data sent - %s\n", result == ESP_OK ? "OK" : "FAIL");
}

void sendTelemetry() {
  Serial.println("\n📊 Sending telemetry...");
  printTelemetry();
  sendToSink(MSG_TYPE_TELEMETRY);
}

void sendHeartbeat() {
  sendToSink(MSG_TYPE_HEARTBEAT);
}

// ============================================
// BLE SCAN
// ============================================
void performBLEScan() {
  Serial.println("\n=== Starting BLE Scan ===");
  filteredBLEDevices.clear();
  
  BLEScanResults* foundDevices = pBLEScan->start(BLE_SCAN_TIME, false);
  
  bleFoundCount = filteredBLEDevices.size();
  Serial.printf("Total: %d | Matching '%s': %d\n", 
                foundDevices->getCount(), BLE_FILTER, bleFoundCount);
  
  if (bleFoundCount > 0) {
    setLedBleFound();
  } else {
    setLedBleNotFound();
  }
  
  for (size_t i = 0; i < filteredBLEDevices.size(); i++) {
    sendBLEData(filteredBLEDevices[i]);
    delay(50);
  }
  
  pBLEScan->clearResults();
  Serial.println("=== BLE Scan Complete ===\n");
}

// ============================================
// WiFi CHANNEL DETECTION
// ============================================
int32_t getWiFiChannel(const char *ssid) {
  int32_t n = WiFi.scanNetworks();
  for (int32_t i = 0; i < n; i++) {
    if (String(ssid) == WiFi.SSID(i)) {
      return WiFi.channel(i);
    }
  }
  return 1;
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  ESP32 MOTE v2 - Enhanced Telemetry   ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("MOTE ID: %d\n", MOTE_ID);
  
  // LED setup
  pinMode(LED_ROUGE, OUTPUT);
  pinMode(LED_JAUNE, OUTPUT);
  pinMode(LED_VERT, OUTPUT);
  pinMode(LED_BLEU, OUTPUT);
  allLedsOff();
  
  // LED test sequence
  Serial.println("🔦 LED test...");
  int leds[] = {LED_ROUGE, LED_JAUNE, LED_VERT, LED_BLEU};
  for (int i = 0; i < 4; i++) {
    digitalWrite(leds[i], HIGH); delay(200);
    digitalWrite(leds[i], LOW);
  }
  
  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  int32_t channel = getWiFiChannel(ssid);
  Serial.printf("📡 WiFi channel: %d\n", channel);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  Serial.printf("📍 MAC: %s\n", WiFi.macAddress().c_str());
  
  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    digitalWrite(LED_ROUGE, HIGH);
    ESP.restart();
  }
  
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));
  esp_now_register_send_cb(onDataSent);
  
  // Add SINK peer
  memset(&sinkPeerInfo, 0, sizeof(esp_now_peer_info_t));
  memcpy(sinkPeerInfo.peer_addr, sinkAddress, 6);
  sinkPeerInfo.channel = 0;
  sinkPeerInfo.encrypt = false;
  esp_now_add_peer(&sinkPeerInfo);
  
  // Add mote peers
  for (int i = 0; i < MOTE_COUNT; i++) {
    if (i != MOTE_ID) {
      memset(&motePeerInfo[i], 0, sizeof(esp_now_peer_info_t));
      memcpy(motePeerInfo[i].peer_addr, moteAddress[i], 6);
      motePeerInfo[i].channel = 0;
      motePeerInfo[i].encrypt = false;
      esp_now_add_peer(&motePeerInfo[i]);
    }
  }
  
  printRoutingTable();
  
  // BLE
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLEScanCallback());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  
  Serial.println("\n=== LED STATUS ===");
  Serial.println("🔴 Rouge (IO25) : Échec d'envoi");
  Serial.println("🟡 Jaune (IO26) : Aucun BLE");
  Serial.println("🟢 Vert  (IO27) : Envoi OK");
  Serial.println("🔵 Bleu  (IO13) : BLE trouvé");
  Serial.println("==================\n");
  
  // Initial telemetry
  printTelemetry();
  
  Serial.println("✅ MOTE v2 ready!\n");
  delay(2000);
  performBLEScan();
  sendTelemetry();
}

// ============================================
// LOOP
// ============================================
unsigned long lastHeartbeat = 0;
unsigned long lastTelemetry = 0;

void loop() {
  unsigned long now = millis();
  
  // BLE scan
  if (now - lastBLEScan >= BLE_SCAN_INTERVAL) {
    lastBLEScan = now;
    performBLEScan();
  }
  
  // Telemetry every 30s
  if (now - lastTelemetry >= TELEMETRY_INTERVAL) {
    lastTelemetry = now;
    sendTelemetry();
  }
  
  // Heartbeat every 60s
  if (now - lastHeartbeat >= 60000) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
  
  delay(100);
}
