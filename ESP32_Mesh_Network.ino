#include <esp_now.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_wifi.h>

// ============================================
// CONFIGURATION - À MODIFIER SELON VOTRE RÉSEAU
// ============================================
const char* ssid = "Freebox-60F745";           // ← Votre SSID WiFi
const char* password = "VOTRE_MOT_DE_PASSE";   // ← Votre mot de passe WiFi
const char* serverUrl = "http://192.168.1.100:3000/api/data";  // ← IP de votre serveur Node.js

// ============================================
// CONFIGURATION RÉSEAU MESH
// ============================================
#define MAX_NODES 20
#define SINK_HEARTBEAT_INTERVAL 3000    // SINK : heartbeat toutes les 3s
#define NODE_HEARTBEAT_INTERVAL 10000   // NODES : heartbeat toutes les 10s
#define SINK_TIMEOUT 15000              // 15s sans SINK = élection
#define INITIAL_SINK_TIMEOUT 30000      // 30s au démarrage avant première élection
#define CHECK_SINK_INTERVAL 5000        // Vérifier le SINK toutes les 5s
#define WIFI_CONNECT_TIMEOUT 40         // 40 tentatives = 20 secondes
#define WIFI_SCAN_INTERVAL 60000        // Scanner le WiFi toutes les 60 secondes

// ============================================
// STRUCTURE DE DONNÉES ESP-NOW
// ============================================
typedef struct struct_message {
  uint8_t nodeId;
  uint8_t msgType;      // 0=data, 1=election, 2=heartbeat, 3=sink_announcement
  bool isSink;
  float cpuUsage;
  uint32_t freeHeap;
  uint32_t totalHeap;
  uint32_t freeFlash;
  uint8_t connectedTo[6];
  int8_t rssi;
  int8_t wifiRssi;      // RSSI du WiFi pour élection
  uint32_t priority;    // Priorité pour élection
  char data[100];
  uint32_t timestamp;
  
  // Données pour le routage mesh
  uint8_t hopCount;        // Nombre de sauts depuis la source
  uint8_t originMAC[6];    // MAC du nœud d'origine
  uint8_t destMAC[6];      // MAC de destination (SINK)
  uint8_t routePath[5][6]; // Chemin parcouru (max 5 sauts)
} struct_message;

// ============================================
// STRUCTURE INFO NŒUD
// ============================================
struct NodeInfo {
  uint8_t mac[6];
  int8_t rssi;
  int8_t wifiRssi;
  bool isSink;
  uint32_t lastSeen;
  uint32_t priority;
  uint8_t hopCount;        // Distance en sauts jusqu'au SINK
  bool canReachSink;       // Ce nœud peut-il atteindre le SINK ?
  uint8_t nextHop[6];      // Prochain saut pour atteindre le SINK
};

// ============================================
// VARIABLES GLOBALES
// ============================================
struct_message myData;
struct_message incomingData;
WebServer server(80);

// État du nœud
bool iAmSink = false;
bool sinkKnown = false;
uint8_t currentSinkMAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMAC[6];
uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Timers
unsigned long lastDataSend = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastSinkCheck = 0;
unsigned long lastSinkSeen = 0;

// Variables de contrôle
bool forceElection = false;
int8_t cachedWiFiRSSI = -127;
unsigned long lastWiFiScan = 0;

// Liste des nœuds connus
NodeInfo knownNodes[MAX_NODES];
int nodeCount = 0;

