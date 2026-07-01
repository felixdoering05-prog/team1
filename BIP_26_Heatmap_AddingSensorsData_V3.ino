#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TinyGPSPlus.h>
#include <Adafruit_BMP280.h>   // ← only new include
#include <WiFi.h>
#include <WebSocketsServer.h>

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
#define BMP_ONBOARD   0x77
#define BMP_EXT       0x76

// ── Objects ───────────────────────────────────────────────────────────────────
Adafruit_ST7789  tft       = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_BMP280  bmpOnboard;
Adafruit_BMP280  bmpExt;
TinyGPSPlus      gps;
WebSocketsServer webSocket(81);

bool bmpOnboardOK = false;
bool bmpExtOK = false;

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 1000;

// ── Telemetry History ─────────────────────────────────────────────────────────
struct TelemetryPoint {
  double lat; double lng; double hdop; int sats;
  float temp; float press; float alt; float speed; float dist;
};
TelemetryPoint history[300];
int histCount = 0;
unsigned long lastHistSave = 0;
const unsigned long HIST_INTERVAL = 5000; // Save history every 5 seconds (25 min total)

double lastLat = 0.0;
double lastLng = 0.0;
float totalDistance = 0.0;

// ── WebSocket event handler — UNCHANGED ──────────────────────────────────────
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n",
                      num, ip[0], ip[1], ip[2], ip[3], payload);
        webSocket.sendTXT(num, "{\"status\":\"connected\"}");
        
        // Push session history on connect
        for (int i = 0; i < histCount; i++) {
          char buf[200];
          snprintf(buf, sizeof(buf),
            "{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.2f,\"sats\":%d,"
            "\"temp\":%.1f,\"press\":%.1f,\"altM\":%.1f,\"speed\":%.1f,\"dist\":%.1f}",
            history[i].lat, history[i].lng, history[i].hdop, history[i].sats,
            history[i].temp, history[i].press, history[i].alt, history[i].speed, history[i].dist);
          webSocket.sendTXT(num, buf);
          delay(5); // Yield to network stack to prevent buffer bloat/lag
        }
      }
      break;
    case WStype_TEXT:
    case WStype_BIN:
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  // ── CRITICAL INIT SEQUENCE — UNCHANGED ───────────────────────────────────
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS);
  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(TFT_backlight, OUTPUT);
  digitalWrite(TFT_backlight, HIGH);

  tft.init(135, 240);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("GNSS Heatmap");
  tft.setTextSize(1);
  tft.setCursor(10, 40);
  tft.println("Connecting to Wi-Fi...");
  tft.setCursor(10, 60);
  tft.println(ssid);

  // ── BMP280 init — Dual Support ────────────────────────────────
  bmpOnboardOK = bmpOnboard.begin(BMP_ONBOARD);
  bmpExtOK = bmpExt.begin(BMP_EXT);

  if (bmpExtOK) {
    bmpExt.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16, Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("External BMP280 (0x76) OK! Using for ambient temp.");
  } else if (bmpOnboardOK) {
    bmpOnboard.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16, Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("Onboard BMP280 (0x77) OK (External not found).");
  } else {
    Serial.println("No BMP280 found — check wiring");
  }

  // ── Wi-Fi — UNCHANGED ────────────────────────────────────────────────────
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // ── IP display — UNCHANGED ────────────────────────────────────────────────
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("GNSS Heatmap");
  tft.setTextSize(1);
  tft.setCursor(10, 35);
  tft.print("IP: ");
  tft.println(WiFi.localIP());
  tft.setCursor(10, 55);
  tft.println("Waiting for GPS...");
}

