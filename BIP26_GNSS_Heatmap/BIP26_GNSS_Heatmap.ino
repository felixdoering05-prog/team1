#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TinyGPSPlus.h>

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

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 1000; // Update every second

void setup() {
  Serial.begin(115200); // USB Serial for PC Web App
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX); // GPS Module

  // CRITICAL INITIALISATION SEQUENCE
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
  tft.println("Waiting for GPS...");
}

void loop() {
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
      tft.fillRect(0, 30, 240, 105, ST77XX_BLACK); // Clear data area
      tft.setCursor(10, 40);
      tft.print("Lat: "); tft.println(lat, 6);
      tft.setCursor(10, 60);
      tft.print("Lng: "); tft.println(lng, 6);
      tft.setCursor(10, 80);
      tft.print("Sats: "); tft.println(sats);
      tft.setCursor(10, 100);
      tft.print("HDOP: "); tft.println(hdop, 2);

      // Draw color indicator box
      tft.fillRect(180, 40, 40, 40, color);
      tft.drawRect(180, 40, 40, 40, ST77XX_WHITE);

      // Print JSON over USB Serial for Web App
      Serial.print("{\"lat\":"); Serial.print(lat, 6);
      Serial.print(",\"lng\":"); Serial.print(lng, 6);
      Serial.print(",\"hdop\":"); Serial.print(hdop, 2);
      Serial.print(",\"sats\":"); Serial.print(sats);
      Serial.println("}");
    } else {
      // Still waiting for valid fix
      tft.fillRect(0, 30, 240, 105, ST77XX_BLACK);
      tft.setCursor(10, 40);
      tft.println("Searching satellites...");
      tft.setCursor(10, 60);
      tft.print("Sats in view: "); tft.println(gps.satellites.value());
      tft.setCursor(10, 80);
      tft.print("Chars RX: "); tft.println(gps.charsProcessed());
    }
  }
}