// Variables de routage
uint8_t myBestNextHop[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myHopCountToSink = 255;

// Canal WiFi détecté
int detectedWiFiChannel = 1;

// ============================================
// DÉCLARATIONS FORWARD
// ============================================
// Nouvelle signature pour ESP32 Arduino Core v3.x (IDF v5.x)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status);
void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len);
bool connectToWiFi();
void setupWebServer();
void collectAndSendData();
void sendHeartbeat();
void announcePresence();
int8_t scanWiFiRSSI();
uint32_t calculatePriority();
void startElection();
void becomeSink();
void resignAsSink();
void updateNodeInfo(const uint8_t *mac, int8_t rssi);
void cleanupNodes(unsigned long now);
void sendToServer();
void sendReceivedDataToServer(const uint8_t *mac);
void sendNodeInfoToServer(const uint8_t *mac, int8_t rssi);
String getMacString(const uint8_t *mac);

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║     ESP32 Mesh Network Node v2.0       ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  // Toujours démarrer en NODE (rôle volatil)
  iAmSink = false;
  Serial.println("🔄 Démarrage en mode NODE");
  Serial.println("   (Le SINK sera élu selon le meilleur RSSI WiFi)");
  
  // Configuration WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(true);
  delay(100);
  
  // Obtenir le MAC
  esp_wifi_get_mac(WIFI_IF_STA, myMAC);
  Serial.printf("\n📱 MAC Address: %s\n", getMacString(myMAC).c_str());
  
  // Détecter le canal WiFi AVANT d'initialiser ESP-NOW
  Serial.println("\n🔍 Détection du canal WiFi...");
  detectWiFiChannel();
  
  // Initialiser ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Erreur initialisation ESP-NOW");
    ESP.restart();
  }
  Serial.println("✅ ESP-NOW initialisé");
  
  // Enregistrer callbacks
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  
  // Ajouter broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  // 0 = utiliser le canal actuel
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("⚠️  Échec ajout broadcast peer");
  }
  
  // Définir le canal ESP-NOW
  esp_wifi_set_channel(detectedWiFiChannel, WIFI_SECOND_CHAN_NONE);
  Serial.printf("📡 ESP-NOW sur canal: %d\n", detectedWiFiChannel);
  
  // Initialiser les données
  myData.nodeId = myMAC[5];
  myData.isSink = false;
  
  // Attendre que tous les nœuds démarrent
  Serial.println("\n⏳ Attente 3 secondes pour synchronisation...");
  delay(3000);
  
  // Envoyer annonce initiale
  Serial.println("📢 Envoi annonce initiale...");
  announcePresence();
  
  // Initialiser les timers
  lastSinkCheck = millis();
  lastSinkSeen = 0;
  
  Serial.println("\n✅ Prêt!");
  Serial.println("   - En attente d'un SINK existant");
  Serial.printf("   - Élection auto dans %d secondes si aucun SINK\n", INITIAL_SINK_TIMEOUT / 1000);
  Serial.println("════════════════════════════════════════\n");
}

// ============================================
// DÉTECTION DU CANAL WIFI
// ============================================
void detectWiFiChannel() {
  Serial.printf("   Recherche du réseau '%s'...\n", ssid);
  
  int n = WiFi.scanNetworks(false, false, false, 1000);
  
  if (n == 0) {
    Serial.println("   ⚠️  Aucun réseau trouvé, utilisation canal 1");
    detectedWiFiChannel = 1;
  } else {
    Serial.printf("   Trouvé %d réseaux\n", n);
    
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == String(ssid)) {
        detectedWiFiChannel = WiFi.channel(i);
        cachedWiFiRSSI = WiFi.RSSI(i);
        lastWiFiScan = millis();
        Serial.printf("   ✅ '%s' trouvé sur canal %d (RSSI: %d dBm)\n", 
                      ssid, detectedWiFiChannel, cachedWiFiRSSI);
        break;
      }
    }
  }
  
  WiFi.scanDelete();
}

// ============================================
// LOOP PRINCIPAL
// ============================================
void loop() {
  unsigned long now = millis();
  
  // Gestion du serveur web si SINK
  if (iAmSink) {
    server.handleClient();
  }
  
  // Heartbeat périodique
  unsigned long heartbeatInterval = iAmSink ? SINK_HEARTBEAT_INTERVAL : NODE_HEARTBEAT_INTERVAL;
  if (now - lastHeartbeat >= heartbeatInterval) {
    sendHeartbeat();
    lastHeartbeat = now;
  }
  
  // Envoi des données toutes les 10 secondes
  if (now - lastDataSend >= 10000) {
    collectAndSendData();
    lastDataSend = now;
  }
  
  // Vérification du SINK (uniquement si NODE)
  if (!iAmSink && (now - lastSinkCheck >= CHECK_SINK_INTERVAL)) {
    lastSinkCheck = now;
    
    if (!sinkKnown) {
      // Pas de SINK connu - vérifier le timeout initial
      if (now > INITIAL_SINK_TIMEOUT) {
        Serial.printf("\n⚠️  TIMEOUT - Aucun SINK détecté depuis %d secondes\n", 
                      INITIAL_SINK_TIMEOUT / 1000);
        Serial.println("🗳️  Lancement élection automatique...");
        startElection();
      }
    } else if (now - lastSinkSeen > SINK_TIMEOUT) {
      // SINK connu mais ne répond plus
      Serial.println("\n⚠️  SINK perdu - Pas de heartbeat depuis 15s");
      Serial.println("🗳️  Lancement élection automatique...");
      sinkKnown = false;
      memset(currentSinkMAC, 0xFF, 6);
      startElection();
    }
  }
  
  // Élection forcée depuis interface web
  if (forceElection) {
    Serial.println("\n🗳️  ÉLECTION MANUELLE demandée depuis interface web");
    forceElection = false;
    startElection();
  }
  
  // Nettoyage des nœuds obsolètes
  static unsigned long lastCleanup = 0;
  if (now - lastCleanup > 10000) {
    cleanupNodes(now);
    lastCleanup = now;
  }
}

