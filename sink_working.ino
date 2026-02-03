/*********
  ESP32 SINK Node - ESP-NOW + MQTT
  Receives data from motes via ESP-NOW routing and publishes to MQTT
  
  Configuration:
  - Sink MAC: ec:62:60:5b:35:08
  - Mote 0 MAC: 24:dc:c3:14:37:98
  - Mote 1 MAC: 08:f9:e0:00:e2:60
  - Mote 2 MAC: ec:62:60:11:a2:3c
  - MQTT Broker: 192.168.1.21
*********/
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ============================================
// CONFIGURATION - À MODIFIER SELON VOTRE RÉSEAU
// ============================================
#define MOTE_COUNT 7

// WiFi credentials
const char *ssid = "iot";
const char *password = "iotisis;";

// MQTT Broker
const char *mqtt_server = "172.18.32.43";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ============================================
// ROUTING TABLE - Static configuration
// ============================================
// Mote MAC addresses (index = moteId)
uint8_t moteAddress[MOTE_COUNT][6] = {
  {0xEC, 0x62, 0x60, 0x11, 0x97, 0xA0},   // Mote 0
  {0x24, 0xDC, 0xC3, 0x14, 0x37, 0x98},  // Mote 1
  {0xC4, 0xDE, 0xE2, 0xB1, 0x3E, 0xC8}, //Mote 2
  
  {0x08, 0xF9, 0xE0, 0x01, 0x0D, 0x00}, //Mote 3
  {0xEC, 0x62, 0x60, 0x5B, 0x35, 0x08}, //Mote 4
  {0x08, 0xF9, 0xE0, 0x00, 0xE2, 0x60}, //Mote 5

  {0x24, 0xDC, 0xC3, 0x14, 0x38, 0x24} //Mote 6

};

esp_now_peer_info_t peerInfo[MOTE_COUNT];

// ============================================
// DATA STRUCTURES FOR ESP-NOW
// ============================================
// Message types
#define MSG_TYPE_DATA      0
#define MSG_TYPE_BLE_SCAN  1
#define MSG_TYPE_COMMAND   2
#define MSG_TYPE_HEARTBEAT 3

// Message structure: Mote -> Sink
typedef struct struct_mote2sinkMessage {
  uint8_t originMAC[6];      // Original sender MAC
  uint8_t destMAC[6];        // Destination MAC (sink)
  uint8_t hopCount;          // Number of hops
  uint8_t msgType;           // Message type
  int boardId;               // Board/mote ID
  int readingId;             // Reading sequence number
  uint32_t timestamp;        // Timestamp (millis)
  float data0;               // Sensor data fields
  float data1;
  float data2;
  float data3;
  bool bool0;
  bool bool1;
  char text[200];            // Text data (JSON for BLE, etc.)
} struct_mote2sinkMessage;

// Message structure: Sink -> Mote
typedef struct struct_sink2moteMessage {
  uint8_t destMAC[6];        // Target mote MAC
  uint8_t msgType;           // Message type
  int boardId;               // Target board ID
  float data0;
  float data1;
  bool bool0;
  bool bool1;
  char text[64];
} struct_sink2moteMessage;

// Storage for incoming messages
struct_mote2sinkMessage incomingMessage;
struct_mote2sinkMessage lastMotesReadings[MOTE_COUNT] = {};

// Outgoing message to motes
struct_sink2moteMessage outgoingMessage;

