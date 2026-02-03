# Tests pour ESP32 Sink/Mote Network

## Configuration du Matériel

| Rôle | MAC Address | MOTE_ID |
|------|-------------|---------|
| **SINK** | ec:62:60:5b:35:08 | - |
| Mote 0 | 24:dc:c3:14:37:98 | 0 |
| Mote 1 | 08:f9:e0:00:e2:60 | 1 |
| Mote 2 | ec:62:60:11:a2:3c | 2 |

**MQTT Broker:** 192.168.1.21:1883

---

## Test 1: Vérification de Compilation

### Procédure
1. Ouvrir Arduino IDE
2. Sélectionner **ESP32 Dev Module** dans Outils > Type de carte
3. Charger `sink.ino` et cliquer sur **Vérifier** (✓)
4. Charger `mote.ino` et cliquer sur **Vérifier** (✓)

### Résultat Attendu
```
Sketch uses X bytes of program storage space.
Global variables use Y bytes of dynamic memory.
```

---

## Test 2: Démarrage du SINK

### Procédure
1. Flash `sink.ino` sur l'ESP32 SINK (ec:62:60:5b:35:08)
2. Ouvrir Serial Monitor à 115200 baud

### Résultat Attendu
```
╔════════════════════════════════════════╗
║     ESP32 SINK Node - ESP-NOW + MQTT   ║
╚════════════════════════════════════════╝
📶 Connecting to WiFi.....
✅ WiFi connected!
   IP Address: 192.168.1.X
   WiFi Channel: X
   Sink MAC Address: ec:62:60:5b:35:08
✅ ESP-NOW initialized
✅ Peer added: Mote 0 (24:dc:c3:14:37:98)
✅ Peer added: Mote 1 (08:f9:e0:00:e2:60)
✅ Peer added: Mote 2 (ec:62:60:11:a2:3c)

=== SINK ROUTING TABLE ===
Mote 0: 24:dc:c3:14:37:98 -> direct
Mote 1: 08:f9:e0:00:e2:60 -> direct
Mote 2: ec:62:60:11:a2:3c -> direct
===========================

✅ SINK ready! Waiting for mote messages...
```

---

## Test 3: Démarrage d'un MOTE

### Procédure
1. Modifier `MOTE_ID` dans `mote.ino` selon l'ESP32 utilisé
2. Flash `mote.ino` sur l'ESP32
3. Ouvrir Serial Monitor à 115200 baud

### Résultat Attendu
```
╔════════════════════════════════════════╗
║   ESP32 MOTE Node - ESP-NOW + BLE      ║
╚════════════════════════════════════════╝
MOTE ID: 0
📡 WiFi channel detected: X
📍 Mote MAC Address: 24:dc:c3:14:37:98
✅ ESP-NOW initialized
✅ SINK peer added: ec:62:60:5b:35:08

=== MOTE ROUTING TABLE ===
My ID: Mote 0
My MAC: 24:dc:c3:14:37:98
Destination: SINK (ec:62:60:5b:35:08)
Next Hop: ec:62:60:5b:35:08
Hop Count: 1
===========================

🔵 Initializing BLE Scanner...
✅ BLE Scanner ready
✅ MOTE ready!
```

---

## Test 4: Communication ESP-NOW (Mote → Sink)

### Procédure
1. SINK et au moins 1 MOTE allumés avec Serial Monitor
2. Attendre le scan BLE du mote (toutes les 30s)

### Résultat Attendu sur MOTE
```
=== Starting BLE Scan ===
Filter: 'reseau1*'
Duration: 5 seconds
=== BLE Scan Complete ===
Total devices: X
Matching 'reseau1': 0
===========================
```

### Résultat Attendu sur SINK
```
💓 Heartbeat received from Mote X
```

---

## Test 5: Scan BLE avec Appareil "reseau1"

### Préparation
1. Installer une app BLE beacon sur smartphone (ex: "nRF Connect")
2. Créer un beacon avec nom: `reseau1_test`
3. Activer le beacon

### Procédure
1. Mote effectue un scan BLE
2. Vérifier les logs

### Résultat Attendu sur MOTE
```
=== Starting BLE Scan ===
   📱 BLE MATCH: reseau1_test [RSSI: -45]
=== BLE Scan Complete ===
Total devices: X
Matching 'reseau1': 1
📱 BLE data sent: {"name":"reseau1_test","mac":"XX:XX:XX:XX:XX:XX","rssi":-45} - OK
```

### Résultat Attendu sur SINK
```
📥 Packet received from: 24:dc:c3:14:37:98, length: X bytes
   Origin MAC: 24:dc:c3:14:37:98 (Mote ID: 0)
   Message type: 1
   📱 BLE Device: {"name":"reseau1_test","mac":"XX:XX:XX:XX:XX:XX","rssi":-45}
📡 MQTT published BLE to: esp32/24:dc:c3:14:37:98/ble
```

---

## Test 6: MQTT Publishing

### Préparation
1. Installer MQTT Explorer ou utiliser `mosquitto_sub`
2. Connecter au broker 192.168.1.21

### Procédure
```bash
mosquitto_sub -h 192.168.1.21 -t "esp32/#" -v
```

### Résultat Attendu
```
esp32/24:dc:c3:14:37:98/ble {"name":"reseau1_test","mac":"XX:XX","rssi":-45}
esp32/24:dc:c3:14:37:98/timestamp 12345
esp32/24:dc:c3:14:37:98/hops 1
```

---

## Test 7: Commande MQTT → Mote

### Procédure
```bash
mosquitto_pub -h 192.168.1.21 -t "esp32/24:dc:c3:14:37:98/output" -m "on"
```

### Résultat Attendu sur SINK
```
📩 MQTT received - Topic: esp32/24:dc:c3:14:37:98/output, Message: on
   Sending ON command to Mote 0
✅ Message sent to Mote 0
```

### Résultat Attendu sur MOTE
```
📥 Received from: ec:62:60:5b:35:08, length: X
   Command for Board ID: 0
   ✅ Command is for this mote!
   💡 LED ON
```

---

## Dépannage

| Problème | Solution |
|----------|----------|
| "Failed to add peer" | Vérifier les adresses MAC |
| "ESP-NOW send: Fail" | Vérifier que les deux ESP32 sont sur le même canal WiFi |
| Pas de réception BLE | Assurer que le nom du beacon commence par "reseau1" |
| MQTT déconnecté | Vérifier l'IP du broker et le réseau WiFi |