// ============================================
// CONNEXION WIFI (AMÉLIORÉE)
// ============================================
bool connectToWiFi() {
  Serial.println("\n📶 Connexion WiFi...");
  Serial.printf("   SSID: %s\n", ssid);
  
  // Déconnexion propre d'abord
  WiFi.disconnect(true);
  delay(100);
  
  // Mode Station + AP pour ESP-NOW
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  
  // Première tentative
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  int maxAttempts = WIFI_CONNECT_TIMEOUT;
  
  Serial.print("   Connexion");
  
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    // Réessayer à mi-chemin si toujours pas connecté
    if (attempts == maxAttempts / 2) {
      Serial.println("\n   ⟳ Nouvelle tentative...");
      WiFi.disconnect();
      delay(500);
      WiFi.begin(ssid, password);
      Serial.print("   Connexion");
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("   ✅ WiFi connecté!");
    Serial.printf("   📍 IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("   📶 RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("   📡 Canal: %d\n", WiFi.channel());
    
    // Synchroniser le canal ESP-NOW
    detectedWiFiChannel = WiFi.channel();
    esp_wifi_set_channel(detectedWiFiChannel, WIFI_SECOND_CHAN_NONE);
    
    return true;
  } else {
    Serial.println("   ❌ Échec connexion WiFi!");
    Serial.printf("   Status: %d\n", WiFi.status());
    
    // Diagnostic
    switch(WiFi.status()) {
      case WL_NO_SSID_AVAIL:
        Serial.println("   → SSID non trouvé");
        break;
      case WL_CONNECT_FAILED:
        Serial.println("   → Mot de passe incorrect?");
        break;
      case WL_DISCONNECTED:
        Serial.println("   → Déconnecté");
        break;
      default:
        Serial.println("   → Erreur inconnue");
    }
    
    return false;
  }
}

// ============================================
// DEVENIR SINK (CORRIGÉ)
// ============================================
void becomeSink() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         DEVENIR SINK                   ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  // ÉTAPE 1: Connecter au WiFi D'ABORD
  if (!connectToWiFi()) {
    Serial.println("\n❌ Impossible de devenir SINK sans WiFi!");
    Serial.println("   Un autre nœud avec meilleur accès WiFi devrait être élu.");
    Serial.println("   Retour en mode NODE...\n");
    
    // Réinitialiser pour permettre une nouvelle élection
    sinkKnown = false;
    lastSinkCheck = millis();
    return;
  }
  
  // ÉTAPE 2: Maintenant on peut devenir SINK
  iAmSink = true;
  myData.isSink = true;
  sinkKnown = true;
  memcpy(currentSinkMAC, myMAC, 6);
  
  // ÉTAPE 3: Configurer le routage
  myHopCountToSink = 0;
  memcpy(myBestNextHop, myMAC, 6);
  
  // ÉTAPE 4: Démarrer le serveur web
  setupWebServer();
  
  // ÉTAPE 5: Annoncer le nouveau SINK
  delay(200);
  announcePresence();
  delay(200);
  sendHeartbeat();
  delay(200);
  sendHeartbeat();  // Double envoi pour fiabilité
  
  Serial.println("\n🎯 MODE SINK ACTIVÉ!");
  Serial.printf("   Serveur web: http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.println("════════════════════════════════════════\n");
}

// ============================================
// DÉMISSIONNER COMME SINK
// ============================================
void resignAsSink() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         DÉMISSION SINK                 ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  iAmSink = false;
  myData.isSink = false;
  sinkKnown = false;
  memset(currentSinkMAC, 0xFF, 6);
  
  // Arrêter le serveur web
  server.stop();
  server.close();
  
  // Déconnecter WiFi
  WiFi.disconnect(true);
  delay(100);
  
  // Réinitialiser le routage
  myHopCountToSink = 255;
  memset(myBestNextHop, 0xFF, 6);
  
  // Remettre ESP-NOW sur le bon canal
  esp_wifi_set_channel(detectedWiFiChannel, WIFI_SECOND_CHAN_NONE);
  
  // Réinitialiser les timers
  lastSinkCheck = millis();
  lastSinkSeen = 0;
  
  // Annoncer la démission
  announcePresence();
  
  Serial.println("✅ Démission effectuée - Mode NODE actif");
  Serial.println("   Une nouvelle élection va avoir lieu...\n");
}

