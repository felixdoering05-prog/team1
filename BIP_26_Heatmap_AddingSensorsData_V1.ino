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
#define BMP_ADDR      0x77     // ← only new define

// ── Objects ───────────────────────────────────────────────────────────────────
Adafruit_ST7789  tft       = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_BMP280  bmp;          // ← only new object
TinyGPSPlus      gps;
WebSocketsServer webSocket(81);

bool bmpOK = false;            // ← only new variable

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 1000;

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

  // ── BMP280 init — added after Wire.begin() ────────────────────────────────
  bmpOK = bmp.begin(BMP_ADDR);
  if (bmpOK) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("BMP280 OK");
  } else {
    Serial.println("BMP280 not found — check wiring");
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

    // ── Read BMP280 every cycle ───────────────────────────────────────────
    float tempC = bmpOK ? bmp.readTemperature()      : 0.0f;
    float altM  = bmpOK ? bmp.readAltitude(1013.25f) : 0.0f;

    if (gps.location.isValid() && gps.hdop.isValid()) {
      double lat  = gps.location.lat();
      double lng  = gps.location.lng();
      double hdop = gps.hdop.hdop();
      int    sats = gps.satellites.value();

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
      tft.print("Temp: ");
      tft.print(bmpOK ? String(tempC, 1) : "N/A");
      tft.println(" C");

      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 128);
      tft.print("Alt:  ");
      tft.print(bmpOK ? String(altM, 0) : "N/A");
      tft.println(" m");

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // reset colour

      // ── HDOP colour box — UNCHANGED ──────────────────────────────────────
      tft.fillRect(180, 55, 40, 40, color);
      tft.drawRect(180, 55, 40, 40, ST77XX_WHITE);

      // ── JSON — original fields kept, tempC and altM added ─────────────────
      char jsonBuf[160];
      snprintf(jsonBuf, sizeof(jsonBuf),
        "{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.2f,\"sats\":%d,"
        "\"tempC\":%.1f,\"altM\":%.0f}",
        lat, lng, hdop, sats, tempC, altM);
      webSocket.broadcastTXT(jsonBuf);

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
      tft.print("Temp: ");
      tft.print(bmpOK ? String(tempC, 1) : "N/A");
      tft.println(" C");

      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 120);
      tft.print("Alt:  ");
      tft.print(bmpOK ? String(altM, 0) : "N/A");
      tft.println(" m");

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    }
  }
}