void loop() {
  webSocket.loop();

  while (Serial1.available() > 0) {
    gps.encode(Serial1.read());
  }

  if (millis() - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = millis();

    // ── Read Sensors every cycle ───────────────────────────────────────────
    float tempC = 0.0f;
    float press = 0.0f;
    
    // Prioritize external sensor if connected, otherwise fallback to onboard
    if (bmpExtOK) {
        tempC = bmpExt.readTemperature();
        press = bmpExt.readPressure() / 100.0F; // hPa
    } else if (bmpOnboardOK) {
        tempC = bmpOnboard.readTemperature();
        press = bmpOnboard.readPressure() / 100.0F; // hPa
    }
    float altM  = gps.altitude.meters();
    float speed = gps.speed.kmph();

    if (gps.location.isValid() && gps.hdop.isValid()) {
      double lat  = gps.location.lat();
      double lng  = gps.location.lng();
      double hdop = gps.hdop.hdop();
      int    sats = gps.satellites.value();

      // SMART FILTER 1: Speed Deadband
      // If speed is less than 1.5 km/h, assume stationary to prevent jitter
      if (speed < 1.5) {
          speed = 0.0;
      }

      // SMART FILTER 2: Spatial Threshold & HDOP Confidence
      if (lastLat != 0.0 && lastLng != 0.0) {
        double distMoved = TinyGPSPlus::distanceBetween(lastLat, lastLng, lat, lng);
        
        // Require a larger movement radius if satellite geometry (HDOP) is poor
        double minRadius = (hdop > 2.0) ? 4.0 : 2.5;
        
        // Only accumulate distance if we moved outside the error radius AND speed > 0
        if (distMoved >= minRadius && speed > 0.0) {
            totalDistance += distMoved;
            lastLat = lat;
            lastLng = lng;
        }
      } else {
        // Initialize origin point
        lastLat = lat;
        lastLng = lng;
      }

      // Save History every 5 seconds
      if (millis() - lastHistSave > HIST_INTERVAL) {
        lastHistSave = millis();
        if (histCount < 300) {
          history[histCount++] = {lat, lng, hdop, sats, tempC, press, altM, speed, totalDistance};
        }
      }

      // ── HDOP colour — UNCHANGED ─────────────────────────────────────────
      uint16_t color = ST77XX_RED;
      if      (hdop < 1.5) color = ST77XX_GREEN;
      else if (hdop <= 2.5) color = ST77XX_ORANGE;

      // ── TFT display — original rows kept, two new rows added ─────────────
      tft.fillRect(0, 55, 240, 85, ST77XX_BLACK); // slightly taller clear area
      tft.setCursor(10, 55);
      tft.print("Lat: ");  tft.println(lat, 6);
      tft.setCursor(10, 70);
      tft.print("Lng: ");  tft.println(lng, 6);
      tft.setCursor(10, 85);
      tft.print("Sats: "); tft.println(sats);
      tft.setCursor(10, 100);
      tft.print("HDOP: "); tft.println(hdop, 2);

      // ── Two new sensor rows ───────────────────────────────────────────────
      tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
      tft.setCursor(10, 115);
      tft.print(bmpExtOK ? "Ext T:" : "Temp: ");
      tft.print((bmpExtOK || bmpOnboardOK) ? String(tempC, 1) : "N/A");
      tft.println(" C");

      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 128);
      tft.print("Alt:  ");
      tft.print((bmpExtOK || bmpOnboardOK) ? String(altM, 0) : "N/A");
      tft.println(" m");

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // reset colour

      // ── HDOP colour box — UNCHANGED ──────────────────────────────────────
      tft.fillRect(180, 55, 40, 40, color);
      tft.drawRect(180, 55, 40, 40, ST77XX_WHITE);

      // ── JSON — broadcast all telemetry ───────────────
      char jsonBuf[200];
      snprintf(jsonBuf, sizeof(jsonBuf),
        "{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.2f,\"sats\":%d,"
        "\"temp\":%.1f,\"press\":%.1f,\"altM\":%.1f,\"speed\":%.1f,\"dist\":%.1f}",
        lat, lng, hdop, sats, tempC, press, altM, speed, totalDistance);
      
      // Send via Wi-Fi
      webSocket.broadcastTXT(jsonBuf);
      
      // Send via USB Cable (Web Serial API)
      Serial.println(jsonBuf);

    } else {
      // ── No fix — UNCHANGED, sensor rows still shown ──────────────────────
      tft.fillRect(0, 55, 240, 85, ST77XX_BLACK);
      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(10, 55);
      tft.println("Searching satellites...");
      tft.setCursor(10, 70);
      tft.print("Sats in view: "); tft.println(gps.satellites.value());
      tft.setCursor(10, 85);
      tft.print("Chars RX: ");     tft.println(gps.charsProcessed());

      // Sensor data still displayed while searching
      tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
      tft.setCursor(10, 105);
      tft.print(bmpExtOK ? "Ext T:" : "Temp: ");
      tft.print((bmpExtOK || bmpOnboardOK) ? String(tempC, 1) : "N/A");
      tft.println(" C");

      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 120);
      tft.print("Alt:  ");
      tft.print((bmpExtOK || bmpOnboardOK) ? String(altM, 0) : "N/A");
      tft.println(" m");

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    }
  }
}