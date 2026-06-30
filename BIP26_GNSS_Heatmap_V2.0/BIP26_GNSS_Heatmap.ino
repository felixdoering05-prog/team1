#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WebSocketsServer.h>

// ---------------------------------------------------------
// WIFI SETTINGS - CHANGE THESE TO YOUR PHONE'S HOTSPOT!
// ---------------------------------------------------------
const char* ssid = "iPhone14";
const char* password = "12345678";

// TFT Display Pins
#define TFT_CS         7
#define TFT_DC        39
#define TFT_RST       40
#define TFT_backlight 45

// SPI Bus
#define SPI_SCK       36
#define SPI_MISO      37
#define SPI_MOSI      35

// I2C Bus
#define I2C_SDA       42
#define I2C_SCL       41

// GPS UART
#define GPS_RX        17
#define GPS_TX        18
#define GPS_BAUD      115200

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
TinyGPSPlus gps;

// WebSocket server on port 81
WebSocketsServer webSocket = WebSocketsServer(81);

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 1000; // Update every second

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Disconnected!\n", num);
            break;
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
                // Send a welcome message
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
  Serial.begin(115200); // Debug info
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX); // GPS Module

  // CRITICAL INITIALISATION SEQUENCE FOR DISPLAY
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

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // Start WebSocket Server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // Display IP Address
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
  // Handle WebSocket clients
  webSocket.loop();

  // Feed incoming data to TinyGPSPlus
  while (Serial1.available() > 0) {
    gps.encode(Serial1.read());
  }

  // Update TFT and output JSON once per second
  if (millis() - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = millis();

    if (gps.location.isValid() && gps.hdop.isValid()) {
      double lat = gps.location.lat();
      double lng = gps.location.lng();
      double hdop = gps.hdop.hdop();
      int sats = gps.satellites.value();

      // Determine signal quality color
      uint16_t color = ST77XX_RED; // Weak
      if (hdop < 1.5) {
        color = ST77XX_GREEN; // Strong
      } else if (hdop <= 2.5) {
        color = ST77XX_ORANGE; // Moderate
      }

      // Update TFT UI
      tft.fillRect(0, 55, 240, 80, ST77XX_BLACK); // Clear data area
      tft.setCursor(10, 55);
      tft.print("Lat: "); tft.println(lat, 6);
      tft.setCursor(10, 75);
      tft.print("Lng: "); tft.println(lng, 6);
      tft.setCursor(10, 95);
      tft.print("Sats: "); tft.println(sats);
      tft.setCursor(10, 115);
      tft.print("HDOP: "); tft.println(hdop, 2);

      // Draw color indicator box
      tft.fillRect(180, 55, 40, 40, color);
      tft.drawRect(180, 55, 40, 40, ST77XX_WHITE);

      // Create JSON string
      char jsonBuf[128];
      snprintf(jsonBuf, sizeof(jsonBuf), "{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.2f,\"sats\":%d}", lat, lng, hdop, sats);
      
      // Broadcast to all connected WebSocket clients
      webSocket.broadcastTXT(jsonBuf);
      
    } else {
      // Still waiting for valid fix
      tft.fillRect(0, 55, 240, 80, ST77XX_BLACK);
      tft.setCursor(10, 55);
      tft.println("Searching satellites...");
      tft.setCursor(10, 75);
      tft.print("Sats in view: "); tft.println(gps.satellites.value());
      tft.setCursor(10, 95);
      tft.print("Chars RX: "); tft.println(gps.charsProcessed());
    }
  }
}
