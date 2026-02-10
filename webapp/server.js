const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const mqtt = require('mqtt');
const fs = require('fs');
const path = require('path');

const app = express();
const server = http.createServer(app);
const io = new Server(server);

const PORT = 3000;
const CONFIG_PATH = path.join(__dirname, 'config.json');
const DEVICES_PATH = path.join(__dirname, 'devices.json');

// ============================================
// DATA STORES (in-memory)
// ============================================
let espDevices = {};      // MAC -> { data0, data1, timestamp, hops, lastSeen, online }
let bleDevices = {};      // deviceName -> { motes: { mac -> rssi }, closestMote, lastSeen }
let mqttConnected = false;
let mqttClient = null;

// ============================================
// CONFIG & DEVICES FILE HELPERS
// ============================================
function loadConfig() {
  try {
    return JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
  } catch (e) {
    const defaultConfig = { mqtt: { broker: '172.18.32.43', port: 1883 } };
    fs.writeFileSync(CONFIG_PATH, JSON.stringify(defaultConfig, null, 2));
    return defaultConfig;
  }
}

function saveConfig(config) {
  fs.writeFileSync(CONFIG_PATH, JSON.stringify(config, null, 2));
}

function loadDevices() {
  try {
    return JSON.parse(fs.readFileSync(DEVICES_PATH, 'utf8'));
  } catch (e) {
    fs.writeFileSync(DEVICES_PATH, '[]');
    return [];
  }
}

function saveDevices(devices) {
  fs.writeFileSync(DEVICES_PATH, JSON.stringify(devices, null, 2));
}

// ============================================
// MQTT CONNECTION
// ============================================
function connectMQTT() {
  const config = loadConfig();
  const brokerUrl = `mqtt://${config.mqtt.broker}:${config.mqtt.port}`;

  // Disconnect existing client
  if (mqttClient) {
    mqttClient.end(true);
    mqttClient = null;
    mqttConnected = false;
  }

  console.log(`🔄 Connecting to MQTT broker: ${brokerUrl}`);

  mqttClient = mqtt.connect(brokerUrl, {
    reconnectPeriod: 5000,
    connectTimeout: 10000,
    clientId: 'iot-dashboard-' + Math.random().toString(16).substr(2, 8)
  });

  mqttClient.on('connect', () => {
    console.log('✅ MQTT connected!');
    mqttConnected = true;
    mqttClient.subscribe('esp32/#', (err) => {
      if (!err) console.log('📥 Subscribed to esp32/#');
    });
    io.emit('mqtt:status', { connected: true, broker: brokerUrl });
  });

  mqttClient.on('error', (err) => {
    console.error('❌ MQTT error:', err.message);
    mqttConnected = false;
    io.emit('mqtt:status', { connected: false, broker: brokerUrl, error: err.message });
  });

  mqttClient.on('offline', () => {
    console.log('⚠️ MQTT offline');
    mqttConnected = false;
    io.emit('mqtt:status', { connected: false, broker: brokerUrl });
  });

  mqttClient.on('reconnect', () => {
    console.log('🔄 MQTT reconnecting...');
  });

  mqttClient.on('close', () => {
    mqttConnected = false;
    io.emit('mqtt:status', { connected: false, broker: brokerUrl });
  });

  mqttClient.on('message', (topic, message) => {
    const payload = message.toString();
    handleMQTTMessage(topic, payload);
  });
}

// ============================================
// MQTT MESSAGE HANDLER
// ============================================
function handleMQTTMessage(topic, payload) {
  const parts = topic.split('/');
  // Expected formats:
  //   esp32/<mac>/data0        -> sensor data
  //   esp32/<mac>/data1        -> sensor data  
  //   esp32/<mac>/timestamp    -> timestamp
  //   esp32/<mac>/hops         -> hop count
  //   esp32/<mac>/ble/<name>/rssi -> BLE RSSI
  
  if (parts.length < 3 || parts[0] !== 'esp32') return;

  const mac = parts[1];
  const now = Date.now();

  // Ensure device entry exists
  if (!espDevices[mac]) {
    espDevices[mac] = { mac, data0: null, data1: null, timestamp: null, hops: null, lastSeen: now, online: true };
  }
  espDevices[mac].lastSeen = now;
  espDevices[mac].online = true;

  // BLE data: esp32/<mac>/ble/<deviceName>/rssi
  if (parts.length === 5 && parts[2] === 'ble' && parts[4] === 'rssi') {
    const deviceName = parts[3];
    const rssi = parseInt(payload);
    
    if (!bleDevices[deviceName]) {
      bleDevices[deviceName] = { name: deviceName, motes: {}, closestMote: null, lastSeen: now };
    }
    
    bleDevices[deviceName].motes[mac] = rssi;
    bleDevices[deviceName].lastSeen = now;
    
    // Calculate closest mote (highest RSSI = closest)
    let closestMac = null;
    let highestRssi = -Infinity;
    for (const [moteMac, moteRssi] of Object.entries(bleDevices[deviceName].motes)) {
      if (moteRssi > highestRssi) {
        highestRssi = moteRssi;
        closestMac = moteMac;
      }
    }
    bleDevices[deviceName].closestMote = closestMac;
    bleDevices[deviceName].closestRssi = highestRssi;
    
    io.emit('ble:update', bleDevices);
    return;
  }

  // Sensor data: esp32/<mac>/<field>
  if (parts.length === 3) {
    const field = parts[2];
    if (['data0', 'data1', 'data2', 'data3'].includes(field)) {
      espDevices[mac][field] = parseFloat(payload);
    } else if (field === 'timestamp') {
      espDevices[mac].timestamp = payload;
    } else if (field === 'hops') {
      espDevices[mac].hops = parseInt(payload);
    }
    
    io.emit('esp:update', espDevices);
  }
}

