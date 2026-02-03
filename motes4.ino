/*********
  ESP32 MOTE Node - ESP-NOW with Static Routing + BLE Scan
  Scans for BLE devices with "reseau1" prefix and sends to SINK
  
  Configuration:
  - Sink MAC: ec:62:60:5b:35:08
  - This Mote: Change MOTE_ID to match your device (0, 1, or 2)
  - Mote 0 MAC: 24:dc:c3:14:37:98
  - Mote 1 MAC: 08:f9:e0:00:e2:60
  - Mote 2 MAC: ec:62:60:11:a2:3c
  
  TODO: Change MOTE_ID below to your mote number (0, 1, or 2)
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
// TODO: SET THIS TO YOUR MOTE NUMBER (0, 1, or 2)
#define MOTE_ID 4

// WiFi SSID (needed for channel detection)
const char *ssid = "iot";

// BLE Configuration
#define BLE_SCAN_TIME 5           // Scan duration in seconds
#define BLE_SCAN_INTERVAL 30000   // Scan every 30 seconds
const char* BLE_FILTER = "reseau1";  // BLE name prefix filter

// LED pin for visual feedback
const int ledPin = 2;  // Built-in LED on most ESP32

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
// Configure based on your network topology:
// - Direct: nextHop = sinkAddress
// - Via another mote: nextHop = that mote's address
typedef struct routing_entry {
  uint8_t destMAC[6];        // Destination (SINK)
  uint8_t nextHopMAC[6];     // Next hop to reach destination
  uint8_t hopCount;          // Distance to destination
} routing_entry;

// Default routing: all motes send directly to sink
// TODO: Modify nextHop if you need multi-hop routing
routing_entry myRoute = {
  .destMAC = {0xEC, 0x62, 0x60, 0x11, 0xA2, 0x3C},     // SINK
  .nextHopMAC = {0xEC, 0x62, 0x60, 0x11, 0x97, 0xA0},  // Mote 0   // Direct to SINK
  .hopCount = 1
};

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
  Serial.println("\n=== MOTE ROUTING TABLE ===");
  Serial.printf("My ID: Mote %d\n", MOTE_ID);
  Serial.printf("My MAC: %s\n", macToString(moteAddress[MOTE_ID]).c_str());
  Serial.printf("Destination: SINK (%s)\n", macToString(myRoute.destMAC).c_str());
  Serial.printf("Next Hop: %s\n", macToString(myRoute.nextHopMAC).c_str());
  Serial.printf("Hop Count: %d\n", myRoute.hopCount);
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
        if (incomingCommand.bool0) {
          digitalWrite(ledPin, HIGH);
          Serial.println("   💡 LED ON");
        } else {
          digitalWrite(ledPin, LOW);
          Serial.println("   💡 LED OFF");
        }
        
        if (strlen(incomingCommand.text) > 0) {
          Serial.printf("   📝 Text: %s\n", incomingCommand.text);
        }
      }
    } else {
      // Forward to another mote (if we're a relay)
      Serial.printf("   ↪️ Forwarding to Mote %d\n", incomingCommand.boardId);
      // TODO: Implement forwarding if needed
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
      Serial.printf("   Relay result: %s\n", result == ESP_OK ? "OK" : "FAIL");
    }
  }
}

// Callback when data is sent (ESP32 Core 3.x)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("📤 Send status: %s\n", 
                status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
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
  Serial.println("║   ESP32 MOTE Node - ESP-NOW + BLE      ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("MOTE ID: %d\n", MOTE_ID);
  
  // LED setup
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  
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
  
  Serial.println("\n════════════════════════════════════════");
  Serial.println("✅ MOTE ready!");
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