// ============================================
// UTILITY FUNCTIONS
// ============================================
String macToString(const uint8_t *mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

int getMoteIdFromMAC(const uint8_t *mac) {
  for (int i = 0; i < MOTE_COUNT; i++) {
    if (memcmp(mac, moteAddress[i], 6) == 0) {
      return i;
    }
  }
  return -1;  // Not found
}

void printRoutingTable() {
  Serial.println("\n=== SINK ROUTING TABLE ===");
  for (int i = 0; i < MOTE_COUNT; i++) {
    Serial.printf("Mote %d: %s -> direct\n", i, macToString(moteAddress[i]).c_str());
  }
  Serial.println("===========================\n");
}

// ============================================
// ESP-NOW CALLBACKS
// ============================================
// Callback when data is received from motes
void onDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  Serial.printf("\n📥 Packet received from: %s, length: %d bytes\n", 
                macToString(mac_addr).c_str(), len);
  
  memcpy(&incomingMessage, incomingData, sizeof(incomingMessage));
  
  int moteId = getMoteIdFromMAC(incomingMessage.originMAC);
  Serial.printf("   Origin MAC: %s (Mote ID: %d)\n", 
                macToString(incomingMessage.originMAC).c_str(), moteId);
  Serial.printf("   Hop count: %d\n", incomingMessage.hopCount);
  Serial.printf("   Message type: %d\n", incomingMessage.msgType);
  Serial.printf("   Board ID: %d\n", incomingMessage.boardId);
  Serial.printf("   Reading ID: %d\n", incomingMessage.readingId);
  Serial.printf("   Timestamp: %lu\n", incomingMessage.timestamp);
  
  // Store last reading from this mote
  if (moteId >= 0 && moteId < MOTE_COUNT) {
    lastMotesReadings[moteId] = incomingMessage;
  }
  
  // Handle different message types
  switch (incomingMessage.msgType) {
    case MSG_TYPE_DATA:
      Serial.printf("   📊 Sensor data - data0: %.2f, data1: %.2f\n", 
                    incomingMessage.data0, incomingMessage.data1);
      break;
      
    case MSG_TYPE_BLE_SCAN:
      Serial.printf("   📱 BLE Device: %s\n", incomingMessage.text);
      break;
      
    case MSG_TYPE_HEARTBEAT:
      Serial.println("   💓 Heartbeat received");
      break;
      
    default:
      Serial.printf("   📄 Text: %s\n", incomingMessage.text);
  }
  
  // Publish to MQTT immediately
  publishToMQTT(moteId);
}

// Callback when data is sent to motes (ESP32 Core 3.x)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("📤 ESP-NOW send status: %s\n", 
                status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

// ============================================
// MQTT FUNCTIONS
// ============================================
void publishToMQTT(int moteId) {
  if (!mqttClient.connected()) {
    Serial.println("⚠️  MQTT not connected, skipping publish");
    return;
  }
  
  if (moteId < 0 || moteId >= MOTE_COUNT) {
    Serial.println("⚠️  Invalid mote ID");
    return;
  }
  
  String macStr = macToString(incomingMessage.originMAC);
  String baseTopic = "esp32/" + macStr + "/";
  
  char buffer[16];
  
  // Publish based on message type
if (incomingMessage.msgType == MSG_TYPE_BLE_SCAN) {
    // **NOUVELLE LOGIQUE** - Parser le JSON pour extraire deviceName et RSSI
    String jsonText = String(incomingMessage.text);

    // Méthode simple sans bibliothèque JSON
    int nameStart = jsonText.indexOf("\"name\":\"") + 8;
    int nameEnd = jsonText.indexOf("\"", nameStart);
    String deviceName = jsonText.substring(nameStart, nameEnd);

    int rssiStart = jsonText.indexOf("\"rssi\":") + 7;
    int rssiEnd = jsonText.indexOf("}", rssiStart);
    String rssiStr = jsonText.substring(rssiStart, rssiEnd);

    // Créer un topic dynamique avec deviceName
    String dynamicTopic = baseTopic + "ble/" + deviceName + "/rssi";

    // Publier la valeur RSSI comme payload (en tant que String)
    mqttClient.publish(dynamicTopic.c_str(), rssiStr.c_str());
    Serial.printf("📡 MQTT published BLE to: %s with RSSI: %s\n", 
              dynamicTopic.c_str(), rssiStr.c_str());} 
          else {
    // Sensor data
    dtostrf(incomingMessage.data0, 1, 2, buffer);
    mqttClient.publish((baseTopic + "data0").c_str(), buffer);
    
    dtostrf(incomingMessage.data1, 1, 2, buffer);
    mqttClient.publish((baseTopic + "data1").c_str(), buffer);
    
    snprintf(buffer, sizeof(buffer), "%lu", incomingMessage.timestamp);
    mqttClient.publish((baseTopic + "timestamp").c_str(), buffer);
    
    snprintf(buffer, sizeof(buffer), "%d", incomingMessage.hopCount);
    mqttClient.publish((baseTopic + "hops").c_str(), buffer);
    
    Serial.printf("📡 MQTT published sensor data to: %s\n", baseTopic.c_str());
  }
}

