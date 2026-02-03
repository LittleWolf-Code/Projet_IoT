// ============================================
// BLE LOCALIZATION SYSTEM - Main Application
// ============================================

// Global State
const state = {
    mqtt: {
        client: null,
        connected: false,
        broker: 'sink',
        port: 8883,
        protocol: 'wss'
    },
    floors: [
        { name: 'ISIS 10A', pdf: null, image: null },
        { name: 'ISIS R+1', pdf: null, image: null }
    ],
    currentFloor: 0,
    motes: [],
    devices: {},
    canvasMode: 'place-motes',
    selectedMote: null,
    rssiParams: {
        rssiAt1m: -59,
        pathLoss: 2.5
    }
};

// Canvas
let canvas, ctx;

// ============================================
// INITIALIZATION
// ============================================

document.addEventListener('DOMContentLoaded', () => {
    initializeCanvas();
    initializeEventListeners();
    loadConfiguration();
    setupPDFJS();
});

function setupPDFJS() {
    // Set PDF.js worker
    pdfjsLib.GlobalWorkerOptions.workerSrc = 'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/3.11.174/pdf.worker.min.js';
}

function initializeCanvas() {
    canvas = document.getElementById('localization-canvas');
    ctx = canvas.getContext('2d');
    drawCanvas();
}

function initializeEventListeners() {
    // MQTT Connection
    document.getElementById('mqtt-connect-btn').addEventListener('click', toggleMQTTConnection);
    
    // ESP Mote Management
    document.getElementById('add-mote-btn').addEventListener('click', addMote);
    
    // Floor Selection
    document.querySelectorAll('.floor-btn').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const floor = parseInt(e.target.dataset.floor);
            switchFloor(floor);
        });
    });
    
    // Canvas Interaction
    canvas.addEventListener('click', handleCanvasClick);
    canvas.addEventListener('mousemove', handleCanvasHover);
    
    // Canvas Mode
    document.getElementById('canvas-mode').addEventListener('change', (e) => {
        state.canvasMode = e.target.value;
        drawCanvas();
    });
    
    // PDF Upload
    document.getElementById('upload-area-0').addEventListener('click', () => {
        document.getElementById('pdf-input-0').click();
    });
    document.getElementById('upload-area-1').addEventListener('click', () => {
        document.getElementById('pdf-input-1').click();
    });
    
    document.getElementById('pdf-input-0').addEventListener('change', (e) => loadPDF(e, 0));
    document.getElementById('pdf-input-1').addEventListener('change', (e) => loadPDF(e, 1));
    
    // Configuration
    document.getElementById('save-config-btn').addEventListener('click', saveConfiguration);
    document.getElementById('clear-canvas-btn').addEventListener('click', clearCanvas);
    
    // RSSI Parameters
    document.getElementById('rssi-at-1m').addEventListener('change', (e) => {
        state.rssiParams.rssiAt1m = parseFloat(e.target.value);
    });
    document.getElementById('path-loss').addEventListener('change', (e) => {
        state.rssiParams.pathLoss = parseFloat(e.target.value);
    });
    
    // Device Filter
    document.getElementById('device-filter').addEventListener('input', filterDevices);
}

// ============================================
// PDF HANDLING
// ============================================

async function loadPDF(event, floorIndex) {
    const file = event.target.files[0];
    if (!file) return;
    
    const uploadArea = document.getElementById(`upload-area-${floorIndex}`);
    uploadArea.classList.add('has-file');
    uploadArea.querySelector('.upload-text').textContent = file.name;
    
    const fileReader = new FileReader();
    fileReader.onload = async function() {
        const typedarray = new Uint8Array(this.result);
        
        try {
            const pdf = await pdfjsLib.getDocument(typedarray).promise;
            const page = await pdf.getPage(1);
            
            const viewport = page.getViewport({ scale: 2 });
            const tempCanvas = document.createElement('canvas');
            const tempCtx = tempCanvas.getContext('2d');
            
            tempCanvas.width = viewport.width;
            tempCanvas.height = viewport.height;
            
            await page.render({
                canvasContext: tempCtx,
                viewport: viewport
            }).promise;
            
            const img = new Image();
            img.onload = () => {
                state.floors[floorIndex].image = img;
                if (floorIndex === state.currentFloor) {
                    drawCanvas();
                }
            };
            img.src = tempCanvas.toDataURL();
            
        } catch (error) {
            console.error('Erreur lors du chargement du PDF:', error);
            alert('Erreur lors du chargement du PDF. Veuillez réessayer.');
        }
    };
    fileReader.readAsArrayBuffer(file);
}

