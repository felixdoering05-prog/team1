#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TinyGPSPlus.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ESPAsyncWebServer.h>

// ── Wi-Fi ─────────────────────────────────────────────────────────────────────
const char* ssid     = "iPhone14";
const char* password = "12345678";

// ── Pin definitions ───────────────────────────────────────────────────────────
#define TFT_CS         7
#define TFT_DC        39
#define TFT_RST       40
#define TFT_backlight 45
#define SPI_SCK       36
#define SPI_MISO      37
#define SPI_MOSI      35
#define I2C_SDA       42
#define I2C_SCL       41
#define GPS_RX        17
#define GPS_TX        18
#define GPS_BAUD      115200
#define BMP_ADDR      0x77

// ── Objects ───────────────────────────────────────────────────────────────────
Adafruit_ST7789   tft       = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_BMP280   bmp;
TinyGPSPlus       gps;
WebSocketsServer  webSocket(81);   // data stream on port 81
AsyncWebServer    httpServer(80);  // serves the web page on port 80

bool bmpOK = false;

// ── SNR parser ────────────────────────────────────────────────────────────────
float  snrValues[24];
int    snrCount = 0;
float  avgSNR   = 0.0;
String nmeaBuffer = "";

void parseGSVLine(const String& line) {
  if (line.indexOf("GSV") < 0) return;
  String clean = line.substring(0, line.indexOf('*'));
  String fields[25];
  int fieldIdx = 0, start = 0;
  for (int i = 0; i <= clean.length(); i++) {
    if (i == clean.length() || clean[i] == ',') {
      fields[fieldIdx++] = clean.substring(start, i);
      start = i + 1;
      if (fieldIdx >= 25) break;
    }
  }
  if (fields[2].toInt() == 1) snrCount = 0;
  for (int g = 4; g + 3 < fieldIdx; g += 4) {
    int snr = fields[g + 3].toInt();
    if (snr > 0 && snrCount < 24) snrValues[snrCount++] = snr;
  }
  if (snrCount > 0) {
    float sum = 0;
    for (int i = 0; i < snrCount; i++) sum += snrValues[i];
    avgSNR = sum / snrCount;
  }
}

// ── Stored heatmap points ─────────────────────────────────────────────────────
struct HeatPoint { double lat; double lng; float snr; float tempC; float altM; };
HeatPoint heatLog[300];
int       heatCount = 0;

String buildPointsJSON() {
  String j = "[";
  for (int i = 0; i < heatCount; i++) {
    if (i > 0) j += ",";
    j += "{\"lat\":"   + String(heatLog[i].lat,   6)
       + ",\"lng\":"   + String(heatLog[i].lng,   6)
       + ",\"snr\":"   + String(heatLog[i].snr,   1)
       + ",\"tempC\":" + String(heatLog[i].tempC, 1)
       + ",\"altM\":"  + String(heatLog[i].altM,  0) + "}";
  }
  return j + "]";
}

