/*
    ESP-NOW Master Receiver
    Le master reçoit les informations BLE envoyées par le slave
*/

#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>
#include <vector>

/* Definitions */
#define ESPNOW_WIFI_CHANNEL 6

// IMPORTANT: Remplacez par l'adresse MAC de votre SLAVE
uint8_t slaveAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // À MODIFIER !

/* Classes */
class ESP_NOW_Slave_Peer : public ESP_NOW_Peer {
public:
  ESP_NOW_Slave_Peer(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) 
    : ESP_NOW_Peer(mac_addr, channel, iface, lmk) {}

  ~ESP_NOW_Slave_Peer() {}

  bool add_peer() {
    if (!add()) {
      log_e("Failed to register the slave peer");
      return false;
    }
    return true;
  }

  void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    Serial.println("\n=== Message from Slave ===");
    Serial.printf("Slave MAC: " MACSTR "\n", MAC2STR(addr()));
    Serial.printf("Message length: %d bytes\n", len);
    Serial.printf("BLE Device Info: %s\n", (char *)data);
    Serial.println("========================\n");
  }
};

/* Global Variables */
ESP_NOW_Slave_Peer *slave = nullptr;
uint32_t messageCount = 0;

/* Main */
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=== ESP-NOW Master Receiver ===");

  // Initialize Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) {
    delay(100);
  }

  Serial.println("Wi-Fi parameters:");
  Serial.println("  Mode: STA");
  Serial.println("  MAC Address: " + WiFi.macAddress());
  Serial.printf("  Channel: %d\n", ESPNOW_WIFI_CHANNEL);

  // Initialize ESP-NOW
  if (!ESP_NOW.begin()) {
    Serial.println("Failed to initialize ESP-NOW");
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.printf("ESP-NOW version: %d, max data length: %d\n", ESP_NOW.getVersion(), ESP_NOW.getMaxDataLen());

  // Register slave peer
  slave = new ESP_NOW_Slave_Peer(slaveAddress, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);
  if (!slave->add_peer()) {
    Serial.println("Failed to register slave");
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.printf("Slave registered: " MACSTR "\n", MAC2STR(slaveAddress));
  Serial.println("Setup complete. Waiting for messages from slave...");
}

void loop() {
  // Afficher un message de statut toutes les 30 secondes
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 30000) {
    lastStatus = millis();
    Serial.printf("Master active - Messages received: %lu\n", messageCount);
  }

  delay(100);
}