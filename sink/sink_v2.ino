/*********
  ESP32 SINK Node v2 - Enhanced MQTT Publishing
  
  Receives ESP-NOW data from motes and publishes to MQTT with
  extended telemetry topics:
    esp32/<mac>/temperature   - Internal temp (°C)
    esp32/<mac>/freeHeap      - Free memory (KB)
    esp32/<mac>/cpuFreq       - CPU frequency (MHz)
    esp32/<mac>/wifiRssi      - WiFi signal (dBm)
    esp32/<mac>/uptime        - Uptime (ms)
    esp32/<mac>/path          - Message route path
    esp32/<mac>/msgCount      - Message counter
    esp32/<mac>/bleFound      - BLE device detected (0/1)
    esp32/<mac>/hops          - Hop count
    esp32/<mac>/ble/<name>/rssi - BLE RSSI per device

  Also publishes SINK own telemetry:
    esp32/sink/temperature, freeHeap, cpuFreq, wifiRssi, uptime, motesActive
    
  Configuration:
  - MQTT Broker: 192.168.1.21
  - WiFi: iot
*********/
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ============================================
// CONFIGURATION
// ============================================
#define MOTE_COUNT 7

const char *ssid = "iot";
const char *password = "iotisis;";
const char *mqtt_server = "192.168.1.21";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ============================================
// ROUTING TABLE
// ============================================
uint8_t moteAddress[MOTE_COUNT][6] = {
  {0xEC, 0x62, 0x60, 0x11, 0x97, 0xA0},  // Mote 0
  {0x24, 0xDC, 0xC3, 0x14, 0x37, 0x98},  // Mote 1
  {0xC4, 0xDE, 0xE2, 0xB1, 0x3E, 0xC8},  // Mote 2
  {0x08, 0xF9, 0xE0, 0x01, 0x0D, 0x00},  // Mote 3
  {0xEC, 0x62, 0x60, 0x5B, 0x35, 0x08},  // Mote 4
  {0x08, 0xF9, 0xE0, 0x00, 0xE2, 0x60},  // Mote 5
  {0x24, 0xDC, 0xC3, 0x14, 0x38, 0x24}   // Mote 6
};

esp_now_peer_info_t peerInfo[MOTE_COUNT];

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
  float data0;    // temperature
  float data1;    // freeHeap (KB)
  float data2;    // cpuFreq (MHz)
  float data3;    // wifiRssi (dBm)
  bool bool0;     // bleFound
  bool bool1;
  char text[200]; // path or BLE JSON
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

struct_mote2sinkMessage incomingMessage;
struct_mote2sinkMessage lastMotesReadings[MOTE_COUNT] = {};
struct_sink2moteMessage outgoingMessage;

// Track last time each mote was seen
unsigned long lastMoteContact[MOTE_COUNT] = {0};

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
    if (memcmp(mac, moteAddress[i], 6) == 0) return i;
  }
  return -1;
}

void printRoutingTable() {
  Serial.println("\n=== SINK ROUTING TABLE ===");
  for (int i = 0; i < MOTE_COUNT; i++) {
    Serial.printf("Mote %d: %s -> direct\n", i, macToString(moteAddress[i]).c_str());
  }
  Serial.println("===========================\n");
}

int countActiveMotes() {
  int count = 0;
  unsigned long now = millis();
  for (int i = 0; i < MOTE_COUNT; i++) {
    if (lastMoteContact[i] > 0 && (now - lastMoteContact[i]) < 120000) {
      count++;
    }
  }
  return count;
}

