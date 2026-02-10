/*********
  ESP32 MOTE Node - ESP-NOW + BLE + LED Status Feedback
  Version "lumiere test" avec 4 LEDs de statut:
    🔴 Rouge (IO25) : Échec d'envoi ESP-NOW
    🟡 Jaune (IO26) : Aucun appareil BLE détecté
    🟢 Vert  (IO27) : Envoi réussi au SINK
    🔵 Bleu  (IO13) : Appareil BLE "reseau1" détecté

  Configuration:
  - Sink MAC: ec:62:60:11:a2:3c
  - TODO: Change MOTE_ID below to your mote number (0-6)
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

// ============================================
// LED PINS - Status feedback
// ============================================
#define LED_ROUGE  25   // Échec d'envoi
#define LED_JAUNE  26   // Aucun BLE détecté
#define LED_VERT   27   // Envoi réussi
#define LED_BLEU   13   // BLE détecté

// ============================================
// ROUTING TABLE - Static configuration
// ============================================
#define MOTE_COUNT 7

// Sink MAC address
uint8_t sinkAddress[6] = {0xEC, 0x62, 0x60, 0x11, 0xA2, 0x3C};

// All motes MAC addresses
uint8_t moteAddress[MOTE_COUNT][6] = {
  {0xEC, 0x62, 0x60, 0x11, 0x97, 0xA0},   // Mote 0
  {0x24, 0xDC, 0xC3, 0x14, 0x37, 0x98},  // Mote 1
  {0xC4, 0xDE, 0xE2, 0xB1, 0x3E, 0xC8}, //Mote 2
  
  {0x08, 0xF9, 0xE0, 0x01, 0x0D, 0x00}, //Mote 3
  {0xEC, 0x62, 0x60, 0x5B, 0x35, 0x08}, //Mote 4
  {0x08, 0xF9, 0xE0, 0x00, 0xE2, 0x60}, //Mote 5

  {0x24, 0xDC, 0xC3, 0x14, 0x38, 0x24} //Mote 6

};

// Routing table: nextHop for each mote to reach SINK
typedef struct routing_entry {
  uint8_t destMAC[6];        // Destination (SINK)
  uint8_t nextHopMAC[6];     // Next hop to reach destination
  uint8_t hopCount;          // Distance to destination
} routing_entry;

// Full routing table: indexed by MOTE_ID
// Each entry defines how that mote reaches the SINK
routing_entry routingTable[MOTE_COUNT] = {
  // Mote 0: direct to SINK (hop 1)
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0xA2,0x3C}, 1 },
  // Mote 1: direct to SINK (hop 1)
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0xA2,0x3C}, 1 },
  // Mote 2: direct to SINK (hop 1)
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0xA2,0x3C}, 1 },
  // Mote 3: via Mote 0 (hop 2)
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0x97,0xA0}, 2 },
  // Mote 4: via Mote 0 (hop 2)
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xEC,0x62,0x60,0x11,0x97,0xA0}, 2 },
  // Mote 5: via Mote 1 (hop 2)
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0x24,0xDC,0xC3,0x14,0x37,0x98}, 2 },
  // Mote 6: via Mote 2 (hop 2)
  { {0xEC,0x62,0x60,0x11,0xA2,0x3C}, {0xC4,0xDE,0xE2,0xB1,0x3E,0xC8}, 2 },
};

// Shortcut: this mote's route
#define myRoute routingTable[MOTE_ID]

// ESP-NOW peer info
esp_now_peer_info_t sinkPeerInfo;
esp_now_peer_info_t motePeerInfo[MOTE_COUNT];

// ============================================
// DATA STRUCTURES FOR ESP-NOW
// ============================================
#define MSG_TYPE_DATA      0
#define MSG_TYPE_BLE_SCAN  1
#define MSG_TYPE_COMMAND   2
#define MSG_TYPE_HEARTBEAT 3

// Message structure: Mote -> Sink
typedef struct struct_mote2sinkMessage {
  uint8_t originMAC[6];
  uint8_t destMAC[6];
  uint8_t hopCount;
  uint8_t msgType;
  int boardId;
  int readingId;
  uint32_t timestamp;
  float data0;
  float data1;
  float data2;
  float data3;
  bool bool0;
  bool bool1;
  char text[200];
} struct_mote2sinkMessage;

// Message structure: Sink -> Mote
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
// LED CONTROL FUNCTIONS
// ============================================
void allLedsOff() {
  digitalWrite(LED_ROUGE, LOW);
  digitalWrite(LED_JAUNE, LOW);
  digitalWrite(LED_VERT, LOW);
  digitalWrite(LED_BLEU, LOW);
}

void setLedSendSuccess() {
  // Vert ON, Rouge OFF
  digitalWrite(LED_VERT, HIGH);
  digitalWrite(LED_ROUGE, LOW);
  Serial.println("   🟢 LED VERT: Envoi réussi");
}

void setLedSendFail() {
  // Rouge ON, Vert OFF
  digitalWrite(LED_ROUGE, HIGH);
  digitalWrite(LED_VERT, LOW);
  Serial.println("   🔴 LED ROUGE: Échec d'envoi");
}

void setLedBleFound() {
  // Bleu ON, Jaune OFF
  digitalWrite(LED_BLEU, HIGH);
  digitalWrite(LED_JAUNE, LOW);
  Serial.println("   🔵 LED BLEU: Appareil BLE détecté");
}

void setLedBleNotFound() {
  // Jaune ON, Bleu OFF
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

class BLEScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveName()) {
      String deviceName = advertisedDevice.getName().c_str();
      
      // Check if name starts with "reseau1"
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

void getMyMAC(uint8_t *mac) {
  esp_wifi_get_mac(WIFI_IF_STA, mac);
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

// ============================================
// ESP-NOW CALLBACKS
// ============================================
// Callback when data is received (from sink or other motes)
void onDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  Serial.printf("\n📥 Received from: %s, length: %d\n", 
                macToString(mac_addr).c_str(), len);
  
  // Check if it's a command from sink (smaller struct)
  if (len == sizeof(struct_sink2moteMessage)) {
    memcpy(&incomingCommand, incomingData, sizeof(incomingCommand));
    
    Serial.printf("   Command for Board ID: %d\n", incomingCommand.boardId);
    
    // Check if this command is for us
    if (incomingCommand.boardId == MOTE_ID) {
      Serial.println("   ✅ Command is for this mote!");
      
      if (incomingCommand.msgType == MSG_TYPE_COMMAND) {
        // LED control via MQTT command
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
      // Forward to another mote (if we're a relay)
      Serial.printf("   ↪️ Forwarding to Mote %d\n", incomingCommand.boardId);
    }
  } 
  // Check if it's a mote message to relay
  else if (len == sizeof(struct_mote2sinkMessage)) {
    struct_mote2sinkMessage relayMessage;
    memcpy(&relayMessage, incomingData, sizeof(relayMessage));
    
    // Check if destination is SINK and we need to relay
    if (memcmp(relayMessage.destMAC, sinkAddress, 6) == 0) {
      Serial.println("   ↪️ Relaying message to SINK");
      relayMessage.hopCount++;
      
      esp_err_t result = esp_now_send(myRoute.nextHopMAC, 
                                       (uint8_t *)&relayMessage, 
                                       sizeof(relayMessage));
      if (result == ESP_OK) {
        setLedSendSuccess();
      } else {
        setLedSendFail();
      }
      Serial.printf("   Relay result: %s\n", result == ESP_OK ? "OK" : "FAIL");
    }
  }
}

// Callback when data is sent (ESP32 Core 3.x)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("📤 Send status: %s\n", 
                status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
  
  // Update LEDs based on actual delivery status
  if (status == ESP_NOW_SEND_SUCCESS) {
    setLedSendSuccess();
  } else {
    setLedSendFail();
  }
}

// ============================================
// SEND FUNCTIONS
// ============================================
void sendToSink(uint8_t msgType, const char* textData = nullptr) {
  // Prepare message
  outgoingMessage = {};
  memcpy(outgoingMessage.originMAC, moteAddress[MOTE_ID], 6);
  memcpy(outgoingMessage.destMAC, sinkAddress, 6);
  outgoingMessage.hopCount = myRoute.hopCount;
  outgoingMessage.msgType = msgType;
  outgoingMessage.boardId = MOTE_ID;
  outgoingMessage.readingId = readingCounter++;
  outgoingMessage.timestamp = millis();
  
  if (textData != nullptr) {
    strncpy(outgoingMessage.text, textData, sizeof(outgoingMessage.text) - 1);
  }
  
  // Send via next hop
  esp_err_t result = esp_now_send(myRoute.nextHopMAC, 
                                   (uint8_t *)&outgoingMessage, 
                                   sizeof(outgoingMessage));
  
  if (result == ESP_OK) {
    setLedSendSuccess();
  } else {
    setLedSendFail();
  }
  
  Serial.printf("📤 Sending to SINK via %s - %s\n", 
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
  
  strncpy(outgoingMessage.text, deviceInfo.c_str(), sizeof(outgoingMessage.text) - 1);
  
  esp_err_t result = esp_now_send(myRoute.nextHopMAC, 
                                   (uint8_t *)&outgoingMessage, 
                                   sizeof(outgoingMessage));
  
  if (result == ESP_OK) {
    setLedSendSuccess();
  } else {
    setLedSendFail();
  }
  
  Serial.printf("📱 BLE data sent: %s - %s\n", 
                deviceInfo.c_str(),
                result == ESP_OK ? "OK" : "FAIL");
}

void sendHeartbeat() {
  sendToSink(MSG_TYPE_HEARTBEAT, "heartbeat");
}

// ============================================
// BLE SCAN FUNCTION
// ============================================
void performBLEScan() {
  Serial.println("\n=== Starting BLE Scan ===");
  Serial.printf("Filter: '%s*'\n", BLE_FILTER);
  Serial.printf("Duration: %d seconds\n", BLE_SCAN_TIME);
  
  filteredBLEDevices.clear();
  
  BLEScanResults* foundDevices = pBLEScan->start(BLE_SCAN_TIME, false);
  
  Serial.printf("\n=== BLE Scan Complete ===\n");
  Serial.printf("Total devices: %d\n", foundDevices->getCount());
  Serial.printf("Matching '%s': %d\n", BLE_FILTER, filteredBLEDevices.size());
  
  // Update BLE status LEDs
  if (filteredBLEDevices.size() > 0) {
    setLedBleFound();
  } else {
    setLedBleNotFound();
  }
  
  // Send each matching device to sink
  for (size_t i = 0; i < filteredBLEDevices.size(); i++) {
    sendBLEData(filteredBLEDevices[i]);
    delay(50);  // Small delay between sends
  }
  
  pBLEScan->clearResults();
  Serial.println("===========================\n");
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
  return 1;  // Default channel
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  ESP32 MOTE - LED Status Test          ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("MOTE ID: %d\n", MOTE_ID);
  
  // -------------------------------------------
  // LED setup
  // -------------------------------------------
  pinMode(LED_ROUGE, OUTPUT);
  pinMode(LED_JAUNE, OUTPUT);
  pinMode(LED_VERT, OUTPUT);
  pinMode(LED_BLEU, OUTPUT);
  allLedsOff();
  
  // Startup LED test: flash all LEDs sequentially
  Serial.println("🔦 LED test sequence...");
  digitalWrite(LED_ROUGE, HIGH); delay(300);
  digitalWrite(LED_ROUGE, LOW);
  digitalWrite(LED_JAUNE, HIGH); delay(300);
  digitalWrite(LED_JAUNE, LOW);
  digitalWrite(LED_VERT, HIGH); delay(300);
  digitalWrite(LED_VERT, LOW);
  digitalWrite(LED_BLEU, HIGH); delay(300);
  digitalWrite(LED_BLEU, LOW);
  Serial.println("✅ LED test complete");
  
  // -------------------------------------------
  // WiFi Configuration (STA mode for ESP-NOW)
  // -------------------------------------------
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  // Detect WiFi channel
  int32_t channel = getWiFiChannel(ssid);
  Serial.printf("📡 WiFi channel detected: %d\n", channel);
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  
  Serial.printf("📍 Mote MAC Address: %s\n", WiFi.macAddress().c_str());
  
  // -------------------------------------------
  // ESP-NOW Configuration
  // -------------------------------------------
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error initializing ESP-NOW");
    digitalWrite(LED_ROUGE, HIGH);  // Red = error
    ESP.restart();
  }
  Serial.println("✅ ESP-NOW initialized");
  
  // Register callbacks
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));
  esp_now_register_send_cb(onDataSent);
  
  // Register SINK as peer
  memset(&sinkPeerInfo, 0, sizeof(esp_now_peer_info_t));
  memcpy(sinkPeerInfo.peer_addr, sinkAddress, 6);
  sinkPeerInfo.channel = 0;
  sinkPeerInfo.encrypt = false;
  
  if (esp_now_add_peer(&sinkPeerInfo) != ESP_OK) {
    Serial.println("❌ Failed to add SINK peer");
  } else {
    Serial.printf("✅ SINK peer added: %s\n", macToString(sinkAddress).c_str());
  }
  
  // Register other motes as peers (for relay)
  for (int i = 0; i < MOTE_COUNT; i++) {
    if (i != MOTE_ID) {  // Don't add ourselves
      memset(&motePeerInfo[i], 0, sizeof(esp_now_peer_info_t));
      memcpy(motePeerInfo[i].peer_addr, moteAddress[i], 6);
      motePeerInfo[i].channel = 0;
      motePeerInfo[i].encrypt = false;
      
      if (esp_now_add_peer(&motePeerInfo[i]) == ESP_OK) {
        Serial.printf("✅ Mote %d peer added: %s\n", i, macToString(moteAddress[i]).c_str());
      }
    }
  }
  
  printRoutingTable();
  
  // -------------------------------------------
  // BLE Configuration
  // -------------------------------------------
  Serial.println("🔵 Initializing BLE Scanner...");
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLEScanCallback());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("✅ BLE Scanner ready");
  
  // -------------------------------------------
  // LED Legend
  // -------------------------------------------
  Serial.println("\n=== LED STATUS LEGEND ===");
  Serial.println("🔴 Rouge (IO25) : Échec d'envoi ESP-NOW");
  Serial.println("🟡 Jaune (IO26) : Aucun appareil BLE détecté");
  Serial.println("🟢 Vert  (IO27) : Envoi réussi au SINK");
  Serial.println("🔵 Bleu  (IO13) : Appareil BLE détecté");
  Serial.println("=========================\n");
  
  Serial.println("════════════════════════════════════════");
  Serial.println("✅ MOTE LED TEST ready!");
  Serial.printf("   BLE scan every %d seconds\n", BLE_SCAN_INTERVAL / 1000);
  Serial.printf("   Filter: devices starting with '%s'\n", BLE_FILTER);
  Serial.println("════════════════════════════════════════\n");
  
  // Initial scan
  delay(2000);
  performBLEScan();
}

// ============================================
// LOOP
// ============================================
unsigned long lastHeartbeat = 0;

void loop() {
  unsigned long now = millis();
  
  // Perform BLE scan periodically
  if (now - lastBLEScan >= BLE_SCAN_INTERVAL) {
    lastBLEScan = now;
    performBLEScan();
  }
  
  // Send heartbeat every 60 seconds
  if (now - lastHeartbeat >= 60000) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
  
  delay(100);
}