// ============================================
// SETUP SERVEUR WEB
// ============================================
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/resign", handleResign);
  server.on("/election", handleStartElection);
  server.on("/update", HTTP_POST, handleUpdateEnd, handleUpdateUpload);
  server.begin();
  Serial.println("   🌐 Serveur web démarré");
}

// ============================================
// HANDLERS SERVEUR WEB
// ============================================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>ESP32 SINK</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;}";
  html += ".card{background:white;padding:20px;margin:15px 0;border-radius:12px;box-shadow:0 4px 15px rgba(0,0,0,0.1);}";
  html += "h1{color:white;text-align:center;margin-bottom:20px;}";
  html += "h2{color:#667eea;margin-top:0;}";
  html += "button{padding:12px 24px;background:#667eea;color:white;border:none;border-radius:8px;cursor:pointer;font-size:14px;margin:5px;transition:all 0.3s;}";
  html += "button:hover{background:#764ba2;transform:translateY(-2px);}";
  html += ".btn-warning{background:#ff9800;} .btn-warning:hover{background:#f57c00;}";
  html += ".btn-danger{background:#f44336;} .btn-danger:hover{background:#d32f2f;}";
  html += "table{width:100%;border-collapse:collapse;margin-top:10px;}";
  html += "th,td{padding:10px;text-align:left;border-bottom:1px solid #eee;}";
  html += "th{background:#f5f5f5;color:#667eea;}";
  html += ".status{display:inline-block;padding:4px 12px;border-radius:20px;font-size:12px;font-weight:bold;}";
  html += ".online{background:#4caf50;color:white;}";
  html += ".sink-badge{background:#f5576c;color:white;padding:2px 8px;border-radius:4px;font-size:11px;margin-left:5px;}";
  html += "</style></head><body>";
  
  html += "<h1>🌐 ESP32 SINK Node</h1>";
  
  // Carte informations
  html += "<div class='card'><h2>📊 Informations</h2>";
  html += "<p><strong>MAC:</strong> " + getMacString(myMAC) + "</p>";
  html += "<p><strong>IP:</strong> " + WiFi.localIP().toString() + "</p>";
  html += "<p><strong>WiFi RSSI:</strong> " + String(WiFi.RSSI()) + " dBm</p>";
  html += "<p><strong>Canal:</strong> " + String(WiFi.channel()) + "</p>";
  html += "<p><strong>Heap Libre:</strong> " + String(ESP.getFreeHeap() / 1024) + " KB</p>";
  html += "<p><strong>Uptime:</strong> " + String(millis() / 1000) + " secondes</p>";
  html += "<p><strong>Nœuds connus:</strong> " + String(nodeCount) + "</p>";
  html += "</div>";
  
  // Carte nœuds
  html += "<div class='card'><h2>📡 Nœuds du Réseau</h2>";
  if (nodeCount == 0) {
    html += "<p>Aucun nœud détecté pour le moment...</p>";
  } else {
    html += "<table>";
    html += "<tr><th>MAC</th><th>RSSI ESP-NOW</th><th>RSSI WiFi</th><th>Rôle</th><th>Dernière vue</th></tr>";
    
    for (int i = 0; i < nodeCount; i++) {
      unsigned long ago = (millis() - knownNodes[i].lastSeen) / 1000;
      String status = ago < 30 ? "online" : "offline";
      
      html += "<tr>";
      html += "<td>" + getMacString(knownNodes[i].mac);
      if (knownNodes[i].isSink) html += "<span class='sink-badge'>SINK</span>";
      html += "</td>";
      html += "<td>" + String(knownNodes[i].rssi) + " dBm</td>";
      html += "<td>" + String(knownNodes[i].wifiRssi) + " dBm</td>";
      html += "<td>" + String(knownNodes[i].isSink ? "🎯 SINK" : "📡 NODE") + "</td>";
      html += "<td><span class='status " + status + "'>" + String(ago) + "s</span></td>";
      html += "</tr>";
    }
    html += "</table>";
  }
  html += "</div>";
  
  // Carte actions
  html += "<div class='card'><h2>⚡ Actions</h2>";
  html += "<button class='btn-danger' onclick=\"if(confirm('Démissionner comme SINK ?')) location.href='/resign'\">📤 Démissionner</button>";
  html += "<button class='btn-warning' onclick=\"if(confirm('Lancer une nouvelle élection ?')) location.href='/election'\">🗳️ Forcer Élection</button>";
  html += "<br><br>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data' style='display:inline;'>";
  html += "<input type='file' name='update' accept='.bin' style='margin-right:10px;'>";
  html += "<button type='submit'>📦 Upload OTA</button>";
  html += "</form>";
  html += "</div>";
  
  html += "<script>setTimeout(function(){location.reload();},10000);</script>";  // Auto-refresh 10s
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleStatus() {
  StaticJsonDocument<1024> doc;
  doc["mac"] = getMacString(myMAC);
  doc["isSink"] = iAmSink;
  doc["wifiRssi"] = WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["nodeCount"] = nodeCount;
  doc["uptime"] = millis() / 1000;
  
  JsonArray nodes = doc.createNestedArray("nodes");
  for (int i = 0; i < nodeCount; i++) {
    JsonObject node = nodes.createNestedObject();
    node["mac"] = getMacString(knownNodes[i].mac);
    node["rssi"] = knownNodes[i].rssi;
    node["wifiRssi"] = knownNodes[i].wifiRssi;
    node["isSink"] = knownNodes[i].isSink;
    node["lastSeen"] = (millis() - knownNodes[i].lastSeen) / 1000;
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleResign() {
  Serial.println("📤 Démission demandée via interface web");
  
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='5;url=/'>";
  html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#f0f0f0;}</style>";
  html += "</head><body>";
  html += "<h1>✅ Démission effectuée</h1>";
  html += "<p>Vous n'êtes plus SINK.</p>";
  html += "<p>Une nouvelle élection va commencer automatiquement.</p>";
  html += "<p><small>Redirection dans 5 secondes...</small></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
  
  delay(500);
  resignAsSink();
}

void handleStartElection() {
  Serial.println("🗳️  Élection demandée via interface web");
  
  // Envoyer message d'élection
  myData.msgType = 1;
  myData.isSink = iAmSink;
  myData.priority = calculatePriority();
  esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
  
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='8;url=/'>";
  html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#f0f0f0;}</style>";
  html += "</head><body>";
  html += "<h1>🗳️ Élection lancée</h1>";
  html += "<p>Tous les nœuds participent à l'élection.</p>";
  html += "<p>Le nœud avec le meilleur signal WiFi deviendra SINK.</p>";
  html += "<p><small>Redirection dans 8 secondes...</small></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
  
  delay(500);
  
  // Démissionner pour permettre une vraie élection
  if (iAmSink) {
    resignAsSink();
  }
  
  forceElection = true;
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("📦 Update: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("✅ Update Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleUpdateEnd() {
  server.sendHeader("Connection", "close");
  if (Update.hasError()) {
    server.send(500, "text/plain", "FAIL");
  } else {
    server.send(200, "text/plain", "OK - Rebooting...");
    delay(1000);
    ESP.restart();
  }
}

// ============================================
// COLLECTE ET ENVOI DES DONNÉES
// ============================================
void collectAndSendData() {
  myData.msgType = 0;  // Data
  myData.cpuUsage = random(10, 80);
  myData.freeHeap = ESP.getFreeHeap();
  myData.totalHeap = ESP.getHeapSize();
  myData.freeFlash = ESP.getFreeSketchSpace();
  myData.timestamp = millis();
  myData.wifiRssi = iAmSink ? WiFi.RSSI() : scanWiFiRSSI();
  
  // Données capteurs simulées
  float temp = random(200, 300) / 10.0;
  float humidity = random(400, 800) / 10.0;
  snprintf(myData.data, sizeof(myData.data), 
           "{\"temp\":%.1f,\"hum\":%.1f}", temp, humidity);
  
  // Informations de routage
  memcpy(myData.originMAC, myMAC, 6);
  myData.hopCount = myHopCountToSink;
  memcpy(myData.routePath[0], myMAC, 6);
  
  if (iAmSink) {
    sendToServer();
  } else {
    if (sinkKnown && myHopCountToSink < 255) {
      memcpy(myData.connectedTo, myBestNextHop, 6);
      memcpy(myData.destMAC, currentSinkMAC, 6);
      
      Serial.printf("📤 Envoi données via %s (hop %d)\n", 
                    getMacString(myBestNextHop).c_str(), myHopCountToSink);
      
      esp_now_send(myBestNextHop, (uint8_t *)&myData, sizeof(myData));
    } else {
      Serial.println("📡 Broadcast données (pas de route vers SINK)");
      esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
    }
  }
}

// ============================================
// HEARTBEAT
// ============================================
void sendHeartbeat() {
  myData.msgType = 2;  // Heartbeat
  myData.isSink = iAmSink;
  myData.wifiRssi = iAmSink ? WiFi.RSSI() : scanWiFiRSSI();
  myData.priority = calculatePriority();
  myData.hopCount = iAmSink ? 0 : myHopCountToSink;
  memcpy(myData.destMAC, iAmSink ? myMAC : currentSinkMAC, 6);
  memcpy(myData.originMAC, myMAC, 6);
  
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
  
  if (result == ESP_OK) {
    Serial.printf("💓 Heartbeat (isSink=%d, hop=%d, priority=%d)\n", 
                  iAmSink, myData.hopCount, myData.priority);
  } else {
    Serial.printf("❌ Heartbeat échec: %d\n", result);
  }
}

// ============================================
// ANNONCE DE PRÉSENCE
// ============================================
void announcePresence() {
  myData.msgType = 3;  // Sink announcement
  myData.isSink = iAmSink;
  myData.wifiRssi = iAmSink ? WiFi.RSSI() : scanWiFiRSSI();
  myData.priority = calculatePriority();
  myData.hopCount = iAmSink ? 0 : myHopCountToSink;
  
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
  
  Serial.printf("📢 Annonce (isSink=%d, priority=%d) - %s\n", 
                iAmSink, myData.priority, result == ESP_OK ? "OK" : "ÉCHEC");
}

// ============================================
// SCAN RSSI WIFI
// ============================================
int8_t scanWiFiRSSI() {
  // Utiliser le cache si récent
  if (cachedWiFiRSSI != -127 && (millis() - lastWiFiScan < WIFI_SCAN_INTERVAL)) {
    return cachedWiFiRSSI;
  }
  
  Serial.printf("🔍 Scan WiFi '%s'... ", ssid);
  
  int n = WiFi.scanNetworks(false, false, false, 500);
  int8_t rssi = -127;
  
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == String(ssid)) {
        rssi = WiFi.RSSI(i);
        Serial.printf("trouvé: %d dBm\n", rssi);
        break;
      }
    }
    if (rssi == -127) {
      Serial.println("SSID non trouvé!");
    }
  } else {
    Serial.println("aucun réseau!");
  }
  
  WiFi.scanDelete();
  
  cachedWiFiRSSI = rssi;
  lastWiFiScan = millis();
  
  return rssi;
}

// ============================================
// CALCUL PRIORITÉ
// ============================================
uint32_t calculatePriority() {
  int8_t wifiRssi = iAmSink ? WiFi.RSSI() : scanWiFiRSSI();
  
  if (wifiRssi == -127 || wifiRssi < -100) {
    return 10;  // Priorité très basse
  }
  
  // Priorité = 127 + RSSI (plus le RSSI est proche de 0, plus la priorité est haute)
  // RSSI -30 dBm → priorité 97
  // RSSI -50 dBm → priorité 77
  // RSSI -70 dBm → priorité 57
  // RSSI -90 dBm → priorité 37
  return 127 + wifiRssi;
}

// ============================================
// ÉLECTION
// ============================================
void startElection() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         ÉLECTION SINK                  ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  // Forcer un nouveau scan WiFi
  cachedWiFiRSSI = -127;
  
  uint32_t myPriority = calculatePriority();
  uint32_t bestPriority = myPriority;
  int bestNodeIndex = -1;
  
  Serial.printf("📊 Ma priorité: %d (RSSI: %d dBm)\n", myPriority, scanWiFiRSSI());
  
  // Comparer avec les autres nœuds connus
  Serial.println("\n   Comparaison avec les autres nœuds:");
  for (int i = 0; i < nodeCount; i++) {
    // Ne considérer que les nœuds vus récemment
    if (millis() - knownNodes[i].lastSeen > 10000) continue;
    
    Serial.printf("   - %s: priorité=%d, wifiRssi=%d\n", 
                  getMacString(knownNodes[i].mac).c_str(),
                  knownNodes[i].priority, 
                  knownNodes[i].wifiRssi);
    
    if (knownNodes[i].priority > bestPriority) {
      bestPriority = knownNodes[i].priority;
      bestNodeIndex = i;
    } else if (knownNodes[i].priority == bestPriority) {
      // Tie-breaker: MAC le plus petit gagne
      if (memcmp(knownNodes[i].mac, myMAC, 6) < 0) {
        bestNodeIndex = i;
      }
    }
  }
  
  if (bestNodeIndex == -1) {
    Serial.println("\n🏆 J'ai la meilleure priorité! Je deviens SINK...");
    becomeSink();
  } else {
    Serial.printf("\n⏳ Le nœud %s a une meilleure priorité (%d)\n", 
                  getMacString(knownNodes[bestNodeIndex].mac).c_str(),
                  bestPriority);
    Serial.println("   Attente que ce nœud devienne SINK...\n");
    
    // Attendre un peu puis vérifier
    delay(5000);
    
    // Si toujours pas de SINK, réessayer
    if (!sinkKnown) {
      Serial.println("⚠️  Le nœud attendu n'est pas devenu SINK, nouvelle tentative...");
      // Ne pas rappeler startElection() immédiatement pour éviter boucle infinie
    }
  }
}