// ── Web page (served over HTTP on port 80) ────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BIP26 Trek Signal Almanac</title>
  <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
  <script src="https://unpkg.com/leaflet.heat@0.2.0/dist/leaflet-heat.js"></script>
  <style>
    *{margin:0;padding:0;box-sizing:border-box;}
    body{font-family:sans-serif;background:#0d1117;color:#e6edf3;display:flex;flex-direction:column;height:100vh;}
    #header{padding:8px 16px;background:#161b22;border-bottom:1px solid #30363d;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px;}
    #header h1{font-size:0.95rem;font-weight:700;letter-spacing:.5px;}
    #status{font-size:0.75rem;color:#8b949e;}
    #map{flex:1;}
    #panel{display:flex;flex-wrap:wrap;gap:5px;padding:7px 12px;background:#161b22;border-top:1px solid #30363d;}
    .card{background:#21262d;border:1px solid #30363d;border-radius:7px;padding:5px 10px;min-width:80px;text-align:center;}
    .card .lbl{font-size:0.6rem;color:#8b949e;text-transform:uppercase;letter-spacing:.4px;}
    .card .val{font-size:0.95rem;font-weight:700;margin-top:1px;}
    #snr-wrap{flex:1;min-width:180px;background:#21262d;border:1px solid #30363d;border-radius:7px;padding:5px 10px;}
    #snr-top{display:flex;justify-content:space-between;font-size:0.65rem;color:#8b949e;margin-bottom:3px;}
    #snr-bg{background:#0d1117;border-radius:3px;height:11px;}
    #snr-fill{height:11px;border-radius:3px;transition:width .6s,background .6s;width:0%;}
    #legend{display:flex;align-items:center;gap:8px;padding:4px 12px;background:#0d1117;font-size:0.65rem;flex-wrap:wrap;}
    #legend span{color:#8b949e;}
    .leg{display:flex;align-items:center;gap:3px;}
    .ldot{width:9px;height:9px;border-radius:50%;}
    #warning{display:none;padding:5px 16px;background:#7f1d1d;font-size:0.78rem;font-weight:600;text-align:center;letter-spacing:.3px;}
    #view-btns{display:flex;gap:5px;}
    .vbtn{padding:3px 10px;border-radius:20px;border:1px solid #30363d;background:#21262d;color:#e6edf3;cursor:pointer;font-size:0.7rem;}
    .vbtn.active{background:#1f6feb;border-color:#1f6feb;}
  </style>
</head>
<body>

<div id="header">
  <h1>📡 BIP26 Trek Signal Almanac</h1>
  <div id="view-btns">
    <button class="vbtn active" id="btn-heat" onclick="setView('heat',this)">Heatmap</button>
    <button class="vbtn" id="btn-dots" onclick="setView('dots',this)">Dots</button>
    <button class="vbtn" id="btn-both" onclick="setView('both',this)">Both</button>
  </div>
  <span id="status">Connecting...</span>
</div>

<div id="warning">⚠ Weak signal zone — SOS reliability degraded at this location</div>
<div id="map"></div>

<div id="panel">
  <div class="card"><div class="lbl">Latitude</div><div class="val" id="c-lat">--</div></div>
  <div class="card"><div class="lbl">Longitude</div><div class="val" id="c-lng">--</div></div>
  <div class="card"><div class="lbl">Sats</div><div class="val" id="c-sats">--</div></div>
  <div class="card"><div class="lbl">HDOP</div><div class="val" id="c-hdop">--</div></div>
  <div class="card"><div class="lbl">Temp °C</div><div class="val" id="c-temp">--</div></div>
  <div class="card"><div class="lbl">Alt m</div><div class="val" id="c-alt">--</div></div>
  <div id="snr-wrap">
    <div id="snr-top">
      <span>Signal Quality (SNR)</span>
      <span id="snr-num">-- dB-Hz</span>
    </div>
    <div id="snr-bg"><div id="snr-fill"></div></div>
  </div>
</div>

<div id="legend">
  <span>SOS Reliability:</span>
  <div class="leg"><div class="ldot" style="background:#2ecc71"></div>✅ ≥35 Reliable</div>
  <div class="leg"><div class="ldot" style="background:#f1c40f"></div>🟡 25-34 Likely OK</div>
  <div class="leg"><div class="ldot" style="background:#e67e22"></div>🟠 15-24 Degraded</div>
  <div class="leg"><div class="ldot" style="background:#e74c3c"></div>🔴 &lt;15 Avoid</div>
</div>

<script>
// ── Map ───────────────────────────────────────────────────────────────────────
const map = L.map('map').setView([53.29,-6.37],17);
L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
  {attribution:'© Esri',maxZoom:20}).addTo(map);
L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_only_labels/{z}/{x}/{y}{r}.png',
  {opacity:0.7}).addTo(map);

let heatLayer  = null;
let dotsLayer  = L.layerGroup().addTo(map);
let liveMarker = null;
let centred    = false;
let allPts     = [];
let curView    = 'heat';

function snrColour(s){ return s>=35?'#2ecc71':s>=25?'#f1c40f':s>=15?'#e67e22':'#e74c3c'; }
function snrPct(s){ return Math.min(100,Math.max(0,(s/50)*100)); }

function rebuildHeat(){
  if(heatLayer) map.removeLayer(heatLayer);
  const pts = allPts.map(p=>[p.lat,p.lng, 1-Math.min(1,p.snr/50)]);
  heatLayer = L.heatLayer(pts,{radius:25,blur:20,gradient:{'0.0':'#2ecc71','0.35':'#f1c40f','0.65':'#e67e22','1.0':'#e74c3c'}});
  if(curView==='heat'||curView==='both') heatLayer.addTo(map);
}

function addDot(p){
  L.circleMarker([p.lat,p.lng],{
    radius:6, color:snrColour(p.snr), fillColor:snrColour(p.snr), fillOpacity:0.9, weight:1
  }).bindPopup(
    `<b>${p.snr>=35?'✅ SOS Reliable':p.snr>=25?'🟡 SOS Likely OK':p.snr>=15?'🟠 SOS Degraded':'🔴 Avoid — SOS Risk'}</b><br>`+
    `SNR: ${p.snr.toFixed(1)} dB-Hz<br>`+
    `Temp: ${p.tempC.toFixed(1)} °C<br>`+
    `Alt: ${p.altM.toFixed(0)} m<br>`+
    `Lat: ${p.lat.toFixed(6)}<br>Lng: ${p.lng.toFixed(6)}`
  ).addTo(dotsLayer);
}

function setView(v, btn){
  curView = v;
  document.querySelectorAll('.vbtn').forEach(b=>b.classList.remove('active'));
  btn.classList.add('active');
  if(heatLayer) map.removeLayer(heatLayer);
  dotsLayer.clearLayers();
  allPts.forEach(addDot);
  if(v==='heat'){ dotsLayer.clearLayers(); if(heatLayer) heatLayer.addTo(map); }
  if(v==='dots'){ if(heatLayer) map.removeLayer(heatLayer); }
  if(v==='both'){ if(heatLayer) heatLayer.addTo(map); }
}

// ── Load history from /points ─────────────────────────────────────────────────
fetch('/points').then(r=>r.json()).then(pts=>{
  pts.forEach(p=>{ allPts.push(p); addDot(p); });
  rebuildHeat();
  if(pts.length>0){ map.setView([pts[pts.length-1].lat,pts[pts.length-1].lng],18); centred=true; }
}).catch(()=>{});

// ── WebSocket — receives live pushes from ESP32 ───────────────────────────────
const ws = new WebSocket('ws://' + location.hostname + ':81');

ws.onopen = ()=>{
  document.getElementById('status').textContent = 'WebSocket connected';
  document.getElementById('status').style.color = '#2ecc71';
};
ws.onclose = ()=>{
  document.getElementById('status').textContent = 'Disconnected';
  document.getElementById('status').style.color = '#e74c3c';
};

ws.onmessage = (evt)=>{
  let d;
  try{ d = JSON.parse(evt.data); } catch(e){ return; }
  if(d.status) return; // welcome message

  document.getElementById('c-lat').textContent  = d.lat.toFixed(6);
  document.getElementById('c-lng').textContent  = d.lng.toFixed(6);
  document.getElementById('c-sats').textContent = d.sats;
  document.getElementById('c-hdop').textContent = d.hdop.toFixed(2);
  document.getElementById('c-temp').textContent = d.tempC.toFixed(1);
  document.getElementById('c-alt').textContent  = d.altM.toFixed(0);
  document.getElementById('snr-num').textContent = d.snr.toFixed(1)+' dB-Hz';
  document.getElementById('snr-fill').style.width      = snrPct(d.snr)+'%';
  document.getElementById('snr-fill').style.background = snrColour(d.snr);

  document.getElementById('status').textContent =
    d.fixOK ? `Fix ✓  ${d.sats} sats` : 'Searching...';

  document.getElementById('warning').style.display =
    (d.fixOK && d.snr < 25) ? 'block' : 'none';

  if(d.fixOK && d.snr > 0){
    const ll=[d.lat,d.lng];
    allPts.push(d);
    addDot(d);
    rebuildHeat();

    if(!liveMarker){
      liveMarker=L.circleMarker(ll,{radius:12,color:'#fff',fill:false,weight:2}).addTo(map);
    } else { liveMarker.setLatLng(ll); }
    if(!centred){ map.setView(ll,18); centred=true; }
  }
};
</script>
</body>
</html>
)rawliteral";

// ── WebSocket event handler ───────────────────────────────────────────────────
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[WS] Client %u connected from %s\n", num, ip.toString().c_str());
    webSocket.sendTXT(num, "{\"status\":\"connected\"}");
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("[WS] Client %u disconnected\n", num);
  }
}