// MQTT callback for subscribed topics
void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  String topicStr = String(topic);
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.printf("\n📩 MQTT received - Topic: %s, Message: %s\n", topic, message.c_str());
  
  // Expected format: esp32/<mac>/output
  if (!topicStr.startsWith("esp32/") || !topicStr.endsWith("/output")) {
    Serial.println("   Topic format not recognized, ignoring");
    return;
  }
  
  // Extract MAC from topic
  int firstSlash = topicStr.indexOf('/');
  int lastSlash = topicStr.lastIndexOf('/');
  String macStr = topicStr.substring(firstSlash + 1, lastSlash);
  
  // Find the mote ID
  int moteId = -1;
  for (int i = 0; i < MOTE_COUNT; i++) {
    if (macStr.equalsIgnoreCase(macToString(moteAddress[i]))) {
      moteId = i;
      break;
    }
  }
  
  if (moteId < 0) {
    Serial.println("   Unknown MAC address in topic");
    return;
  }
  
  // Prepare and send command to mote
  outgoingMessage = {};
  memcpy(outgoingMessage.destMAC, moteAddress[moteId], 6);
  outgoingMessage.msgType = MSG_TYPE_COMMAND;
  outgoingMessage.boardId = moteId;
  
  if (message == "on") {
    outgoingMessage.bool0 = true;
    Serial.printf("   Sending ON command to Mote %d\n", moteId);
  } else if (message == "off") {
    outgoingMessage.bool0 = false;
    Serial.printf("   Sending OFF command to Mote %d\n", moteId);
  } else {
    strncpy(outgoingMessage.text, message.c_str(), sizeof(outgoingMessage.text) - 1);
    Serial.printf("   Sending text command to Mote %d: %s\n", moteId, message.c_str());
  }
  
  sendToMote(moteId);
}

void sendToMote(int moteId) {
  if (moteId < 0 || moteId >= MOTE_COUNT) return;
  
  esp_err_t result = esp_now_send(moteAddress[moteId], 
                                   (uint8_t *)&outgoingMessage, 
                                   sizeof(struct_sink2moteMessage));
  
  if (result == ESP_OK) {
    Serial.printf("✅ Message sent to Mote %d (%s)\n", 
                  moteId, macToString(moteAddress[moteId]).c_str());
  } else {
    Serial.printf("❌ Failed to send to Mote %d\n", moteId);
  }
}

void mqttReconnect() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 3) {
    Serial.print("🔄 Attempting MQTT connection...");
    
    String clientId = "ESP32-SINK-" + String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected!");
      // Subscribe to all mote output topics
      mqttClient.subscribe("esp32/+/output");
      Serial.println("📥 Subscribed to: esp32/+/output");
    } else {
      Serial.printf("failed, rc=%d, retry in 2s\n", mqttClient.state());
      delay(2000);
      attempts++;
    }
  }
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║     ESP32 SINK Node - ESP-NOW + MQTT   ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  // -------------------------------------------
  // WiFi Configuration
  // -------------------------------------------
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  
  Serial.print("📶 Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  Serial.printf("✅ WiFi connected!\n");
  Serial.printf("   IP Address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("   WiFi Channel: %d\n", WiFi.channel());
  Serial.printf("   Sink MAC Address: %s\n", WiFi.macAddress().c_str());
  
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
  
  // Register all motes as peers
  for (int i = 0; i < MOTE_COUNT; i++) {
    memset(&peerInfo[i], 0, sizeof(esp_now_peer_info_t));
    memcpy(peerInfo[i].peer_addr, moteAddress[i], 6);
    peerInfo[i].channel = 0;  // Use current channel
    peerInfo[i].encrypt = false;
    
    if (esp_now_add_peer(&peerInfo[i]) != ESP_OK) {
      Serial.printf("❌ Failed to add peer Mote %d\n", i);
    } else {
      Serial.printf("✅ Peer added: Mote %d (%s)\n", i, macToString(moteAddress[i]).c_str());
    }
  }
  
  printRoutingTable();
  
  // -------------------------------------------
  // MQTT Configuration
  // -------------------------------------------
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);  // Increase buffer for BLE data
  
  Serial.printf("📡 MQTT Broker: %s:1883\n", mqtt_server);
  
  Serial.println("\n════════════════════════════════════════");
  Serial.println("✅ SINK ready! Waiting for mote messages...");
  Serial.println("════════════════════════════════════════\n");
}

// ============================================
// LOOP
// ============================================
unsigned long lastStatusPrint = 0;

void loop() {
  // MQTT connection management
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();
  
  // Print status every 30 seconds
  if (millis() - lastStatusPrint > 30000) {
    lastStatusPrint = millis();
    Serial.printf("\n📊 Status - Uptime: %lu s, MQTT: %s, Motes: %d\n",
                  millis() / 1000,
                  mqttClient.connected() ? "connected" : "disconnected",
                  MOTE_COUNT);
  }
}