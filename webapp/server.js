const express = require('express');
const http = require('http');
const https = require('https');
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

const POSITION_HISTORY_LIMIT = 500;
const SCAN_BUFFER_LIMIT = 2000;

const DEFAULT_CONFIG = {
  mqtt: { broker: '172.18.32.43', port: 1883 },
  localization: {
    analysisEverySec: 30,
    windowSec: 60,
    txPower: -59,
    pathLossExponent: 2.0
  },
  map: {
    width: 1000,
    height: 700
  },
  zones: [],
  influx: {
    enabled: false,
    url: 'http://127.0.0.1:8086',
    org: '',
    bucket: '',
    token: ''
  }
};

let configCache = null;

// ============================================
// DATA STORES (in-memory)
// ============================================
let espDevices = {}; // MAC -> { data0, data1, timestamp, hops, lastSeen, online }
let bleDevices = {}; // deviceName -> { motes, moteLastSeen, closestMote, closestRssi, lastSeen }
let bleScanBuffer = {}; // deviceName -> [{ mac, rssi, timestamp }]
let devicePositions = {}; // deviceName -> { x, y, z, room, ... }
let deviceTracks = {}; // deviceName -> [{ x, y, z, room, timestamp }]
let deviceRoomState = {}; // deviceName -> { currentRoom, startTimestamp, stays[] }
let mqttConnected = false;
let mqttClient = null;
let localizationTimer = null;
let lastInfluxErrorAt = 0;

// ============================================
// CONFIG & DEVICES FILE HELPERS
// ============================================
function mergeDeep(base, override) {
  if (Array.isArray(base)) {
    return Array.isArray(override) ? override : base;
  }
  if (typeof base !== 'object' || base === null) {
    return override === undefined ? base : override;
  }

  const result = { ...base };
  const source = typeof override === 'object' && override !== null ? override : {};
  for (const key of Object.keys(source)) {
    if (!(key in base)) {
      result[key] = source[key];
      continue;
    }
    result[key] = mergeDeep(base[key], source[key]);
  }
  return result;
}

function ensureConfigDefaults(config) {
  const merged = mergeDeep(DEFAULT_CONFIG, config || {});

  merged.mqtt.port = Number(merged.mqtt.port) || 1883;
  merged.localization.analysisEverySec = Math.max(10, Number(merged.localization.analysisEverySec) || 30);
  merged.localization.windowSec = Math.max(20, Number(merged.localization.windowSec) || 60);
  merged.localization.txPower = Number(merged.localization.txPower);
  if (Number.isNaN(merged.localization.txPower)) merged.localization.txPower = -59;
  merged.localization.pathLossExponent = Number(merged.localization.pathLossExponent);
  if (Number.isNaN(merged.localization.pathLossExponent) || merged.localization.pathLossExponent <= 0) {
    merged.localization.pathLossExponent = 2.0;
  }

  merged.map.width = Math.max(100, Number(merged.map.width) || 1000);
  merged.map.height = Math.max(100, Number(merged.map.height) || 700);
  if (!Array.isArray(merged.zones)) merged.zones = [];

  return merged;
}

function loadConfig() {
  try {
    const parsed = JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
    const normalized = ensureConfigDefaults(parsed);
    fs.writeFileSync(CONFIG_PATH, JSON.stringify(normalized, null, 2));
    return normalized;
  } catch (e) {
    const defaults = ensureConfigDefaults(DEFAULT_CONFIG);
    fs.writeFileSync(CONFIG_PATH, JSON.stringify(defaults, null, 2));
    return defaults;
  }
}

function getConfig() {
  if (!configCache) {
    configCache = loadConfig();
  }
  return configCache;
}

function setConfig(nextConfig) {
  const normalized = ensureConfigDefaults(nextConfig);
  configCache = normalized;
  fs.writeFileSync(CONFIG_PATH, JSON.stringify(normalized, null, 2));
  return normalized;
}

function loadDevices() {
  try {
    const parsed = JSON.parse(fs.readFileSync(DEVICES_PATH, 'utf8'));
    return Array.isArray(parsed) ? parsed : [];
  } catch (e) {
    fs.writeFileSync(DEVICES_PATH, '[]');
    return [];
  }
}

function saveDevices(devices) {
  fs.writeFileSync(DEVICES_PATH, JSON.stringify(devices, null, 2));
}