// ============================================
// ESP-NOW CALLBACKS
// ============================================
void onDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  Serial.printf("\n📥 Packet from: %s (%d bytes)\n", 
                macToString(mac_addr).c_str(), len);
  
  memcpy(&incomingMessage, incomingData, sizeof(incomingMessage));
  
  int moteId = getMoteIdFromMAC(incomingMessage.originMAC);
  Serial.printf("   Origin: %s (Mote %d)\n", 
                macToString(incomingMessage.originMAC).c_str(), moteId);
  Serial.printf("   Type: %d | Hops: %d | Reading: %d\n", 
                incomingMessage.msgType, incomingMessage.hopCount, incomingMessage.readingId);
  
  // Store reading and update contact time
  if (moteId >= 0 && moteId < MOTE_COUNT) {
    lastMotesReadings[moteId] = incomingMessage;
    lastMoteContact[moteId] = millis();
  }
  
  // Log message details
  switch (incomingMessage.msgType) {
    case MSG_TYPE_DATA:
      Serial.printf("   📊 Data - T:%.1f°C Heap:%.1fKB CPU:%.0fMHz RSSI:%.0fdBm\n",
                    incomingMessage.data0, incomingMessage.data1,
                    incomingMessage.data2, incomingMessage.data3);
      break;
    case MSG_TYPE_BLE_SCAN:
      Serial.printf("   📱 BLE: %s\n", incomingMessage.text);
      break;
    case MSG_TYPE_HEARTBEAT:
      Serial.printf("   💓 Heartbeat - T:%.1f°C Heap:%.1fKB\n",
                    incomingMessage.data0, incomingMessage.data1);
      break;
    case MSG_TYPE_TELEMETRY:
      Serial.printf("   📊 Telemetry - T:%.1f°C Heap:%.1fKB CPU:%.0fMHz RSSI:%.0fdBm\n",
                    incomingMessage.data0, incomingMessage.data1,
                    incomingMessage.data2, incomingMessage.data3);
      Serial.printf("   🛤️ Path: %s\n", incomingMessage.text);
      break;
  }
  
  publishToMQTT(moteId);
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("📤 ESP-NOW send: %s\n", 
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ============================================
// MQTT PUBLISHING
// ============================================
void publishToMQTT(int moteId) {
  if (!mqttClient.connected()) {
    Serial.println("⚠️ MQTT not connected");
    return;
  }
  if (moteId < 0 || moteId >= MOTE_COUNT) return;
  
  String macStr = macToString(incomingMessage.originMAC);
  String base = "esp32/" + macStr + "/";
  char buf[32];
  
  // === BLE SCAN DATA ===
  if (incomingMessage.msgType == MSG_TYPE_BLE_SCAN) {
    String jsonText = String(incomingMessage.text);
    
    int nameStart = jsonText.indexOf("\"name\":\"") + 8;
    int nameEnd = jsonText.indexOf("\"", nameStart);
    String deviceName = jsonText.substring(nameStart, nameEnd);
    
    int rssiStart = jsonText.indexOf("\"rssi\":") + 7;
    int rssiEnd = jsonText.indexOf("}", rssiStart);
    String rssiStr = jsonText.substring(rssiStart, rssiEnd);
    
    String bleTopic = base + "ble/" + deviceName + "/rssi";
    mqttClient.publish(bleTopic.c_str(), rssiStr.c_str());
    Serial.printf("📡 MQTT BLE: %s = %s\n", bleTopic.c_str(), rssiStr.c_str());
  }
  
  // === ALWAYS publish telemetry data (available in all v2 messages) ===
  
  // Temperature
  dtostrf(incomingMessage.data0, 1, 1, buf);
  mqttClient.publish((base + "temperature").c_str(), buf);
  
  // Free Heap (KB)
  dtostrf(incomingMessage.data1, 1, 1, buf);
  mqttClient.publish((base + "freeHeap").c_str(), buf);
  
  // CPU Frequency
  dtostrf(incomingMessage.data2, 1, 0, buf);
  mqttClient.publish((base + "cpuFreq").c_str(), buf);
  
  // WiFi RSSI
  dtostrf(incomingMessage.data3, 1, 0, buf);
  mqttClient.publish((base + "wifiRssi").c_str(), buf);
  
  // Uptime
  snprintf(buf, sizeof(buf), "%lu", incomingMessage.timestamp);
  mqttClient.publish((base + "uptime").c_str(), buf);
  
  // Hop count
  snprintf(buf, sizeof(buf), "%d", incomingMessage.hopCount);
  mqttClient.publish((base + "hops").c_str(), buf);
  
  // Message count
  snprintf(buf, sizeof(buf), "%d", incomingMessage.readingId);
  mqttClient.publish((base + "msgCount").c_str(), buf);
  
  // BLE found flag
  mqttClient.publish((base + "bleFound").c_str(), incomingMessage.bool0 ? "1" : "0");
  
  // Message path (only for non-BLE messages)
  if (incomingMessage.msgType != MSG_TYPE_BLE_SCAN && strlen(incomingMessage.text) > 0) {
    mqttClient.publish((base + "path").c_str(), incomingMessage.text);
  }
  
  // Board ID (for identification)
  snprintf(buf, sizeof(buf), "%d", incomingMessage.boardId);
  mqttClient.publish((base + "boardId").c_str(), buf);
  
  Serial.printf("📡 MQTT published all telemetry for mote %d\n", moteId);
}

// ============================================
// SINK OWN TELEMETRY
// ============================================
void publishSinkTelemetry() {
  if (!mqttClient.connected()) return;
  
  String base = "esp32/sink/";
  char buf[32];
  
  // Temperature
  dtostrf(temperatureRead(), 1, 1, buf);
  mqttClient.publish((base + "temperature").c_str(), buf);
  
  // Free Heap
  dtostrf((float)ESP.getFreeHeap() / 1024.0, 1, 1, buf);
  mqttClient.publish((base + "freeHeap").c_str(), buf);
  
  // CPU Freq
  snprintf(buf, sizeof(buf), "%d", getCpuFrequencyMhz());
  mqttClient.publish((base + "cpuFreq").c_str(), buf);
  
  // WiFi RSSI
  snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
  mqttClient.publish((base + "wifiRssi").c_str(), buf);
  
  // Uptime
  snprintf(buf, sizeof(buf), "%lu", millis());
  mqttClient.publish((base + "uptime").c_str(), buf);
  
  // Active motes count
  snprintf(buf, sizeof(buf), "%d", countActiveMotes());
  mqttClient.publish((base + "motesActive").c_str(), buf);
  
  Serial.printf("📡 SINK telemetry published (active motes: %d)\n", countActiveMotes());
}

// ============================================
// MQTT CALLBACK (commands from web)
// ============================================
void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  String topicStr = String(topic);
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.printf("\n📩 MQTT: %s = %s\n", topic, message.c_str());
  
  if (!topicStr.startsWith("esp32/") || !topicStr.endsWith("/output")) return;
  
  int firstSlash = topicStr.indexOf('/');
  int lastSlash = topicStr.lastIndexOf('/');
  String macStr = topicStr.substring(firstSlash + 1, lastSlash);
  
  int moteId = -1;
  for (int i = 0; i < MOTE_COUNT; i++) {
    if (macStr.equalsIgnoreCase(macToString(moteAddress[i]))) {
      moteId = i;
      break;
    }
  }
  
  if (moteId < 0) {
    Serial.println("   Unknown MAC");
    return;
  }
  
  outgoingMessage = {};
  memcpy(outgoingMessage.destMAC, moteAddress[moteId], 6);
  outgoingMessage.msgType = MSG_TYPE_COMMAND;
  outgoingMessage.boardId = moteId;
  
  if (message == "on") {
    outgoingMessage.bool0 = true;
  } else if (message == "off") {
    outgoingMessage.bool0 = false;
  } else {
    strncpy(outgoingMessage.text, message.c_str(), sizeof(outgoingMessage.text) - 1);
  }
  
  sendToMote(moteId);
}

