const espMap = {
    'mac1': { mac: 'mac1', x: 0, y: 0, z: 0 },
    'mac2': { mac: 'mac2', x: 0, y: 10, z: 0 },
    'mac3': { mac: 'mac3', x: 10, y: 0, z: 0 },
    'mac4': { mac: 'mac4', x: 0, y: 0, z: 1 }
};

const bleDevices = {
    'user1': {
        motes: {
            'mac1': { rssi: -60, timestamp: Date.now() },
            'mac2': { rssi: -65, timestamp: Date.now() },
            'mac3': { rssi: -75, timestamp: Date.now() }
        }
    },
    'user2': {
        motes: {
            'mac3': { rssi: -50, timestamp: Date.now() }
        }
    },
    'user3': {
        motes: {
            'mac1': { rssi: -59, timestamp: Date.now() },
            'mac2': { rssi: -85, timestamp: Date.now() }
        }
    },
    'user4': {
        motes: {
            'mac4': { rssi: -40, timestamp: Date.now() },
            'mac1': { rssi: -85, timestamp: Date.now() }
        }
    }
};

const blePositions = {};

// ============================================
// POSITION LOGIC (Copied from server.js)
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

    for (const [deviceName, bleData] of Object.entries(bleDevices)) {
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
                delete bleData.motes[mac];
            }
        }

        if (recentMotes.length === 0) continue;

        const floorMotes = recentMotes.filter(m => m.z === bestFloor);
        if (floorMotes.length === 0) continue;

        floorMotes.sort((a, b) => b.rssi - a.rssi);

        let finalPos = null;
        const historyPos = blePositions[deviceName];

        if (floorMotes.length === 1) {
            finalPos = { x: floorMotes[0].x, y: floorMotes[0].y, z: bestFloor };
        } else if (floorMotes.length === 2) {
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
            const top3 = floorMotes.slice(0, 3).map(m => ({ ...m, r: estimateDistance(m.rssi) }));
            const triPos = trilaterate(top3[0], top3[1], top3[2]);

            if (triPos && !isNaN(triPos.x) && !isNaN(triPos.y)) {
                finalPos = { ...triPos, z: bestFloor };
            } else {
                let wx = 0, wy = 0, sumWeight = 0;
                top3.forEach(m => { let w = 1 / m.r; wx += m.x * w; wy += m.y * w; sumWeight += w; });
                finalPos = { x: wx / sumWeight, y: wy / sumWeight, z: bestFloor };
            }
        }

        if (finalPos) {
            finalPos.x = Math.round(finalPos.x * 100) / 100;
            finalPos.y = Math.round(finalPos.y * 100) / 100;
            blePositions[deviceName] = { ...finalPos, timestamp: now };
        }
    }

    console.log('--- POSITIONING RESULTS ---');
    console.log(blePositions);
}

calculatePositions();
