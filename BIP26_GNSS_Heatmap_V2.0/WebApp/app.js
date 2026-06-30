// TU Dublin Tallaght Campus Coordinates
const CAMPUS_LAT = 53.2890;
const CAMPUS_LNG = -6.3620;
const DEFAULT_ZOOM = 17;

// Initialize Map
const map = L.map('map').setView([CAMPUS_LAT, CAMPUS_LNG], DEFAULT_ZOOM);

// Add OpenStreetMap tiles
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    maxZoom: 19,
    attribution: '© OpenStreetMap contributors'
}).addTo(map);

// Configure Heatmap
const heatGradient = {
    0.2: 'red',
    0.5: 'orange',
    1.0: '#22c55e' // green
};

const heat = L.heatLayer([], {
    radius: 25,
    blur: 15,
    maxZoom: 19,
    max: 1.0, 
    gradient: heatGradient
}).addTo(map);

// UI Elements
const ipInput = document.getElementById('ip-input');
const btnConnect = document.getElementById('connect-btn');
const statusText = document.getElementById('connection-status');
const valSats = document.getElementById('sats-val');
const valHdop = document.getElementById('hdop-val');
const qualityIndicator = document.getElementById('quality-indicator');

let ws = null;

// Handle Connect Button
btnConnect.addEventListener('click', () => {
    if (ws && ws.readyState === WebSocket.OPEN) {
        disconnectDevice();
    } else {
        const ip = ipInput.value.trim();
        if (!ip) {
            statusText.textContent = "Please enter an IP address";
            return;
        }
        connectDevice(ip);
    }
});

function connectDevice(ip) {
    statusText.textContent = "Connecting to " + ip + "...";
    
    // Connect to WebSocket server on ESP32 (Port 81)
    ws = new WebSocket(`ws://${ip}:81`);

    ws.onopen = () => {
        statusText.textContent = "Connected to ESP32 (Wi-Fi)";
        btnConnect.textContent = "Disconnect";
        btnConnect.classList.add('connected');
    };

    ws.onmessage = (event) => {
        processLine(event.data);
    };

    ws.onclose = () => {
        statusText.textContent = "Disconnected";
        btnConnect.textContent = "Connect via Wi-Fi";
        btnConnect.classList.remove('connected');
        ws = null;
    };

    ws.onerror = (error) => {
        console.error("WebSocket Error: ", error);
        statusText.textContent = "Connection Error!";
        btnConnect.textContent = "Connect via Wi-Fi";
        btnConnect.classList.remove('connected');
        if (ws) {
            ws.close();
            ws = null;
        }
    };
}

function disconnectDevice() {
    if (ws) {
        ws.close();
    }
}

// Process incoming JSON from ESP32
function processLine(line) {
    if (!line.startsWith('{') || !line.endsWith('}')) return;

    try {
        const data = JSON.parse(line);
        // Ignore the welcome message
        if (data.status === "connected") return;
        
        updateUI(data);
    } catch (e) {
        console.error("Failed to parse JSON:", line);
    }
}

function updateUI(data) {
    const { lat, lng, hdop, sats } = data;

    // Update Text Stats
    valSats.textContent = sats;
    valHdop.textContent = hdop.toFixed(2);

    let intensity = 0.2; // default weak
    
    if (hdop < 1.5) {
        intensity = 1.0;
        qualityIndicator.style.backgroundColor = 'var(--color-green)';
    } else if (hdop <= 2.5) {
        intensity = 0.5;
        qualityIndicator.style.backgroundColor = 'var(--color-orange)';
    } else {
        intensity = 0.2;
        qualityIndicator.style.backgroundColor = 'var(--color-red)';
    }

    // Add point to heatmap
    heat.addLatLng([lat, lng, intensity]);
}