// ============================================
// MQTT CONNECTION
// ============================================

function toggleMQTTConnection() {
    if (state.mqtt.connected) {
        disconnectMQTT();
    } else {
        connectMQTT();
    }
}

function connectMQTT() {
    const broker = document.getElementById('mqtt-broker').value;
    const port = document.getElementById('mqtt-port').value;
    const protocol = document.getElementById('mqtt-protocol').value;
    const username = document.getElementById('mqtt-username').value;
    const password = document.getElementById('mqtt-password').value;
    
    const url = `${protocol}://${broker}:${port}`;
    
    const options = {
        clientId: 'ble_localization_' + Math.random().toString(16).substr(2, 8),
        clean: true,
        reconnectPeriod: 1000,
    };
    
    if (username) {
        options.username = username;
        options.password = password;
    }
    
    try {
        state.mqtt.client = mqtt.connect(url, options);
        
        state.mqtt.client.on('connect', () => {
            console.log('Connecté au broker MQTT');
            state.mqtt.connected = true;
            updateConnectionStatus(true);
            
            // Subscribe to all ESP mote topics
            const topic = 'esp/+/ble/+/rssi';
            state.mqtt.client.subscribe(topic, (err) => {
                if (err) {
                    console.error('Erreur de souscription:', err);
                } else {
                    console.log('Souscrit au topic:', topic);
                }
            });
        });
        
        state.mqtt.client.on('message', handleMQTTMessage);
        
        state.mqtt.client.on('error', (error) => {
            console.error('Erreur MQTT:', error);
            alert('Erreur de connexion MQTT: ' + error.message);
            updateConnectionStatus(false);
        });
        
        state.mqtt.client.on('close', () => {
            console.log('Connexion MQTT fermée');
            state.mqtt.connected = false;
            updateConnectionStatus(false);
        });
        
    } catch (error) {
        console.error('Erreur de connexion:', error);
        alert('Impossible de se connecter au broker MQTT');
    }
}

function disconnectMQTT() {
    if (state.mqtt.client) {
        state.mqtt.client.end();
        state.mqtt.client = null;
        state.mqtt.connected = false;
        updateConnectionStatus(false);
    }
}

function updateConnectionStatus(connected) {
    const indicator = document.getElementById('status-indicator');
    const text = document.getElementById('status-text');
    const btn = document.getElementById('mqtt-connect-btn');
    
    if (connected) {
        indicator.classList.add('connected');
        text.textContent = 'Connecté';
        btn.textContent = '🔌 Déconnecter';
        btn.classList.remove('btn-primary');
        btn.classList.add('btn-danger');
    } else {
        indicator.classList.remove('connected');
        text.textContent = 'Déconnecté';
        btn.textContent = '🔌 Connecter au Broker';
        btn.classList.remove('btn-danger');
        btn.classList.add('btn-primary');
    }
}

// ============================================
// MQTT MESSAGE HANDLING
// ============================================

function handleMQTTMessage(topic, message) {
    // Topic format: esp/mac_address/ble/devicename/rssi
    const parts = topic.split('/');
    
    if (parts.length !== 5 || parts[0] !== 'esp' || parts[2] !== 'ble' || parts[4] !== 'rssi') {
        console.warn('Format de topic invalide:', topic);
        return;
    }
    
    const moteMac = parts[1];
    const deviceName = parts[3];
    const rssi = parseInt(message.toString());
    
    // Find the mote
    const mote = state.motes.find(m => m.mac.toLowerCase() === moteMac.toLowerCase());
    
    if (!mote) {
        console.warn('ESP Mote non trouvé:', moteMac);
        return;
    }
    
    if (!mote.position) {
        console.warn('ESP Mote sans position:', moteMac);
        return;
    }
    
    // Update device data
    if (!state.devices[deviceName]) {
        state.devices[deviceName] = {
            name: deviceName,
            rssiData: {},
            position: null,
            lastUpdate: Date.now()
        };
    }
    
    state.devices[deviceName].rssiData[moteMac] = {
        rssi: rssi,
        distance: rssiToDistance(rssi),
        timestamp: Date.now()
    };
    state.devices[deviceName].lastUpdate = Date.now();
    
    // Calculate position using trilateration
    calculateDevicePosition(deviceName);
    
    // Update UI
    updateDeviceList();
    drawCanvas();
}