function normalizeMac(mac) {
  return String(mac || '').trim().toLowerCase();
}

function parseOptionalNumber(value) {
  if (value === null || value === undefined || value === '') return null;
  const num = Number(value);
  return Number.isFinite(num) ? num : null;
}

function getAnchorsByMac() {
  const devices = loadDevices();
  const anchors = {};
  for (const d of devices) {
    const mac = normalizeMac(d.mac);
    const x = parseOptionalNumber(d.x);
    const y = parseOptionalNumber(d.y);
    const z = parseOptionalNumber(d.z);
    if (!mac || x === null || y === null || z === null) continue;
    anchors[mac] = {
      mac,
      name: d.name || mac,
      type: d.type || 'mote',
      x,
      y,
      z
    };
  }
  return anchors;
}

// ============================================
// INFLUXDB (OPTIONAL)
// ============================================
function escapeInfluxTag(v) {
  return String(v).replace(/\\/g, '\\\\').replace(/,/g, '\\,').replace(/ /g, '\\ ').replace(/=/g, '\\=');
}

function escapeInfluxFieldString(v) {
  return String(v).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

function writeInfluxLine(line) {
  const cfg = getConfig();
  const influx = cfg.influx || {};
  if (!influx.enabled) return;
  if (!influx.url || !influx.org || !influx.bucket || !influx.token) return;

  let target;
  try {
    const base = influx.url.endsWith('/') ? influx.url.slice(0, -1) : influx.url;
    target = new URL(`${base}/api/v2/write?org=${encodeURIComponent(influx.org)}&bucket=${encodeURIComponent(influx.bucket)}&precision=ms`);
  } catch (err) {
    return;
  }

  const client = target.protocol === 'https:' ? https : http;
  const req = client.request({
    method: 'POST',
    hostname: target.hostname,
    port: target.port || (target.protocol === 'https:' ? 443 : 80),
    path: `${target.pathname}${target.search}`,
    headers: {
      Authorization: `Token ${influx.token}`,
      'Content-Type': 'text/plain; charset=utf-8',
      'Content-Length': Buffer.byteLength(line)
    }
  }, (res) => {
    if (res.statusCode && res.statusCode >= 300) {
      const now = Date.now();
      if (now - lastInfluxErrorAt > 30000) {
        lastInfluxErrorAt = now;
        console.error(`InfluxDB write failed with status ${res.statusCode}`);
      }
    }
    res.resume();
  });

  req.on('error', () => {
    const now = Date.now();
    if (now - lastInfluxErrorAt > 30000) {
      lastInfluxErrorAt = now;
      console.error('InfluxDB write failed (network error)');
    }
  });

  req.write(line);
  req.end();
}

function writeBleScanToInflux(deviceName, espMac, rssi, timestamp) {
  const line = `ble_scan,device=${escapeInfluxTag(deviceName)},esp=${escapeInfluxTag(espMac)} rssi=${Number(rssi)}i ${timestamp}`;
  writeInfluxLine(line);
}

function writePositionToInflux(position) {
  const room = position.room || 'Hors zone';
  const line = `device_position,device=${escapeInfluxTag(position.name)} x=${position.x},y=${position.y},z=${position.z},method="${escapeInfluxFieldString(position.method)}",room="${escapeInfluxFieldString(room)}" ${position.updatedAt}`;
  writeInfluxLine(line);
}

// ============================================
// MQTT CONNECTION
// ============================================
function connectMQTT() {
  const config = getConfig();
  const brokerUrl = `mqtt://${config.mqtt.broker}:${config.mqtt.port}`;

  if (mqttClient) {
    mqttClient.end(true);
    mqttClient = null;
    mqttConnected = false;
  }

  console.log(`Connecting to MQTT broker: ${brokerUrl}`);

  mqttClient = mqtt.connect(brokerUrl, {
    reconnectPeriod: 5000,
    connectTimeout: 10000,
    clientId: `iot-dashboard-${Math.random().toString(16).slice(2, 10)}`
  });

  mqttClient.on('connect', () => {
    console.log('MQTT connected');
    mqttConnected = true;
    mqttClient.subscribe('esp32/#', (err) => {
      if (!err) console.log('Subscribed to esp32/#');
    });
    io.emit('mqtt:status', { connected: true, broker: brokerUrl });
  });

  mqttClient.on('error', (err) => {
    console.error('MQTT error:', err.message);
    mqttConnected = false;
    io.emit('mqtt:status', { connected: false, broker: brokerUrl, error: err.message });
  });

  mqttClient.on('offline', () => {
    mqttConnected = false;
    io.emit('mqtt:status', { connected: false, broker: brokerUrl });
  });

  mqttClient.on('reconnect', () => {
    console.log('MQTT reconnecting...');
  });

  mqttClient.on('close', () => {
    mqttConnected = false;
    io.emit('mqtt:status', { connected: false, broker: brokerUrl });
  });

  mqttClient.on('message', (topic, message) => {
    handleMQTTMessage(topic, message.toString());
  });
}

// ============================================
// LOCALIZATION LOGIC
// ============================================
function distanceFromRssi(rssi, txPower, pathLossExponent) {
  if (!Number.isFinite(rssi)) return null;
  const distance = Math.pow(10, (txPower - rssi) / (10 * pathLossExponent));
  return Math.max(0.5, Math.min(40, distance));
}

function pointDistance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y);
}