// ============================================
// CALLBACKS ESP-NOW
// ============================================
// Nouvelle signature pour ESP32 Arduino Core v3.x (IDF v5.x)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    // Log seulement les échecs importants
    // Serial.println("⚠️  ESP-NOW envoi échec");
  }
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(struct_message)) return;
  
  memcpy(&incomingData, data, sizeof(incomingData));
  
  Serial.printf("📨 Reçu de %s (type=%d, isSink=%d, priority=%d)\n",
                getMacString(info->src_addr).c_str(),
                incomingData.msgType, 
                incomingData.isSink,
                incomingData.priority);
  
  // Mettre à jour les infos du nœud
  updateNodeInfo(info->src_addr, info->rx_ctrl->rssi);
  
  // DÉTECTION DE CONFLIT: Si je suis SINK et reçois un autre SINK
  if (iAmSink && incomingData.isSink && 
      (incomingData.msgType == 2 || incomingData.msgType == 3)) {
    
    uint32_t myPriority = calculatePriority();
    Serial.printf("⚠️  CONFLIT SINK! Ma priorité: %d vs Autre: %d\n", 
                  myPriority, incomingData.priority);
    
    if (incomingData.priority > myPriority) {
      Serial.println("🔻 L'autre SINK a meilleure priorité, je démissionne...");
      resignAsSink();
      return;
    } else if (incomingData.priority == myPriority) {
      // Tie-breaker: MAC le plus petit gagne
      if (memcmp(info->src_addr, myMAC, 6) < 0) {
        Serial.println("🔻 Tie-breaker: je démissionne (autre MAC plus petit)");
        resignAsSink();
        return;
      }
    }
    Serial.println("✅ Je garde mon rôle de SINK (meilleure priorité)");
  }
  
  // Traiter selon le type de message
  switch (incomingData.msgType) {
    case 0:  // Data
      if (iAmSink) {
        sendReceivedDataToServer(info->src_addr);
      }
      break;
      
    case 1:  // Election
      Serial.println("🗳️  Message d'élection reçu");
      sendHeartbeat();  // Répondre avec ma priorité
      
      if (iAmSink && incomingData.priority > calculatePriority()) {
        Serial.println("🔻 Challenger avec meilleure priorité, je démissionne...");
        resignAsSink();
      }
      break;
      
    case 2:  // Heartbeat
    case 3:  // Sink announcement
      // Si je suis SINK, envoyer les infos du nœud au serveur
      if (iAmSink && !incomingData.isSink) {
        sendNodeInfoToServer(info->src_addr, info->rx_ctrl->rssi);
      }
      
      if (incomingData.isSink && !iAmSink) {
        // Un SINK existe!
        if (!sinkKnown || memcmp(currentSinkMAC, info->src_addr, 6) != 0) {
          Serial.printf("🎯 SINK détecté: %s\n", getMacString(info->src_addr).c_str());
        }
        sinkKnown = true;
        lastSinkSeen = millis();
        memcpy(currentSinkMAC, info->src_addr, 6);
        
        // Mettre à jour le routage
        myHopCountToSink = 1;
        memcpy(myBestNextHop, info->src_addr, 6);
      }
      break;
  }
}

