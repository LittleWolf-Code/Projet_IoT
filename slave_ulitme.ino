/*
    ESP-NOW Slave with BLE Scanner + Filter
    Le slave scanne les appareils BLE, filtre ceux contenant "reseau_1"
    et envoie la liste complète au master à la fin du scan
*/

#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <vector>

/* Definitions */
#define ESPNOW_WIFI_CHANNEL 6

// IMPORTANT: Remplacez par l'adresse MAC de votre MASTER
uint8_t masterAddress[] = {0x08, 0xF9, 0xE0, 0x00, 0xE2, 0x60}; // À MODIFIER !

// Filtre : mot-clé à rechercher dans les appareils BLE
const char* FILTER_KEYWORD = "reseau_1";

/* BLE Variables */
int scanTime = 1; // en secondes
BLEScan* pBLEScan;

// Liste pour stocker les appareils filtrés pendant le scan
std::vector<String> filteredDevicesList;

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

    // Vérifier si le device contient "reseau_1" dans son nom ou ses données
    bool matchFound = false;
    
    // Recherche dans le nom de l'appareil
    if (advertisedDevice.haveName()) {
      String deviceName = advertisedDevice.getName().c_str();
      if (deviceName.indexOf(FILTER_KEYWORD) >= 0) {
        matchFound = true;
        Serial.printf("BLE Device found: %s\n", deviceInfo.c_str());
        Serial.println("  -> MATCH in device name!");
        filteredDevices++;
        filteredDevicesList.push_back(deviceInfo);
        Serial.println("  -> Added to send list");
      }
    } else {
      //Serial.println("  -> Filtered out (no match)");
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
  // Réinitialiser les compteurs et la liste
  filteredDevices = 0;
  filteredDevicesList.clear();
  
  Serial.println("\n========== Starting BLE Scan ==========");
  BLEScanResults *foundDevices = pBLEScan->start(scanTime, false);
  
  Serial.printf("Scan complete!\n");
  Serial.printf("  Total devices found: %d\n", foundDevices->getCount());
  Serial.printf("  Devices matching filter: %lu\n", filteredDevices);
  Serial.printf("  Total devices scanned: %lu\n", totalDevices);
  
  // Envoyer tous les appareils filtrés au master
  if (filteredDevicesList.size() > 0 && master != nullptr) {
    Serial.println("\n--- Sending filtered devices to master ---");
    
    for (int i = 0; i < filteredDevicesList.size(); i++) {
      char data[250];
      snprintf(data, sizeof(data), "%s", filteredDevicesList[i].c_str());
      
      Serial.printf("[%d/%d] Sending: %s\n", i+1, filteredDevicesList.size(), data);
      
      if (!master->send_message((uint8_t *)data, strlen(data) + 1)) {
        Serial.println("  -> ERROR: Failed to send to master");
      } else {
        Serial.println("  -> SUCCESS: Sent to master");
      }
      
      delay(10); // Petit délai entre chaque envoi pour éviter la surcharge
    }
    
    Serial.println("--- All devices sent ---");
  } else {
    Serial.println("No filtered devices to send");
  }
  
  Serial.println("=======================================\n");
  
  pBLEScan->clearResults();

  delay(5000);
}