function weightedCentroid(anchorDistances) {
  let sumWeight = 0;
  let sumX = 0;
  let sumY = 0;
  for (const ad of anchorDistances) {
    const weight = 1 / Math.max(ad.d, 0.5);
    sumWeight += weight;
    sumX += ad.x * weight;
    sumY += ad.y * weight;
  }
  if (sumWeight === 0) return null;
  return { x: sumX / sumWeight, y: sumY / sumWeight };
}

function circleIntersections(c1, r1, c2, r2) {
  const d = pointDistance(c1, c2);
  if (d === 0) return [];
  if (d > r1 + r2) return [];
  if (d < Math.abs(r1 - r2)) return [];

  const a = (r1 * r1 - r2 * r2 + d * d) / (2 * d);
  const hSq = r1 * r1 - a * a;
  if (hSq < 0) return [];
  const h = Math.sqrt(hSq);

  const x2 = c1.x + (a * (c2.x - c1.x)) / d;
  const y2 = c1.y + (a * (c2.y - c1.y)) / d;

  const rx = -((c2.y - c1.y) * (h / d));
  const ry = (c2.x - c1.x) * (h / d);

  const p1 = { x: x2 + rx, y: y2 + ry };
  const p2 = { x: x2 - rx, y: y2 - ry };

  if (Math.abs(p1.x - p2.x) < 1e-8 && Math.abs(p1.y - p2.y) < 1e-8) {
    return [p1];
  }
  return [p1, p2];
}

function bestTwoAnchorPoint(a1, a2, previous) {
  const intersections = circleIntersections({ x: a1.x, y: a1.y }, a1.d, { x: a2.x, y: a2.y }, a2.d);
  if (intersections.length === 1) return intersections[0];
  if (intersections.length === 2) {
    if (previous) {
      const d0 = pointDistance(intersections[0], previous);
      const d1 = pointDistance(intersections[1], previous);
      return d0 <= d1 ? intersections[0] : intersections[1];
    }
    return intersections[0];
  }

  // Fallback when circles do not intersect.
  const total = a1.d + a2.d;
  const t = total > 0 ? Math.min(0.9, Math.max(0.1, a1.d / total)) : 0.5;
  return {
    x: a1.x + (a2.x - a1.x) * t,
    y: a1.y + (a2.y - a1.y) * t
  };
}

function trilaterate3(a1, a2, a3) {
  const A = 2 * (a2.x - a1.x);
  const B = 2 * (a2.y - a1.y);
  const C = (a1.d * a1.d - a2.d * a2.d) - (a1.x * a1.x - a2.x * a2.x) - (a1.y * a1.y - a2.y * a2.y);

  const D = 2 * (a3.x - a1.x);
  const E = 2 * (a3.y - a1.y);
  const F = (a1.d * a1.d - a3.d * a3.d) - (a1.x * a1.x - a3.x * a3.x) - (a1.y * a1.y - a3.y * a3.y);

  const det = A * E - B * D;
  if (Math.abs(det) < 1e-6) {
    return weightedCentroid([a1, a2, a3]);
  }

  return {
    x: (C * E - B * F) / det,
    y: (A * F - C * D) / det
  };
}

