const mqtt = require('mqtt');
const aedes = require('aedes')();
const net = require('net');
const http = require('http');
const fs = require('fs');

// Create mock devices
const devices = [
    { mac: 'mac1', name: 'ESP Center', type: 'sink', x: 0, y: 0, z: 0 },
    { mac: 'mac2', name: 'ESP North', type: 'mote', x: 0, y: 10, z: 0 },
    { mac: 'mac3', name: 'ESP East', type: 'mote', x: 10, y: 0, z: 0 },
    { mac: 'mac4', name: 'ESP Floor1', type: 'mote', x: 0, y: 0, z: 1 }
];
fs.writeFileSync('devices.json', JSON.stringify(devices, null, 2));

const config = { "mqtt": { "broker": "localhost", "port": 1883 } };
fs.writeFileSync('config.json', JSON.stringify(config, null, 2));

// Start broker
console.log('Starting Aedes broker...');
const server = net.createServer(aedes.handle);
server.listen(1883, function () {
    console.log('Aedes broker listening on port 1883');

    setTimeout(() => {
        console.log('Publishing mock MQTT data...');
        const client = mqtt.connect('mqtt://localhost:1883');
        client.on('connect', () => {
            // User 1 on Floor 0 (expected near origin if all distances are equal, but let's give different RSSIs)
            client.publish('esp32/mac1/ble/user1/rssi', '-60'); // ~1.0m from mac1 (0,0,0)
            client.publish('esp32/mac2/ble/user1/rssi', '-65'); // ~1.5m from mac2 (0,10,0) -> User1 is close to 0,0,0
            client.publish('esp32/mac3/ble/user1/rssi', '-75'); // ~4.3m from mac3 (10,0,0)

            // User 2: Cas 1 (1 ESP32 saw it)
            client.publish('esp32/mac3/ble/user2/rssi', '-50');

            // User 3: Cas 2 (2 ESP32 saw it)
            client.publish('esp32/mac1/ble/user3/rssi', '-59'); // ~1m from 0,0
            client.publish('esp32/mac2/ble/user3/rssi', '-85'); // Far from 0,10

            // Floor test: User 4 at z=1
            client.publish('esp32/mac4/ble/user4/rssi', '-40'); // Super close to Floor 1
            client.publish('esp32/mac1/ble/user4/rssi', '-85'); // Weak at Floor 0

            console.log('Data published. Waiting 32 seconds for positioning loop (interval=30s)...');

            setTimeout(() => {
                console.log('Fetching positions from API...');
                http.get('http://localhost:3000/api/positions', (res) => {
                    let data = '';
                    res.on('data', chunk => data += chunk);
                    res.on('end', () => {
                        console.log('Positions result:\n', data);
                        process.exit(0);
                    });
                }).on('error', err => {
                    console.error('Error fetching API:', err);
                    process.exit(1);
                });
            }, 32000);
        });
    }, 3000);
});