// ============================================
// CHECK DEVICE ONLINE STATUS (every 30s)
// ============================================
setInterval(() => {
  const now = Date.now();
  const TIMEOUT = 90000; // 90 seconds
  
  for (const mac in espDevices) {
    const wasOnline = espDevices[mac].online;
    espDevices[mac].online = (now - espDevices[mac].lastSeen) < TIMEOUT;
    if (wasOnline !== espDevices[mac].online) {
      io.emit('esp:update', espDevices);
    }
  }
}, 30000);

// ============================================
// EXPRESS MIDDLEWARE
// ============================================
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ============================================
// REST API - DEVICES
// ============================================
app.get('/api/devices', (req, res) => {
  res.json(loadDevices());
});

app.post('/api/devices', (req, res) => {
  const { mac, name, type } = req.body;
  if (!mac || !name) {
    return res.status(400).json({ error: 'MAC and name are required' });
  }
  
  const devices = loadDevices();
  
  // Check for duplicate MAC
  if (devices.find(d => d.mac.toLowerCase() === mac.toLowerCase())) {
    return res.status(409).json({ error: 'Device with this MAC already exists' });
  }
  
  const device = {
    mac: mac.toLowerCase(),
    name,
    type: type || 'mote',
    addedAt: new Date().toISOString()
  };
  
  devices.push(device);
  saveDevices(devices);
  
  io.emit('devices:update', devices);
  res.status(201).json(device);
});

app.delete('/api/devices/:mac', (req, res) => {
  const mac = req.params.mac.toLowerCase();
  let devices = loadDevices();
  const initial = devices.length;
  devices = devices.filter(d => d.mac.toLowerCase() !== mac);
  
  if (devices.length === initial) {
    return res.status(404).json({ error: 'Device not found' });
  }
  
  saveDevices(devices);
  io.emit('devices:update', devices);
  res.json({ success: true });
});

// ============================================
// REST API - SETTINGS
// ============================================
app.get('/api/settings', (req, res) => {
  const config = loadConfig();
  res.json({ ...config, mqttConnected });
});

app.post('/api/settings', (req, res) => {
  const { broker, port } = req.body;
  if (!broker) {
    return res.status(400).json({ error: 'Broker address is required' });
  }
  
  const config = loadConfig();
  config.mqtt.broker = broker;
  config.mqtt.port = parseInt(port) || 1883;
  saveConfig(config);
  
  // Reconnect MQTT with new settings
  console.log(`⚙️ Settings updated: ${broker}:${config.mqtt.port}`);
  connectMQTT();
  
  res.json({ success: true, config: config.mqtt });
});

// ============================================
// REST API - LIVE DATA
// ============================================
app.get('/api/esp-data', (req, res) => {
  res.json(espDevices);
});

app.get('/api/ble-data', (req, res) => {
  res.json(bleDevices);
});

app.get('/api/mqtt-status', (req, res) => {
  const config = loadConfig();
  res.json({
    connected: mqttConnected,
    broker: config.mqtt.broker,
    port: config.mqtt.port
  });
});

// ============================================
// SOCKET.IO
// ============================================
io.on('connection', (socket) => {
  console.log(`🔌 Client connected: ${socket.id}`);
  
  // Send current state to new client
  socket.emit('esp:update', espDevices);
  socket.emit('ble:update', bleDevices);
  socket.emit('mqtt:status', {
    connected: mqttConnected,
    broker: `mqtt://${loadConfig().mqtt.broker}:${loadConfig().mqtt.port}`
  });
  socket.emit('devices:update', loadDevices());
  
  socket.on('disconnect', () => {
    console.log(`🔌 Client disconnected: ${socket.id}`);
  });
});

// ============================================
// START SERVER
// ============================================
server.listen(PORT, () => {
  console.log(`\n╔════════════════════════════════════════╗`);
  console.log(`║    IoT MQTT Dashboard Server           ║`);
  console.log(`╚════════════════════════════════════════╝`);
  console.log(`🌐 Web: http://localhost:${PORT}`);
  console.log(`📡 Starting MQTT connection...`);
  connectMQTT();
});
