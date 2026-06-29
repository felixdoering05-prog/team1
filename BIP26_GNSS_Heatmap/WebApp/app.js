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
// Custom gradient mapping for HDOP:
// 1.0 (Green/Strong) -> Intensity 1.0
// 0.5 (Orange/Moderate) -> Intensity 0.5
// 0.2 (Red/Weak) -> Intensity 0.2
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
const btnConnect = document.getElementById('connect-btn');
const statusText = document.getElementById('connection-status');
const valSats = document.getElementById('sats-val');
const valHdop = document.getElementById('hdop-val');
const qualityIndicator = document.getElementById('quality-indicator');

let port;
let reader;
let keepReading = true;

// Handle Connect Button
btnConnect.addEventListener('click', async () => {
    if (port) {
        await disconnectDevice();
    } else {
        await connectDevice();
    }
});

async function connectDevice() {
    try {
        // Request a port and open a connection
        port = await navigator.serial.requestPort();
        await port.open({ baudRate: 115200 });

        statusText.textContent = "Connected to ESP32";
        btnConnect.textContent = "Disconnect";
        btnConnect.classList.add('connected');
        
        keepReading = true;
        readLoop();
    } catch (err) {
        console.error("Connection error:", err);
        statusText.textContent = "Error: " + err.message;
    }
}

async function disconnectDevice() {
    keepReading = false;
    if (reader) {
        await reader.cancel();
    }
    
    statusText.textContent = "Disconnected";
    btnConnect.textContent = "Connect to ESP32";
    btnConnect.classList.remove('connected');
    port = null;
}

// Read data from Serial
async function readLoop() {
    const textDecoder = new TextDecoderStream();
    const readableStreamClosed = port.readable.pipeTo(textDecoder.writable);
    reader = textDecoder.readable.getReader();

    let buffer = "";

    try {
        while (keepReading) {
            const { value, done } = await reader.read();
            if (done) {
                // reader has been canceled
                break;
            }
            if (value) {
                buffer += value;
                let lines = buffer.split('\n');
                // Keep the last partial line in the buffer
                buffer = lines.pop(); 

                for (let line of lines) {
                    processLine(line.trim());
                }
            }
        }
    } catch (error) {
        console.error("Read loop error:", error);
        statusText.textContent = "Connection lost";
    } finally {
        reader.releaseLock();
        await readableStreamClosed.catch(e => {}); // Ignore errors on close
        await port.close();
    }
}

// Process incoming JSON from ESP32
function processLine(line) {
    if (!line.startsWith('{') || !line.endsWith('}')) return;

    try {
        const data = JSON.parse(line);
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

    // Calculate intensity based on HDOP
    // Target: HDOP 1.0 -> Intensity 1.0
    // Target: HDOP 2.5 -> Intensity 0.5
    // Target: HDOP > 4.0 -> Intensity 0.2
    // We use a simple clamping function
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

    // Add point to heatmap: [lat, lng, intensity]
    heat.addLatLng([lat, lng, intensity]);
    
    // Optionally pan map to current location (uncomment if desired)
    // map.panTo([lat, lng]);
}