// ============================================
// MISE À JOUR INFO NŒUD
// ============================================
void updateNodeInfo(const uint8_t *mac, int8_t rssi) {
  int index = -1;
  
  // Chercher si le nœud existe déjà
  for (int i = 0; i < nodeCount; i++) {
    if (memcmp(knownNodes[i].mac, mac, 6) == 0) {
      index = i;
      break;
    }
  }
  
  // Nouveau nœud
  if (index == -1 && nodeCount < MAX_NODES) {
    index = nodeCount;
    nodeCount++;
    memcpy(knownNodes[index].mac, mac, 6);
    Serial.printf("➕ Nouveau nœud: %s\n", getMacString(mac).c_str());
  }
  
  // Mettre à jour les infos
  if (index != -1) {
    knownNodes[index].rssi = rssi;
    knownNodes[index].wifiRssi = incomingData.wifiRssi;
    knownNodes[index].isSink = incomingData.isSink;
    knownNodes[index].lastSeen = millis();
    knownNodes[index].priority = incomingData.priority;
    knownNodes[index].hopCount = incomingData.hopCount;
    
    if (incomingData.isSink) {
      knownNodes[index].canReachSink = true;
      knownNodes[index].hopCount = 0;
      memcpy(knownNodes[index].nextHop, mac, 6);
    } else if (incomingData.hopCount < 255) {
      knownNodes[index].canReachSink = true;
      memcpy(knownNodes[index].nextHop, mac, 6);
    } else {
      knownNodes[index].canReachSink = false;
    }
  }
}