// ── TFT row helper ────────────────────────────────────────────────────────────
void tftRow(int y, const char* label, String value, uint16_t colour = ST77XX_WHITE) {
  tft.fillRect(0, y, 240, 16, ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK); tft.setCursor(2, y+2); tft.print(label);
  tft.setTextColor(colour,      ST77XX_BLACK); tft.print(value);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS);
  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(TFT_backlight, OUTPUT);
  digitalWrite(TFT_backlight, HIGH);
  tft.init(135, 240); tft.setRotation(3); tft.fillScreen(ST77XX_BLACK);

  tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(10, 4); tft.print("Trek Almanac");
  tft.setTextSize(1);

  // BMP280
  bmpOK = bmp.begin(BMP_ADDR);
  if (bmpOK) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
  }

  // Wi-Fi
  tft.setCursor(2, 28); tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.print("Connecting WiFi...");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }

  tft.fillRect(0, 26, 240, 16, ST77XX_BLACK);
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    tft.setCursor(2, 28); tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.print("IP: "); tft.print(ip);
    Serial.println("WiFi connected: " + ip);
  } else {
    tft.setCursor(2, 28); tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
    tft.print("WiFi failed");
  }

  // HTTP server — serves the web page and historical points
  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send_P(200, "text/html", INDEX_HTML);
  });
  httpServer.on("/points", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "application/json", buildPointsJSON());
  });
  httpServer.begin();

  // WebSocket server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop() {
  webSocket.loop();

  // Read GPS — intercept each line for SNR parsing
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    gps.encode(c);
    if (c == '\n') {
      parseGSVLine(nmeaBuffer);
      nmeaBuffer = "";
    } else if (c != '\r') {
      nmeaBuffer += c;
    }
  }

  if (millis() - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = millis();

    bool fixOK = gps.location.isValid() && gps.hdop.isValid();
    float tempC = bmpOK ? bmp.readTemperature()      : 0.0f;
    float altM  = bmpOK ? bmp.readAltitude(1013.25f) : 0.0f;
    int   sats  = gps.satellites.value();

    if (fixOK) {
      double lat  = gps.location.lat();
      double lng  = gps.location.lng();
      double hdop = gps.hdop.hdop();

      // Store point
      if (avgSNR > 0 && heatCount < 300) {
        heatLog[heatCount++] = { lat, lng, avgSNR, tempC, altM };
      }

      // HDOP colour
      uint16_t hdopCol = (hdop < 1.5) ? ST77XX_GREEN :
                         (hdop < 2.5) ? 0xFD20 : ST77XX_RED;
      // SNR colour
      uint16_t snrCol  = (avgSNR >= 35) ? ST77XX_GREEN :
                         (avgSNR >= 25) ? ST77XX_YELLOW :
                         (avgSNR >= 15) ? 0xFD20 : ST77XX_RED;

      // TFT
      tftRow(44,  "Lat:  ", String(lat,  6), ST77XX_YELLOW);
      tftRow(60,  "Lng:  ", String(lng,  6), ST77XX_YELLOW);
      tftRow(76,  "Sats: ", String(sats),    ST77XX_WHITE);
      tftRow(92,  "HDOP: ", String(hdop, 2), hdopCol);
      tftRow(108, "SNR:  ", String(avgSNR,1)+" dB", snrCol);
      tftRow(120, "Tmp:  ", String(tempC,1)+"C  Alt:"+String(altM,0)+"m", ST77XX_CYAN);

      // WebSocket JSON broadcast
      char buf[200];
      snprintf(buf, sizeof(buf),
        "{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.2f,\"sats\":%d,"
        "\"snr\":%.1f,\"tempC\":%.1f,\"altM\":%.0f,\"fixOK\":true}",
        lat, lng, hdop, sats, avgSNR, tempC, altM);
      webSocket.broadcastTXT(buf);

    } else {
      tft.fillRect(0, 44, 240, 80, ST77XX_BLACK);
      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(2, 48); tft.print("Searching... Sats: "); tft.print(sats);
      tft.setCursor(2, 64); tft.print("Chars RX: "); tft.print(gps.charsProcessed());
      if (bmpOK) {
        tftRow(80,  "Tmp: ", String(tempC,1)+"C", ST77XX_MAGENTA);
        tftRow(96,  "Alt: ", String(altM,0)+"m",  ST77XX_CYAN);
      }
      // Still broadcast so the web page stays alive
      char buf[120];
      snprintf(buf, sizeof(buf),
        "{\"lat\":0,\"lng\":0,\"hdop\":99,\"sats\":%d,"
        "\"snr\":%.1f,\"tempC\":%.1f,\"altM\":%.0f,\"fixOK\":false}",
        sats, avgSNR, tempC, altM);
      webSocket.broadcastTXT(buf);
    }
  }
}a