function pointInPolygon(point, polygon) {
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const xi = polygon[i].x;
    const yi = polygon[i].y;
    const xj = polygon[j].x;
    const yj = polygon[j].y;
    const intersect = ((yi > point.y) !== (yj > point.y)) &&
      (point.x < ((xj - xi) * (point.y - yi)) / (yj - yi + 1e-9) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

function findZoneName(x, y, z, zones) {
  for (const zone of zones) {
    if (Number(zone.z) !== Number(z)) continue;
    if (!Array.isArray(zone.points) || zone.points.length < 3) continue;
    if (pointInPolygon({ x, y }, zone.points)) {
      return zone.name || null;
    }
  }
  return null;
}

function updateRoomHistory(deviceName, roomName, timestamp) {
  const normalizedRoom = roomName || 'Hors zone';
  if (!deviceRoomState[deviceName]) {
    deviceRoomState[deviceName] = {
      currentRoom: normalizedRoom,
      startTimestamp: timestamp,
      stays: []
    };
    return;
  }

  const state = deviceRoomState[deviceName];
  if (state.currentRoom === normalizedRoom) return;

  state.stays.push({
    room: state.currentRoom,
    start: state.startTimestamp,
    end: timestamp,
    durationSec: Math.max(0, Math.round((timestamp - state.startTimestamp) / 1000))
  });

  state.currentRoom = normalizedRoom;
  state.startTimestamp = timestamp;
}

function appendTrackPoint(deviceName, position) {
  if (!deviceTracks[deviceName]) deviceTracks[deviceName] = [];
  deviceTracks[deviceName].push({
    x: position.x,
    y: position.y,
    z: position.z,
    room: position.room || null,
    timestamp: position.updatedAt
  });
  if (deviceTracks[deviceName].length > POSITION_HISTORY_LIMIT) {
    deviceTracks[deviceName] = deviceTracks[deviceName].slice(-POSITION_HISTORY_LIMIT);
  }
}

function collectWindowReadings(deviceName, cutoff, anchorsByMac) {
  const scans = bleScanBuffer[deviceName] || [];
  const grouped = {};

  for (let i = scans.length - 1; i >= 0; i -= 1) {
    const scan = scans[i];
    if (scan.timestamp < cutoff) break;
    const mac = normalizeMac(scan.mac);
    if (!anchorsByMac[mac]) continue;
    if (!grouped[mac]) {
      grouped[mac] = { sum: 0, count: 0, latest: scan.timestamp };
    }
    grouped[mac].sum += scan.rssi;
    grouped[mac].count += 1;
    if (scan.timestamp > grouped[mac].latest) grouped[mac].latest = scan.timestamp;
  }

  const readings = [];
  for (const [mac, stats] of Object.entries(grouped)) {
    readings.push({
      anchor: anchorsByMac[mac],
      rssi: stats.sum / stats.count,
      latest: stats.latest
    });
  }
  return readings;
}

function runLocalizationAnalysis() {
  const config = getConfig();
  const anchorsByMac = getAnchorsByMac();
  const zones = config.zones || [];
  const now = Date.now();
  const windowMs = config.localization.windowSec * 1000;
  const cutoff = now - windowMs;
  const cleanupCutoff = now - Math.max(windowMs * 4, 120000);

  let changed = false;

  for (const [deviceName, scans] of Object.entries(bleScanBuffer)) {
    if (!Array.isArray(scans) || scans.length === 0) continue;

    bleScanBuffer[deviceName] = scans.filter((s) => s.timestamp >= cleanupCutoff).slice(-SCAN_BUFFER_LIMIT);
    const readings = collectWindowReadings(deviceName, cutoff, anchorsByMac);
    if (readings.length === 0) continue;

    const floorReading = readings.reduce((best, current) => (
      !best || current.rssi > best.rssi ? current : best
    ), null);
    if (!floorReading) continue;
    const z = floorReading.anchor.z;

    let sameFloor = readings
      .filter((r) => Number(r.anchor.z) === Number(z))
      .sort((a, b) => b.rssi - a.rssi);
    if (sameFloor.length === 0) continue;

    if (sameFloor.length > 3) {
      sameFloor = sameFloor.slice(0, 3);
    }

    const withDistances = [];
    for (const r of sameFloor) {
      const d = distanceFromRssi(r.rssi, config.localization.txPower, config.localization.pathLossExponent);
      if (!Number.isFinite(d)) continue;
      withDistances.push({
        mac: r.anchor.mac,
        name: r.anchor.name,
        x: r.anchor.x,
        y: r.anchor.y,
        z: r.anchor.z,
        rssi: Number(r.rssi.toFixed(2)),
        d
      });
    }
    if (withDistances.length === 0) continue;

    const previous = devicePositions[deviceName];
    let method = 'single_anchor';
    let point = null;

    if (withDistances.length === 1) {
      point = { x: withDistances[0].x, y: withDistances[0].y };
      method = 'single_anchor';
    } else if (withDistances.length === 2) {
      point = bestTwoAnchorPoint(withDistances[0], withDistances[1], previous);
      method = 'two_anchor_intersection';
    } else {
      point = trilaterate3(withDistances[0], withDistances[1], withDistances[2]);
      method = 'trilateration';
    }

    if (!point) continue;

    const x = Math.max(0, Math.min(config.map.width, Number(point.x.toFixed(2))));
    const y = Math.max(0, Math.min(config.map.height, Number(point.y.toFixed(2))));
    const room = findZoneName(x, y, z, zones);

    const nextPosition = {
      name: deviceName,
      x,
      y,
      z,
      room,
      method,
      anchors: withDistances.map((a) => ({
        mac: a.mac,
        name: a.name,
        rssi: a.rssi,
        distance: Number(a.d.toFixed(2))
      })),
      updatedAt: now
    };

    devicePositions[deviceName] = nextPosition;
    appendTrackPoint(deviceName, nextPosition);
    updateRoomHistory(deviceName, room, now);
    writePositionToInflux(nextPosition);
    changed = true;
  }

  // Clear stale BLE mote readings from UI objects.
  for (const device of Object.values(bleDevices)) {
    if (!device.moteLastSeen) continue;
    for (const [mac, ts] of Object.entries(device.moteLastSeen)) {
      if (ts < cutoff) {
        delete device.motes[mac];
        delete device.moteLastSeen[mac];
      }
    }
  }

  for (const [deviceName, pos] of Object.entries(devicePositions)) {
    if ((pos.updatedAt || 0) < cutoff) {
      delete devicePositions[deviceName];
      changed = true;
    }
  }

  if (changed) {
    io.emit('position:update', devicePositions);
  }
}

function startLocalizationLoop() {
  const intervalMs = getConfig().localization.analysisEverySec * 1000;
  if (localizationTimer) clearInterval(localizationTimer);
  localizationTimer = setInterval(runLocalizationAnalysis, intervalMs);
}

// ============================================
// MQTT MESSAGE HANDLER
// ============================================
function handleMQTTMessage(topic, payload) {
  const parts = topic.split('/');
  // Expected formats:
  //   esp32/<mac>/data0
  //   esp32/<mac>/data1
  //   esp32/<mac>/timestamp
  //   esp32/<mac>/hops
  //   esp32/<mac>/ble/<name>/rssi
  if (parts.length < 3 || parts[0] !== 'esp32') return;

  const mac = normalizeMac(parts[1]);
  const now = Date.now();

  if (!espDevices[mac]) {
    espDevices[mac] = {
      mac,
      data0: null,
      data1: null,
      timestamp: null,
      hops: null,
      lastSeen: now,
      online: true
    };
  }
  espDevices[mac].lastSeen = now;
  espDevices[mac].online = true;

  if (parts.length === 5 && parts[2] === 'ble' && parts[4] === 'rssi') {
    const deviceName = parts[3];
    const rssi = parseInt(payload, 10);
    if (Number.isNaN(rssi)) return;

    if (!bleDevices[deviceName]) {
      bleDevices[deviceName] = {
        name: deviceName,
        motes: {},
        moteLastSeen: {},
        closestMote: null,
        closestRssi: null,
        lastSeen: now
      };
    }

    bleDevices[deviceName].motes[mac] = rssi;
    bleDevices[deviceName].moteLastSeen[mac] = now;
    bleDevices[deviceName].lastSeen = now;

    if (!bleScanBuffer[deviceName]) bleScanBuffer[deviceName] = [];
    bleScanBuffer[deviceName].push({ mac, rssi, timestamp: now });
    if (bleScanBuffer[deviceName].length > SCAN_BUFFER_LIMIT) {
      bleScanBuffer[deviceName] = bleScanBuffer[deviceName].slice(-SCAN_BUFFER_LIMIT);
    }

    let closestMac = null;
    let highestRssi = -Infinity;
    for (const [moteMac, moteRssi] of Object.entries(bleDevices[deviceName].motes)) {
      if (moteRssi > highestRssi) {
        highestRssi = moteRssi;
        closestMac = moteMac;
      }
    }
    bleDevices[deviceName].closestMote = closestMac;
    bleDevices[deviceName].closestRssi = Number.isFinite(highestRssi) ? highestRssi : null;

    writeBleScanToInflux(deviceName, mac, rssi, now);
    io.emit('ble:update', bleDevices);
    return;
  }

  if (parts.length === 3) {
    const field = parts[2];
    if (['data0', 'data1', 'data2', 'data3'].includes(field)) {
      espDevices[mac][field] = parseFloat(payload);
    } else if (field === 'timestamp') {
      espDevices[mac].timestamp = payload;
    } else if (field === 'hops') {
      espDevices[mac].hops = parseInt(payload, 10);
    }

    io.emit('esp:update', espDevices);
  }
}

// ============================================
// CHECK DEVICE ONLINE STATUS (every 30s)
// ============================================
setInterval(() => {
  const now = Date.now();
  const timeoutMs = 90000;
  let changed = false;

  for (const mac in espDevices) {
    const wasOnline = espDevices[mac].online;
    espDevices[mac].online = (now - espDevices[mac].lastSeen) < timeoutMs;
    if (wasOnline !== espDevices[mac].online) changed = true;
  }

  if (changed) {
    io.emit('esp:update', espDevices);
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

  const normalizedMac = normalizeMac(mac);
  const devices = loadDevices();
  if (devices.find((d) => normalizeMac(d.mac) === normalizedMac)) {
    return res.status(409).json({ error: 'Device with this MAC already exists' });
  }

  const device = {
    mac: normalizedMac,
    name,
    type: type || 'mote',
    x: parseOptionalNumber(x),
    y: parseOptionalNumber(y),
    z: parseOptionalNumber(z),
    addedAt: new Date().toISOString()
  };

  devices.push(device);
  saveDevices(devices);
  io.emit('devices:update', devices);
  res.status(201).json(device);
});

app.delete('/api/devices/:mac', (req, res) => {
  const mac = normalizeMac(req.params.mac);
  let devices = loadDevices();
  const initial = devices.length;
  devices = devices.filter((d) => normalizeMac(d.mac) !== mac);

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
  const config = getConfig();
  const safeConfig = {
    ...config,
    influx: {
      ...config.influx,
      token: config.influx && config.influx.token ? '***' : ''
    }
  };
  res.json({ ...safeConfig, mqttConnected });
});

app.post('/api/settings', (req, res) => {
  const { broker, port } = req.body;
  if (!broker) {
    return res.status(400).json({ error: 'Broker address is required' });
  }

  const config = getConfig();
  config.mqtt.broker = String(broker).trim();
  config.mqtt.port = Number(port) || 1883;
  setConfig(config);

  connectMQTT();
  res.json({ success: true, config: config.mqtt });
});

app.post('/api/localization/settings', (req, res) => {
  const { analysisEverySec, windowSec, txPower, pathLossExponent } = req.body;
  const config = getConfig();

  if (analysisEverySec !== undefined) config.localization.analysisEverySec = analysisEverySec;
  if (windowSec !== undefined) config.localization.windowSec = windowSec;
  if (txPower !== undefined) config.localization.txPower = txPower;
  if (pathLossExponent !== undefined) config.localization.pathLossExponent = pathLossExponent;

  const saved = setConfig(config);
  startLocalizationLoop();
  res.json({ success: true, localization: saved.localization });
});

app.post('/api/influx/settings', (req, res) => {
  const { enabled, url, org, bucket, token } = req.body;
  const config = getConfig();
  if (enabled !== undefined) config.influx.enabled = Boolean(enabled);
  if (url !== undefined) config.influx.url = String(url);
  if (org !== undefined) config.influx.org = String(org);
  if (bucket !== undefined) config.influx.bucket = String(bucket);
  if (token !== undefined) config.influx.token = String(token);
  const saved = setConfig(config);
  res.json({ success: true, influx: { ...saved.influx, token: saved.influx.token ? '***' : '' } });
});

// ============================================
// REST API - ZONES / MAP / HISTORY
// ============================================
app.get('/api/map-state', (req, res) => {
  const config = getConfig();
  res.json({
    map: config.map,
    zones: config.zones || [],
    localization: config.localization,
    anchors: Object.values(getAnchorsByMac()),
    positions: devicePositions
  });
});

app.get('/api/positions', (req, res) => {
  res.json(devicePositions);
});

app.post('/api/zones', (req, res) => {
  const { id, name, z, points, x, y, width, height, color } = req.body;
  if (!name) return res.status(400).json({ error: 'Zone name is required' });
  const floor = Number(z);
  if (!Number.isFinite(floor)) return res.status(400).json({ error: 'Zone floor (z) is required' });

  let normalizedPoints = [];
  if (Array.isArray(points) && points.length >= 3) {
    normalizedPoints = points
      .map((p) => ({ x: Number(p.x), y: Number(p.y) }))
      .filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y));
  } else {
    const rx = Number(x);
    const ry = Number(y);
    const rw = Number(width);
    const rh = Number(height);
    if (![rx, ry, rw, rh].every(Number.isFinite)) {
      return res.status(400).json({ error: 'Provide points[] or rectangle x/y/width/height' });
    }
    normalizedPoints = [
      { x: rx, y: ry },
      { x: rx + rw, y: ry },
      { x: rx + rw, y: ry + rh },
      { x: rx, y: ry + rh }
    ];
  }

  if (normalizedPoints.length < 3) {
    return res.status(400).json({ error: 'Zone needs at least 3 points' });
  }

  const zoneId = id || `zone-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
  const zone = {
    id: zoneId,
    name: String(name),
    z: floor,
    color: color || '#3b82f6',
    points: normalizedPoints
  };

  const config = getConfig();
  const zones = Array.isArray(config.zones) ? config.zones : [];
  const existingIdx = zones.findIndex((zo) => zo.id === zoneId);
  if (existingIdx >= 0) zones[existingIdx] = zone;
  else zones.push(zone);
  config.zones = zones;
  const saved = setConfig(config);

  io.emit('zones:update', saved.zones);
  res.json({ success: true, zone });
});

app.delete('/api/zones/:id', (req, res) => {
  const zoneId = req.params.id;
  const config = getConfig();
  const initial = (config.zones || []).length;
  config.zones = (config.zones || []).filter((z) => z.id !== zoneId);
  if (config.zones.length === initial) {
    return res.status(404).json({ error: 'Zone not found' });
  }
  const saved = setConfig(config);
  io.emit('zones:update', saved.zones);
  res.json({ success: true });
});

app.get('/api/device-history/:deviceName', (req, res) => {
  const deviceName = req.params.deviceName;
  const state = deviceRoomState[deviceName];
  const now = Date.now();
  const stays = state ? [...state.stays] : [];
  if (state) {
    stays.push({
      room: state.currentRoom,
      start: state.startTimestamp,
      end: now,
      durationSec: Math.max(0, Math.round((now - state.startTimestamp) / 1000)),
      ongoing: true
    });
  }
  res.json({
    device: deviceName,
    currentPosition: devicePositions[deviceName] || null,
    stays,
    points: deviceTracks[deviceName] || []
  });
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
  const config = getConfig();
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
  console.log(`Client connected: ${socket.id}`);
  const config = getConfig();

  socket.emit('esp:update', espDevices);
  socket.emit('ble:update', bleDevices);
  socket.emit('position:update', devicePositions);
  socket.emit('zones:update', config.zones || []);
  socket.emit('mqtt:status', {
    connected: mqttConnected,
    broker: `mqtt://${config.mqtt.broker}:${config.mqtt.port}`
  });
  socket.emit('devices:update', loadDevices());

  socket.on('disconnect', () => {
    console.log(`Client disconnected: ${socket.id}`);
  });
});

// ============================================
// START SERVER
// ============================================
server.listen(PORT, () => {
  console.log(`Web: http://localhost:${PORT}`);
  configCache = loadConfig();
  startLocalizationLoop();
  connectMQTT();
});