// ============================================
// RSSI TO DISTANCE CONVERSION
// ============================================

function rssiToDistance(rssi) {
    // Formula: distance = 10 ^ ((RSSI_measured - RSSI_at_1m) / (10 * n))
    const { rssiAt1m, pathLoss } = state.rssiParams;
    const distance = Math.pow(10, (rssiAt1m - rssi) / (10 * pathLoss));
    return distance;
}

// ============================================
// TRILATERATION ALGORITHM
// ============================================

function calculateDevicePosition(deviceName) {
    const device = state.devices[deviceName];
    const now = Date.now();
    
    // Filter valid RSSI data (less than 5 seconds old)
    const validData = [];
    
    for (const [moteMac, data] of Object.entries(device.rssiData)) {
        if (now - data.timestamp < 5000) {
            const mote = state.motes.find(m => m.mac.toLowerCase() === moteMac.toLowerCase());
            if (mote && mote.position && mote.floor === state.currentFloor) {
                validData.push({
                    x: mote.position.x,
                    y: mote.position.y,
                    distance: data.distance
                });
            }
        }
    }
    
    if (validData.length < 3) {
        // Not enough data for trilateration
        device.position = null;
        return;
    }
    
    // Use least squares trilateration
    const position = trilaterate(validData);
    device.position = position;
}

function trilaterate(points) {
    // Least squares trilateration
    // Using the first point as reference
    const p1 = points[0];
    
    let A = [];
    let b = [];
    
    for (let i = 1; i < points.length; i++) {
        const p = points[i];
        A.push([2 * (p.x - p1.x), 2 * (p.y - p1.y)]);
        b.push([
            p.x * p.x - p1.x * p1.x + 
            p.y * p.y - p1.y * p1.y + 
            p1.distance * p1.distance - p.distance * p.distance
        ]);
    }
    
    // Solve using least squares (simplified for 2D)
    try {
        const result = leastSquares(A, b);
        return { x: result[0], y: result[1] };
    } catch (e) {
        console.error('Erreur de trilatération:', e);
        return null;
    }
}

function leastSquares(A, b) {
    // Simplified least squares solver for 2x2 system
    if (A.length < 2) return null;
    
    // Use first two equations
    const a11 = A[0][0], a12 = A[0][1];
    const a21 = A[1][0], a22 = A[1][1];
    const b1 = b[0][0], b2 = b[1][0];
    
    const det = a11 * a22 - a12 * a21;
    
    if (Math.abs(det) < 0.0001) {
        throw new Error('Système singulier');
    }
    
    const x = (b1 * a22 - b2 * a12) / det;
    const y = (a11 * b2 - a21 * b1) / det;
    
    return [x, y];
}

// ============================================
// ESP MOTE MANAGEMENT
// ============================================

function addMote() {
    const macInput = document.getElementById('new-mote-mac');
    const mac = macInput.value.trim().toUpperCase();
    
    if (!mac) {
        alert('Veuillez entrer une adresse MAC');
        return;
    }
    
    // Validate MAC format
    const macRegex = /^([0-9A-F]{2}[:-]){5}([0-9A-F]{2})$/;
    if (!macRegex.test(mac)) {
        alert('Format d\'adresse MAC invalide (ex: AA:BB:CC:DD:EE:FF)');
        return;
    }
    
    // Check if already exists
    if (state.motes.find(m => m.mac === mac)) {
        alert('Cette adresse MAC existe déjà');
        return;
    }
    
    const mote = {
        mac: mac,
        position: null,
        floor: state.currentFloor
    };
    
    state.motes.push(mote);
    macInput.value = '';
    
    updateMoteList();
    saveConfiguration();
}

function removeMote(mac) {
    state.motes = state.motes.filter(m => m.mac !== mac);
    updateMoteList();
    drawCanvas();
    saveConfiguration();
}

