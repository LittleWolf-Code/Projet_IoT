const mqtt = require('mqtt');
const aedes = require('aedes')();
const net = require('net');
const http = require('http');

console.log('Starting Aedes broker...');
const server = net.createServer(aedes.handle);
server.listen(1883, function () {
  console.log('Aedes broker listening on port 1883');
  
  setTimeout(() => {
    console.log('Publishing mock MQTT data...');
    const client = mqtt.connect('mqtt://localhost:1883');
    client.on('connect', () => {
        // Mac addresses: mac1, mac2, mac3
        // bleDevice: "user1"
        client.publish('esp32/mac1/ble/user1/rssi', '-65'); // Close to mac1
        client.publish('esp32/mac2/ble/user1/rssi', '-75'); // Medium from mac2
        client.publish('esp32/mac3/ble/user1/rssi', '-85'); // Far from mac3
        
        // Floor test: user2 at z=1
        client.publish('esp32/mac4/ble/user2/rssi', '-50'); // Strong at floor 1
        client.publish('esp32/mac1/ble/user2/rssi', '-80'); // Weak at floor 0
        
        console.log('Data published. Waiting for backend positioning loop...');
        
        setTimeout(() => {
            console.log('Fetching positions from API...');
            http.get('http://localhost:3000/api/positions', (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    console.log('Positions result:', data);
                    process.exit(0);
                });
            }).on('error', err => {
                console.error('Error fetching API:', err);
                process.exit(1);
            });
        }, 8000); // Wait 8 seconds (position calculation loop is 5s)
    });
  }, 3000);
});
