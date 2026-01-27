/*
    ESP-NOW Slave with BLE Scanner + Filter
    Le slave scanne les appareils BLE, filtre ceux contenant "reseau_1"
    et envoie uniquement ceux-là au master
*/

#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

/* Definitions */
#define ESPNOW_WIFI_CHANNEL 6

// IMPORTANT: Remplacez par l'adresse MAC de votre MASTER
uint8_t masterAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // À MODIFIER !

// Filtre : mot-clé à rechercher dans les appareils BLE
const char* FILTER_KEYWORD = "reseau_1";

/* BLE Variables */
int scanTime = 10; // en secondes
BLEScan* pBLEScan;

/* Classes */
class ESP_NOW_Master_Peer : public ESP_NOW_Peer {
public:
  ESP_NOW_Master_Peer(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) 
    : ESP_NOW_Peer(mac_addr, channel, iface, lmk) {}

  ~ESP_NOW_Master_Peer() {}

  bool add_peer() {
    if (!add()) {
      log_e("Failed to register the master peer");
      return false;
    }
    return true;
  }

  bool send_message(const uint8_t *data, size_t len) {
    if (!send(data, len)) {
      log_e("Failed to send message to master");
      return false;
    }
    return true;
  }
};

/* Global Variables */
ESP_NOW_Master_Peer *master = nullptr;
uint32_t totalDevices = 0;
uint32_t filteredDevices = 0;

/* BLE Callback */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    totalDevices++;
    
    String deviceInfo = advertisedDevice.toString();
    Serial.printf("BLE Device found: %s\n", deviceInfo.c_str());

    // Vérifier si le device contient "reseau_1" dans son nom ou ses données
    bool matchFound = false;
    
    // Recherche dans le nom de l'appareil
    if (advertisedDevice.haveName()) {
      String deviceName = advertisedDevice.getName().c_str();
      if (deviceName.indexOf(FILTER_KEYWORD) >= 0) {
        matchFound = true;
        Serial.println("  -> MATCH in device name!");
      }
    }
    
    // Recherche dans les données complètes (toString)
    if (!matchFound && deviceInfo.indexOf(FILTER_KEYWORD) >= 0) {
      matchFound = true;
      Serial.println("  -> MATCH in device data!");
    }

    // Si le filtre correspond, envoyer au master
    if (matchFound) {
      filteredDevices++;
      
      if (master != nullptr) {
        char data[250];
        snprintf(data, sizeof(data), "%s", deviceInfo.c_str());

        Serial.printf("  -> Sending to master: %s\n", data);

        if (!master->send_message((uint8_t *)data, strlen(data) + 1)) {
          Serial.println("  -> ERROR: Failed to send to master");
        } else {
          Serial.println("  -> SUCCESS: Sent to master");
        }
      }
    } else {
      Serial.println("  -> Filtered out (no match)");
    }
  }
};

/* Main */
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=== ESP-NOW Slave with BLE Scanner + Filter ===");
  Serial.printf("Filter keyword: '%s'\n", FILTER_KEYWORD);

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

  // Register master peer
  master = new ESP_NOW_Master_Peer(masterAddress, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);
  if (!master->add_peer()) {
    Serial.println("Failed to register master");
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.printf("Master registered: " MACSTR "\n", MAC2STR(masterAddress));

  // Initialize BLE
  Serial.println("Initializing BLE Scanner...");
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  Serial.println("Setup complete. Starting BLE scans...");
  Serial.println("Only devices containing 'reseau_1' will be sent to master\n");
}

void loop() {
  Serial.println("\n========== Starting BLE Scan ==========");
  BLEScanResults *foundDevices = pBLEScan->start(scanTime, false);
  
  Serial.printf("Scan complete!\n");
  Serial.printf("  Total devices found: %d\n", foundDevices->getCount());
  Serial.printf("  Devices matching filter: %lu\n", filteredDevices);
  Serial.printf("  Total devices scanned: %lu\n", totalDevices);
  Serial.println("=======================================\n");
  
  pBLEScan->clearResults();

  delay(5000);
}