function updateMoteList() {
    const list = document.getElementById('mote-list');
    list.innerHTML = '';
    
    state.motes.forEach(mote => {
        const item = document.createElement('div');
        item.className = 'mote-item';
        
        const posText = mote.position 
            ? `📍 (${Math.round(mote.position.x)}, ${Math.round(mote.position.y)}) - ${state.floors[mote.floor].name}`
            : '❌ Non placé';
        
        item.innerHTML = `
            <div class="mote-header">
                <div>
                    <div class="mote-mac">${mote.mac}</div>
                    <div class="mote-position">${posText}</div>
                </div>
                <div class="mote-actions">
                    <button class="btn-icon" onclick="selectMoteForPlacement('${mote.mac}')" title="Placer sur le plan">
                        📍
                    </button>
                    <button class="btn-icon" onclick="removeMote('${mote.mac}')" title="Supprimer">
                        🗑️
                    </button>
                </div>
            </div>
        `;
        
        list.appendChild(item);
    });
}

function selectMoteForPlacement(mac) {
    state.selectedMote = mac;
    state.canvasMode = 'place-motes';
    document.getElementById('canvas-mode').value = 'place-motes';
    alert(`Cliquez sur le plan pour placer ${mac}`);
}

// Make functions globally accessible
window.removeMote = removeMote;
window.selectMoteForPlacement = selectMoteForPlacement;

// ============================================
// DEVICE LIST
// ============================================

function updateDeviceList() {
    const list = document.getElementById('device-list');
    const filter = document.getElementById('device-filter').value.toLowerCase();
    
    const devices = Object.values(state.devices)
        .filter(d => d.name.toLowerCase().includes(filter))
        .sort((a, b) => b.lastUpdate - a.lastUpdate);
    
    if (devices.length === 0) {
        list.innerHTML = `
            <div style="text-align: center; padding: var(--spacing-xl); color: var(--color-text-muted);">
                <div style="font-size: 3rem; opacity: 0.3;">📡</div>
                <p class="text-small">Aucun appareil détecté</p>
            </div>
        `;
        return;
    }
    
    list.innerHTML = '';
    
    devices.forEach(device => {
        const item = document.createElement('div');
        item.className = 'device-item';
        
        const rssiCount = Object.keys(device.rssiData).length;
        const posText = device.position 
            ? `X: ${Math.round(device.position.x)}, Y: ${Math.round(device.position.y)}`
            : 'Position inconnue';
        
        const age = Math.round((Date.now() - device.lastUpdate) / 1000);
        
        item.innerHTML = `
            <div class="device-name">📱 ${device.name}</div>
            <div class="device-info">
                <div class="device-position">${posText}</div>
                <div>🎯 ${rssiCount} ESP mote(s)</div>
                <div>⏱️ Mis à jour il y a ${age}s</div>
            </div>
        `;
        
        list.appendChild(item);
    });
}

function filterDevices() {
    updateDeviceList();
}

// ============================================
// CANVAS DRAWING
// ============================================

function drawCanvas() {
    // Clear canvas
    ctx.fillStyle = '#0a0e1a';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    
    // Draw floor plan if available
    const floor = state.floors[state.currentFloor];
    if (floor.image) {
        const scale = Math.min(
            canvas.width / floor.image.width,
            canvas.height / floor.image.height
        );
        const width = floor.image.width * scale;
        const height = floor.image.height * scale;
        const x = (canvas.width - width) / 2;
        const y = (canvas.height - height) / 2;
        
        ctx.drawImage(floor.image, x, y, width, height);
    } else {
        // Draw grid
        drawGrid();
    }
    
    // Draw ESP motes
    drawMotes();
    
    // Draw devices
    drawDevices();
}

function drawGrid() {
    ctx.strokeStyle = 'rgba(148, 163, 184, 0.1)';
    ctx.lineWidth = 1;
    
    const gridSize = 50;
    
    for (let x = 0; x < canvas.width; x += gridSize) {
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, canvas.height);
        ctx.stroke();
    }
    
    for (let y = 0; y < canvas.height; y += gridSize) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(canvas.width, y);
        ctx.stroke();
    }
}

function drawMotes() {
    state.motes.forEach(mote => {
        if (!mote.position || mote.floor !== state.currentFloor) return;
        
        const { x, y } = mote.position;
        
        // Draw range circle (semi-transparent)
        ctx.fillStyle = 'rgba(0, 212, 255, 0.05)';
        ctx.beginPath();
        ctx.arc(x, y, 100, 0, Math.PI * 2);
        ctx.fill();
        
        // Draw mote marker
        ctx.fillStyle = '#00d4ff';
        ctx.beginPath();
        ctx.arc(x, y, 8, 0, Math.PI * 2);
        ctx.fill();
        
        // Draw border
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 2;
        ctx.stroke();
        
        // Draw label
        ctx.fillStyle = '#f1f5f9';
        ctx.font = '12px Inter';
        ctx.textAlign = 'center';
        ctx.fillText(mote.mac.substring(12), x, y - 15);
    });
}

