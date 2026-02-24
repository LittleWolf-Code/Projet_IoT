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
let bleDevices = {};      // deviceName -> { motes: { mac -> {rssi, timestamp} }, closestMote, lastSeen }
let blePositions = {};    // deviceName -> { ...pos, timestamp }
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

    bleDevices[deviceName].motes[mac] = { rssi, timestamp: now };
    bleDevices[deviceName].lastSeen = now;

    // Calculate closest mote (highest RSSI = closest)
    let closestMac = null;
    let highestRssi = -Infinity;
    for (const [moteMac, moteData] of Object.entries(bleDevices[deviceName].motes)) {
      if (moteData.rssi > highestRssi) {
        highestRssi = moteData.rssi;
        closestMac = moteMac;
      }
    }
    bleDevices[deviceName].closestMote = closestMac;
    bleDevices[deviceName].closestRssi = highestRssi;

    // Emit formatted data for frontend backwards compatibility
    const frontendBle = JSON.parse(JSON.stringify(bleDevices));
    for (const d in frontendBle) {
      for (const m in frontendBle[d].motes) {
        frontendBle[d].motes[m] = frontendBle[d].motes[m].rssi;
      }
    }
    io.emit('ble:update', frontendBle);
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
// POSITION LOGIC (Trilateration)
// ============================================
function dist(p1, p2) {
  return Math.sqrt(Math.pow(p1.x - p2.x, 2) + Math.pow(p1.y - p2.y, 2));
}

function estimateDistance(rssi) {
  if (rssi === 0) return -1.0;
  const txPower = -59;
  return Math.pow(10, (txPower - rssi) / (10 * 2.5));
}

function getTwoCirclesIntersections(c1, c2) {
  const d = dist(c1, c2);
  if (d > c1.r + c2.r || d < Math.abs(c1.r - c2.r) || (d === 0 && c1.r === c2.r)) {
    const ratio = c1.r / (c1.r + c2.r);
    return [{ x: c1.x + (c2.x - c1.x) * ratio, y: c1.y + (c2.y - c1.y) * ratio }];
  }
  const a = (c1.r * c1.r - c2.r * c2.r + d * d) / (2 * d);
  const hSq = c1.r * c1.r - a * a;
  const h = hSq > 0 ? Math.sqrt(hSq) : 0;
  const x2 = c1.x + a * (c2.x - c1.x) / d;
  const y2 = c1.y + a * (c2.y - c1.y) / d;

  return [
    { x: x2 + h * (c2.y - c1.y) / d, y: y2 - h * (c2.x - c1.x) / d },
    { x: x2 - h * (c2.y - c1.y) / d, y: y2 + h * (c2.x - c1.x) / d }
  ];
}

function trilaterate(p1, p2, p3) {
  const d = dist(p1, p2);
  if (d === 0) return null;
  const ex = { x: (p2.x - p1.x) / d, y: (p2.y - p1.y) / d };
  const ix = ex.x * (p3.x - p1.x) + ex.y * (p3.y - p1.y);
  const ey = { x: p3.x - p1.x - ix * ex.x, y: p3.y - p1.y - ix * ex.y };
  const eyDist = Math.sqrt(ey.x * ey.x + ey.y * ey.y);

  if (eyDist === 0) return null; // Collinear

  ey.x /= eyDist;
  ey.y /= eyDist;

  const j = ey.x * (p3.x - p1.x) + ey.y * (p3.y - p1.y);
  const x = (Math.pow(p1.r, 2) - Math.pow(p2.r, 2) + Math.pow(d, 2)) / (2 * d);
  const y = (Math.pow(p1.r, 2) - Math.pow(p3.r, 2) + Math.pow(ix, 2) + Math.pow(j, 2)) / (2 * j) - (ix * x) / j;

  return { x: p1.x + x * ex.x + y * ey.x, y: p1.y + x * ex.y + y * ey.y };
}

function calculatePositions() {
  const now = Date.now();
  const devices = loadDevices(); // Metadata x, y, z
  const espMap = {};
  devices.forEach(d => espMap[d.mac] = d);

  for (const [deviceName, bleData] of Object.entries(bleDevices)) {
    // 1. Filter motes detected in the last 60 seconds
    const recentMotes = [];
    let maxRssi = -Infinity;
    let bestFloor = 0;

    for (const [mac, moteData] of Object.entries(bleData.motes)) {
      if (now - moteData.timestamp <= 60000) {
        const espMeta = espMap[mac];
        if (espMeta && espMeta.x !== undefined && espMeta.y !== undefined) {
          recentMotes.push({ mac, rssi: moteData.rssi, x: espMeta.x, y: espMeta.y, z: espMeta.z || 0 });
          if (moteData.rssi > maxRssi) {
            maxRssi = moteData.rssi;
            bestFloor = espMeta.z || 0;
          }
        }
      } else {
        // Clean up old motes from memory
        delete bleData.motes[mac];
      }
    }

    if (recentMotes.length === 0) continue;

    // 2. Filter by the determined floor (Z)
    const floorMotes = recentMotes.filter(m => m.z === bestFloor);
    if (floorMotes.length === 0) continue;

    // Sort by RSSI to get the closest ones
    floorMotes.sort((a, b) => b.rssi - a.rssi);

    let finalPos = null;
    const historyPos = blePositions[deviceName];

    if (floorMotes.length === 1) {
      // Cas 1: 1 ESP
      finalPos = { x: floorMotes[0].x, y: floorMotes[0].y, z: bestFloor };
    } else if (floorMotes.length === 2) {
      // Cas 2: 2 ESPs -> Intersections
      const c1 = { ...floorMotes[0], r: estimateDistance(floorMotes[0].rssi) };
      const c2 = { ...floorMotes[1], r: estimateDistance(floorMotes[1].rssi) };
      const pts = getTwoCirclesIntersections(c1, c2);

      if (pts.length === 1 || !historyPos) {
        finalPos = { ...pts[0], z: bestFloor };
      } else {
        const d1 = dist(pts[0], historyPos);
        const d2 = dist(pts[1], historyPos);
        finalPos = d1 < d2 ? { ...pts[0], z: bestFloor } : { ...pts[1], z: bestFloor };
      }
    } else {
      // Cas 3 & 4: 3+ ESPs -> Trilateration
      const top3 = floorMotes.slice(0, 3).map(m => ({ ...m, r: estimateDistance(m.rssi) }));
      const triPos = trilaterate(top3[0], top3[1], top3[2]);

      if (triPos && !isNaN(triPos.x) && !isNaN(triPos.y)) {
        finalPos = { ...triPos, z: bestFloor };
      } else {
        // Fallback weighted average if collinear
        let wx = 0, wy = 0, sumWeight = 0;
        top3.forEach(m => { let w = 1 / m.r; wx += m.x * w; wy += m.y * w; sumWeight += w; });
        finalPos = { x: wx / sumWeight, y: wy / sumWeight, z: bestFloor };
      }
    }

    if (finalPos) {
      // Round coordinates to 2 decimals for cleaner output
      finalPos.x = Math.round(finalPos.x * 100) / 100;
      finalPos.y = Math.round(finalPos.y * 100) / 100;
      blePositions[deviceName] = { ...finalPos, timestamp: now };
    }
  }

  io.emit('positions:update', blePositions);
}

setInterval(() => {
  calculatePositions();
}, 30000);

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
  const { mac, name, type, x, y, z } = req.body;
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
    x: x !== undefined ? x : 0,
    y: y !== undefined ? y : 0,
    z: z !== undefined ? z : 0,
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

app.get('/api/positions', (req, res) => {
  res.json(blePositions);
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