// ============================================
// NETTOYAGE NŒUDS OBSOLÈTES
// ============================================
void cleanupNodes(unsigned long now) {
  for (int i = 0; i < nodeCount; i++) {
    if (now - knownNodes[i].lastSeen > 60000) {  // 60 secondes
      Serial.printf("🗑️  Suppression nœud inactif: %s\n", 
                    getMacString(knownNodes[i].mac).c_str());
      
      // Décaler les nœuds suivants
      for (int j = i; j < nodeCount - 1; j++) {
        knownNodes[j] = knownNodes[j + 1];
      }
      nodeCount--;
      i--;
    }
  }
}

// ============================================
// ENVOI AU SERVEUR
// ============================================
void sendToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi déconnecté, impossible d'envoyer au serveur");
    return;
  }
  
  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  
  StaticJsonDocument<512> doc;
  doc["mac"] = getMacString(myMAC);
  doc["nodeId"] = myData.nodeId;
  doc["nodeType"] = 1;  // SINK
  doc["cpuUsage"] = myData.cpuUsage;
  doc["freeHeap"] = myData.freeHeap;
  doc["totalHeap"] = myData.totalHeap;
  doc["freeFlash"] = myData.freeFlash;
  doc["rssi"] = WiFi.RSSI();
  doc["isSink"] = true;
  doc["ip"] = WiFi.localIP().toString();
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  int httpCode = http.POST(jsonString);
  
  if (httpCode > 0) {
    Serial.printf("📤 Données envoyées au serveur (HTTP %d)\n", httpCode);
  } else {
    Serial.printf("❌ Erreur envoi serveur: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
}

void sendReceivedDataToServer(const uint8_t *mac) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  
  StaticJsonDocument<512> doc;
  doc["mac"] = getMacString(mac);
  doc["nodeId"] = incomingData.nodeId;
  doc["nodeType"] = 0;  // NODE
  doc["cpuUsage"] = incomingData.cpuUsage;
  doc["freeHeap"] = incomingData.freeHeap;
  doc["totalHeap"] = incomingData.totalHeap;
  doc["freeFlash"] = incomingData.freeFlash;
  doc["rssi"] = incomingData.rssi;
  doc["wifiRssi"] = incomingData.wifiRssi;
  doc["connectedTo"] = getMacString(myMAC);
  doc["isSink"] = false;
  
  // Parser les données capteur
  StaticJsonDocument<200> sensorDoc;
  if (deserializeJson(sensorDoc, incomingData.data) == DeserializationError::Ok) {
    doc["sensorData"] = sensorDoc;
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  int httpCode = http.POST(jsonString);
  
  if (httpCode > 0) {
    Serial.printf("📤 Données de %s relayées au serveur\n", getMacString(mac).c_str());
  }
  
  http.end();
}

// Nouvelle fonction pour envoyer les infos des nœuds (heartbeat/announcement)
void sendNodeInfoToServer(const uint8_t *mac, int8_t rssi) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  // Limiter la fréquence d'envoi pour éviter de surcharger le serveur
  static unsigned long lastNodeInfoSend = 0;
  static uint8_t lastNodeMAC[6] = {0};
  
  // Ne pas envoyer plus d'une fois par seconde pour le même nœud
  if (memcmp(lastNodeMAC, mac, 6) == 0 && millis() - lastNodeInfoSend < 5000) {
    return;
  }
  memcpy(lastNodeMAC, mac, 6);
  lastNodeInfoSend = millis();
  
  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  
  StaticJsonDocument<512> doc;
  doc["mac"] = getMacString(mac);
  doc["nodeId"] = incomingData.nodeId;
  doc["nodeType"] = 0;  // NODE
  doc["cpuUsage"] = incomingData.cpuUsage;
  doc["freeHeap"] = incomingData.freeHeap;
  doc["totalHeap"] = incomingData.totalHeap;
  doc["freeFlash"] = incomingData.freeFlash;
  doc["rssi"] = rssi;  // RSSI ESP-NOW mesuré par le SINK
  doc["wifiRssi"] = incomingData.wifiRssi;
  doc["priority"] = incomingData.priority;
  doc["connectedTo"] = getMacString(myMAC);
  doc["isSink"] = false;
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  int httpCode = http.POST(jsonString);
  
  if (httpCode > 0) {
    Serial.printf("📡 Info nœud %s envoyée au serveur\n", getMacString(mac).c_str());
  } else {
    Serial.printf("❌ Erreur envoi info nœud: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
}

// ============================================
// UTILITAIRE MAC → STRING
// ============================================
String getMacString(const uint8_t *mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}