function drawDevices() {
    Object.values(state.devices).forEach(device => {
        if (!device.position) return;
        
        const { x, y } = device.position;
        
        // Draw device marker
        ctx.fillStyle = '#10b981';
        ctx.beginPath();
        ctx.arc(x, y, 6, 0, Math.PI * 2);
        ctx.fill();
        
        // Draw border
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 2;
        ctx.stroke();
        
        // Draw label
        ctx.fillStyle = '#10b981';
        ctx.font = 'bold 12px Inter';
        ctx.textAlign = 'center';
        ctx.fillText(device.name, x, y + 20);
    });
}

// ============================================
// CANVAS INTERACTION
// ============================================

function handleCanvasClick(event) {
    if (state.canvasMode !== 'place-motes') return;
    
    const rect = canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    
    if (state.selectedMote) {
        // Place the selected mote
        const mote = state.motes.find(m => m.mac === state.selectedMote);
        if (mote) {
            mote.position = { x, y };
            mote.floor = state.currentFloor;
            state.selectedMote = null;
            updateMoteList();
            drawCanvas();
            saveConfiguration();
        }
    } else {
        // Find unplaced mote
        const unplacedMote = state.motes.find(m => !m.position);
        if (unplacedMote) {
            unplacedMote.position = { x, y };
            unplacedMote.floor = state.currentFloor;
            updateMoteList();
            drawCanvas();
            saveConfiguration();
        } else {
            alert('Tous les ESP motes sont déjà placés');
        }
    }
}

function handleCanvasHover(event) {
    // Could add hover effects here
}

// ============================================
// FLOOR SWITCHING
// ============================================

function switchFloor(floorIndex) {
    state.currentFloor = floorIndex;
    
    // Update UI
    document.querySelectorAll('.floor-btn').forEach((btn, idx) => {
        if (idx === floorIndex) {
            btn.classList.add('active');
        } else {
            btn.classList.remove('active');
        }
    });
    
    drawCanvas();
}

// ============================================
// CONFIGURATION MANAGEMENT
// ============================================

function saveConfiguration() {
    const config = {
        motes: state.motes,
        rssiParams: state.rssiParams,
        mqtt: {
            broker: document.getElementById('mqtt-broker').value,
            port: document.getElementById('mqtt-port').value,
            protocol: document.getElementById('mqtt-protocol').value
        }
    };
    
    localStorage.setItem('ble_localization_config', JSON.stringify(config));
    alert('✅ Configuration sauvegardée');
}

function loadConfiguration() {
    const saved = localStorage.getItem('ble_localization_config');
    if (!saved) return;
    
    try {
        const config = JSON.parse(saved);
        
        if (config.motes) {
            state.motes = config.motes;
            updateMoteList();
        }
        
        if (config.rssiParams) {
            state.rssiParams = config.rssiParams;
            document.getElementById('rssi-at-1m').value = config.rssiParams.rssiAt1m;
            document.getElementById('path-loss').value = config.rssiParams.pathLoss;
        }
        
        if (config.mqtt) {
            document.getElementById('mqtt-broker').value = config.mqtt.broker;
            document.getElementById('mqtt-port').value = config.mqtt.port;
            document.getElementById('mqtt-protocol').value = config.mqtt.protocol;
        }
        
        drawCanvas();
    } catch (e) {
        console.error('Erreur de chargement de la configuration:', e);
    }
}

function clearCanvas() {
    if (confirm('Êtes-vous sûr de vouloir effacer tous les ESP motes ?')) {
        state.motes = [];
        updateMoteList();
        drawCanvas();
        saveConfiguration();
    }
}

// ============================================
// AUTO-REFRESH
// ============================================

setInterval(() => {
    // Remove old device data (older than 30 seconds)
    const now = Date.now();
    Object.keys(state.devices).forEach(deviceName => {
        if (now - state.devices[deviceName].lastUpdate > 30000) {
            delete state.devices[deviceName];
        }
    });
    
    updateDeviceList();
    drawCanvas();
}, 1000);