void sendToMote(int moteId) {
  if (moteId < 0 || moteId >= MOTE_COUNT) return;
  esp_err_t result = esp_now_send(moteAddress[moteId], 
                                   (uint8_t *)&outgoingMessage, 
                                   sizeof(struct_sink2moteMessage));
  Serial.printf("%s Sent to Mote %d\n", result == ESP_OK ? "✅" : "❌", moteId);
}

void mqttReconnect() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 3) {
    Serial.print("🔄 MQTT connecting...");
    String clientId = "ESP32-SINK-V2-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("OK!");
      mqttClient.subscribe("esp32/+/output");
      Serial.println("📥 Subscribed: esp32/+/output");
    } else {
      Serial.printf("fail (rc=%d)\n", mqttClient.state());
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
  Serial.println("║  ESP32 SINK v2 - Enhanced Telemetry   ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  // WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  Serial.print("📶 WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.printf("\n✅ WiFi OK | IP: %s | CH: %d | MAC: %s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.channel(),
                WiFi.macAddress().c_str());
  
  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW failed");
    ESP.restart();
  }
  
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));
  esp_now_register_send_cb(onDataSent);
  
  for (int i = 0; i < MOTE_COUNT; i++) {
    memset(&peerInfo[i], 0, sizeof(esp_now_peer_info_t));
    memcpy(peerInfo[i].peer_addr, moteAddress[i], 6);
    peerInfo[i].channel = 0;
    peerInfo[i].encrypt = false;
    if (esp_now_add_peer(&peerInfo[i]) == ESP_OK) {
      Serial.printf("✅ Peer: Mote %d (%s)\n", i, macToString(moteAddress[i]).c_str());
    }
  }
  
  printRoutingTable();
  
  // MQTT
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  Serial.printf("📡 MQTT: %s:1883\n", mqtt_server);
  
  Serial.println("\n=== MQTT TOPICS (per mote) ===");
  Serial.println("  esp32/<mac>/temperature");
  Serial.println("  esp32/<mac>/freeHeap");
  Serial.println("  esp32/<mac>/cpuFreq");
  Serial.println("  esp32/<mac>/wifiRssi");
  Serial.println("  esp32/<mac>/uptime");
  Serial.println("  esp32/<mac>/path");
  Serial.println("  esp32/<mac>/hops");
  Serial.println("  esp32/<mac>/msgCount");
  Serial.println("  esp32/<mac>/bleFound");
  Serial.println("  esp32/<mac>/ble/<name>/rssi");
  Serial.println("  esp32/sink/* (sink telemetry)");
  Serial.println("==============================\n");
  
  Serial.println("✅ SINK v2 ready!\n");
}

// ============================================
// LOOP
// ============================================
unsigned long lastStatusPrint = 0;
unsigned long lastSinkTelemetry = 0;

void loop() {
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();
  
  // Publish sink own telemetry every 30s
  if (millis() - lastSinkTelemetry > 30000) {
    lastSinkTelemetry = millis();
    publishSinkTelemetry();
  }
  
  // Status print every 30s
  if (millis() - lastStatusPrint > 30000) {
    lastStatusPrint = millis();
    Serial.printf("\n📊 SINK - Uptime: %lus | MQTT: %s | Active motes: %d | Heap: %.1fKB\n",
                  millis() / 1000,
                  mqttClient.connected() ? "✅" : "❌",
                  countActiveMotes(),
                  (float)ESP.getFreeHeap() / 1024.0);
  }
}
