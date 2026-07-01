// ============================================================
//  BIP26_GNSS_Heatmap — Sensors + SNR Edition
//  TU Dublin BIP26 SENSATE-X 2.0
//  Hardware: Tenstar TS-ESP32-S3 + Waveshare LC29H(AA)
//  Sensors:  BMP280 external (0x76), QMI8658C IMU (0x6B)
//  Display:  ST7789 135x240 landscape
// ============================================================

#include <Adafruit_BMP280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>
#include <SensorQMI8658.hpp> // SensorLib -- install via Library Manager
#include <TinyGPSPlus.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <Wire.h>

// -- Wi-Fi credentials
const char *ssid = "iPhone14";
const char *password = "12345678";

// -- Pin definitions (Tenstar TS-ESP32-S3, confirmed by Mike Gill TU Dublin)
#define TFT_CS 7
#define TFT_DC 39
#define TFT_RST 40
#define TFT_backlight 45
#define SPI_SCK 36
#define SPI_MISO 37
#define SPI_MOSI 35
#define I2C_SDA 42
#define I2C_SCL 41
#define GPS_RX 17 // Receives NMEA from LC29H (yellow wire)
#define GPS_TX 18 // Transmits to LC29H (blue wire)
#define GPS_BAUD 115200
#define BMP_ONBOARD 0x77
#define BMP_EXT 0x76
#define QMI_ADDR 0x6B // QMI8658C = QMI8658_L_SLAVE_ADDRESS

// -- Objects
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_BMP280 bmpOnboard;
Adafruit_BMP280 bmpExt;
SensorQMI8658 qmi;
TinyGPSPlus gps;
WebSocketsServer webSocket(81);

// -- Galileo GSV custom NMEA fields
// $GAGSV,totalMsg,msgNum,totalSats,[PRN,Elev,Azim,SNR]x4,...
// Field indices (1-based from sentence name as field 0):
//   PRN:  4,  8, 12, 16
//   Elev: 5,  9, 13, 17
//   Azim: 6, 10, 14, 18  (not parsed -- not needed for SNR vs Elevation study)
//   SNR:  7, 11, 15, 19
TinyGPSCustom gaTotalMsg(gps, "GAGSV", 1);
TinyGPSCustom gaMsgNum(gps, "GAGSV", 2);
TinyGPSCustom gaSatsTotal(gps, "GAGSV", 3);
TinyGPSCustom gaPRN1(gps, "GAGSV", 4);  // Satellite PRN (slot 1)
TinyGPSCustom gaElev1(gps, "GAGSV", 5); // Elevation deg (slot 1)
TinyGPSCustom gaSNR1(gps, "GAGSV", 7);  // C/N0 dB-Hz   (slot 1)
TinyGPSCustom gaPRN2(gps, "GAGSV", 8);
TinyGPSCustom gaElev2(gps, "GAGSV", 9);
TinyGPSCustom gaSNR2(gps, "GAGSV", 11);
TinyGPSCustom gaPRN3(gps, "GAGSV", 12);
TinyGPSCustom gaElev3(gps, "GAGSV", 13);
TinyGPSCustom gaSNR3(gps, "GAGSV", 15);
TinyGPSCustom gaPRN4(gps, "GAGSV", 16);
TinyGPSCustom gaElev4(gps, "GAGSV", 17);
TinyGPSCustom gaSNR4(gps, "GAGSV", 19);

float avgGalileoSNR = 0.0f;
int gaSatsInView = 0;
static float gaSnrSum = 0.0f;
static int gaSnrCount = 0;

// -- Sensor availability flags
bool bmpOnboardOK = false;
bool bmpExtOK = false;
bool qmiOK = false;

// -- Timing
unsigned long lastUpdate = 0;
unsigned long lastHistSave = 0;
const unsigned long UPDATE_INTERVAL = 1000; // 1 s display refresh
const unsigned long HIST_INTERVAL = 5000;   // 5 s history save -> 25 min max

// -- Telemetry history (stored in RAM, pushed to new WebSocket clients)
struct TelemetryPoint {
  double lat, lng, hdop;
  int sats;
  float temp, press, alt, speed, dist;
  float avgSNR;
  int gaSats;
  float ax, ay, az; // QMI8658C accelerometer (g)
};
TelemetryPoint history[300];
int histCount = 0;

// -- Dead-reckoning totals
double lastLat = 0.0;
double lastLng = 0.0;
float totalDistance = 0.0f; // metres accumulator

// =============================================================================
//  emitGAGSVSatellites()
//  Called once per $GAGSV sentence (every sentence, not just the last one).
//  Loops through all 4 satellite slots and emits a compact JSON record for
//  each slot that has a valid PRN + SNR, suitable for MATLAB scatter plots.
//
//  Output format (one line per satellite, to Serial AND WebSocket):
//    {"type":"sv","prn":<int>,"el":<int>,"snr":<int>,"t":<millis>}
//
//  Import into MATLAB with:
//    lines = readlines("session.json");
//    sv = lines(contains(lines, '"type":"sv"'));
// =============================================================================
void emitGAGSVSatellites() {
  // Pack parallel arrays so we can loop over slots 0-3
  const char *prnFields[4] = {gaPRN1.value(), gaPRN2.value(), gaPRN3.value(),
                              gaPRN4.value()};
  const char *elevFields[4] = {gaElev1.value(), gaElev2.value(),
                               gaElev3.value(), gaElev4.value()};
  const char *snrFields[4] = {gaSNR1.value(), gaSNR2.value(), gaSNR3.value(),
                              gaSNR4.value()};

  unsigned long t = millis();

  for (int i = 0; i < 4; i++) {
    // Skip empty slots — empty field means no satellite in this position
    if (prnFields[i][0] == '\0')
      continue;
    int prn = atoi(prnFields[i]);
    if (prn <= 0)
      continue; // PRN of 0 is invalid

    // SNR of 0 or empty means the satellite is in view but not tracked;
    // we still emit it with snr=0 so MATLAB sees the elevation without signal.
    int snr = (snrFields[i][0] != '\0') ? atoi(snrFields[i]) : 0;
    int elev = (elevFields[i][0] != '\0') ? atoi(elevFields[i]) : -1;

    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"sv\",\"prn\":%d,\"el\":%d,\"snr\":%d,\"t\":%lu}", prn,
             elev, snr, t);

    Serial.println(buf);         // Serial Monitor / file log
    webSocket.broadcastTXT(buf); // WebSocket backup log
  }
}

// =============================================================================
//  processGAGSV()
//  Called once per complete NMEA sentence (when gps.encode() returns true).
//  Accumulates SNR values across all GAGSV messages in one round, then
//  computes the average when the final message of the round arrives.
// =============================================================================
void processGAGSV() {
  if (!gaMsgNum.isUpdated())
    return; // No new GAGSV sentence -- skip

  int msgNum = atoi(gaMsgNum.value());
  int totalMsg = atoi(gaTotalMsg.value());
  if (msgNum <= 0 || totalMsg <= 0)
    return;

  // First message of a new round -- clear accumulators
  if (msgNum == 1) {
    gaSnrSum = 0.0f;
    gaSnrCount = 0;
  }

  // Emit per-satellite PRN/Elevation/SNR records for MATLAB logging.
  // Called on EVERY sentence so no satellite slot is missed across
  // multi-message rounds.
  emitGAGSVSatellites();

  // Accumulate SNR for the round average (existing behaviour)
  const char *snrFields[4] = {gaSNR1.value(), gaSNR2.value(), gaSNR3.value(),
                              gaSNR4.value()};
  for (int i = 0; i < 4; i++) {
    if (snrFields[i][0] != '\0') { // Non-empty field
      int snr = atoi(snrFields[i]);
      if (snr > 0) { // 0 = satellite not being tracked
        gaSnrSum += snr;
        gaSnrCount++;
      }
    }
  }

  gaSatsInView = atoi(gaSatsTotal.value());

  // Final message of the round -> publish the average
  if (msgNum == totalMsg) {
    avgGalileoSNR = (gaSnrCount > 0) ? (gaSnrSum / gaSnrCount) : 0.0f;
  }
}

// =============================================================================
//  WebSocket event handler
// =============================================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload,
                    size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.printf("[%u] Disconnected!\n", num);
    break;

  case WStype_CONNECTED: {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[%u] Connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2],
                  ip[3]);
    webSocket.sendTXT(num, "{\"status\":\"connected\"}");

    // Push session history so the map is rebuilt on reconnect
    for (int i = 0; i < histCount; i++) {
      char buf[300];
      snprintf(buf, sizeof(buf),
               "{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.2f,\"sats\":%d,"
               "\"temp\":%.1f,\"press\":%.1f,\"altM\":%.1f,\"speed\":%.1f,"
               "\"dist\":%.1f,\"avgSNR\":%.1f,\"gaSats\":%d,"
               "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f}",
               history[i].lat, history[i].lng, history[i].hdop, history[i].sats,
               history[i].temp, history[i].press, history[i].alt,
               history[i].speed, history[i].dist, history[i].avgSNR,
               history[i].gaSats, history[i].ax, history[i].ay, history[i].az);
      webSocket.sendTXT(num, buf);
      delay(5); // yield to network stack -- prevents TX buffer overrun
    }
    break;
  }

  case WStype_TEXT:
  case WStype_BIN:
  case WStype_ERROR:
  case WStype_FRAGMENT_TEXT_START:
  case WStype_FRAGMENT_BIN_START:
  case WStype_FRAGMENT:
  case WStype_FRAGMENT_FIN:
  default:
    break;
  }
}

// =============================================================================
//  setup()
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  // CRITICAL INIT SEQUENCE -- do not reorder
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS);
  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(TFT_backlight, OUTPUT);
  digitalWrite(TFT_backlight, HIGH);
  tft.init(135, 240);
  tft.setRotation(3); // Landscape: 240 wide x 135 tall
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 5);
  tft.println("GNSS Heatmap");
  tft.setTextSize(1);
  tft.setCursor(10, 30);
  tft.println("Connecting to Wi-Fi...");

  // BMP280 -- try external (0x76) first, fall back to onboard (0x77)
  bmpExtOK = bmpExt.begin(BMP_EXT);
  bmpOnboardOK = bmpOnboard.begin(BMP_ONBOARD);

  if (bmpExtOK) {
    bmpExt.setSampling(
        Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
        Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
        Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("BMP280 external (0x76) OK -- using for ambient temp");
  } else if (bmpOnboardOK) {
    bmpOnboard.setSampling(
        Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
        Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
        Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("BMP280 onboard (0x77) OK -- external not found");
  } else {
    Serial.println("ERROR: No BMP280 found -- check I2C wiring");
  }

  // QMI8658C IMU -- QMI8658_L_SLAVE_ADDRESS = 0x6B (defined inside SensorLib)
  qmiOK = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (qmiOK) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_1000Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.configGyroscope(SensorQMI8658::GYR_RANGE_64DPS,
                        SensorQMI8658::GYR_ODR_896_8Hz,
                        SensorQMI8658::LPF_MODE_3);
    qmi.enableAccelerometer();
    qmi.enableGyroscope();
    Serial.println("QMI8658C IMU OK");
  } else {
    Serial.println("ERROR: QMI8658C not found -- check I2C address 0x6B");
  }

  // Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // Static header -- drawn once; loop only redraws y >= 42
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 5);
  tft.println("GNSS Heatmap");
  tft.setTextSize(1);
  tft.setCursor(10, 28);
  tft.print("IP: ");
  tft.println(WiFi.localIP());
  tft.drawFastHLine(0, 38, 240, ST77XX_CYAN); // separator line
  tft.setCursor(10, 44);
  tft.println("Waiting for GPS fix...");
}

// =============================================================================
//  loop()
// =============================================================================
void loop() {
  webSocket.loop();

  // Feed every incoming byte to TinyGPSPlus.
  // gps.encode() returns true once a complete sentence has been parsed --
  // only THEN call processGAGSV() so it sees coherent field values.
  while (Serial1.available() > 0) {
    if (gps.encode(Serial1.read())) {
      processGAGSV();
    }
  }

  if (millis() - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = millis();

    // -- Read BMP280
    float tempC = 0.0f;
    float press = 0.0f;
    if (bmpExtOK) {
      tempC = bmpExt.readTemperature();
      press = bmpExt.readPressure() / 100.0f; // Pa -> hPa
    } else if (bmpOnboardOK) {
      tempC = bmpOnboard.readTemperature();
      press = bmpOnboard.readPressure() / 100.0f;
    }

    // -- Read QMI8658C accelerometer
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (qmiOK && qmi.getDataReady()) {
      qmi.getAccelerometer(ax, ay, az); // values in g
    }

    float altM = gps.altitude.meters();
    float speed = gps.speed.kmph();

    // =========================================================================
    //  GPS FIX BRANCH
    // =========================================================================
    if (gps.location.isValid() && gps.hdop.isValid()) {
      double lat = gps.location.lat();
      double lng = gps.location.lng();
      double hdop = gps.hdop.hdop();
      int sats = gps.satellites.value();

      // SMART FILTER 1: Speed deadband -- clamp jitter below 1.5 km/h
      if (speed < 1.5f)
        speed = 0.0f;

      // SMART FILTER 2: Spatial threshold + HDOP confidence gate
      if (lastLat != 0.0 && lastLng != 0.0) {
        double distMoved =
            TinyGPSPlus::distanceBetween(lastLat, lastLng, lat, lng);
        double minRadius = (hdop > 2.0) ? 4.0 : 2.5; // metres
        if (distMoved >= minRadius && speed > 0.0f) {
          totalDistance += distMoved;
          lastLat = lat;
          lastLng = lng;
        }
      } else {
        lastLat = lat;
        lastLng = lng;
      }

      // Save history point every 5 seconds
      if (millis() - lastHistSave > HIST_INTERVAL) {
        lastHistSave = millis();
        if (histCount < 300) {
          history[histCount++] = {
              lat,  lng,   hdop,          sats,          tempC,        press,
              altM, speed, totalDistance, avgGalileoSNR, gaSatsInView, ax,
              ay,   az};
        }
      }

      // HDOP colour for status box
      uint16_t hdopColor = ST77XX_RED;
      if (hdop < 1.5)
        hdopColor = ST77XX_GREEN;
      else if (hdop <= 2.5)
        hdopColor = ST77XX_ORANGE;

      // TFT: clear dynamic area
      // BUG FIX: height=93 fills exactly y=42 to y=134 (135px screen edge)
      tft.fillRect(0, 42, 240, 93, ST77XX_BLACK);

      // Row 1  y=44 : Latitude
      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(10, 44);
      tft.print("Lat: ");
      tft.print(lat, 6);

      // Row 2  y=56 : Longitude
      tft.setCursor(10, 56);
      tft.print("Lng: ");
      tft.print(lng, 6);

      // Row 3  y=68 : Satellites + HDOP
      tft.setCursor(10, 68);
      tft.print("Sats:");
      tft.print(sats);
      tft.print("  HDOP:");
      tft.print(hdop, 2);

      // HDOP colour box -- top-right corner, 22x22 px
      tft.fillRect(215, 44, 22, 22, hdopColor);
      tft.drawRect(215, 44, 22, 22, ST77XX_WHITE);

      // Row 4  y=80 : Galileo SNR (colour-coded by signal quality)
      tft.setCursor(10, 80);
      tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
      tft.print("Gal SNR:");
      if (avgGalileoSNR > 0.0f) {
        // Colour: green >=35 dB-Hz, orange 25-34, red <25
        uint16_t snrColor = (avgGalileoSNR >= 35)   ? ST77XX_GREEN
                            : (avgGalileoSNR >= 25) ? 0xFD20
                                                    : // orange
                                ST77XX_RED;
        tft.setTextColor(snrColor, ST77XX_BLACK);
        tft.print(avgGalileoSNR, 1);
        tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        tft.print("dB (");
        tft.print(gaSatsInView);
        tft.print("sv)");
      } else {
        tft.print("--");
      }

      // Row 5  y=92 : BMP280 Temperature + Pressure (magenta)
      tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
      tft.setCursor(10, 92);
      if (bmpExtOK || bmpOnboardOK) {
        tft.print(bmpExtOK ? "Ext:" : "Tmp:");
        tft.print(tempC, 1);
        tft.print("C ");
        tft.print(press, 0);
        tft.print("hPa");
      } else {
        tft.print("BMP280: N/A");
      }

      // Row 6  y=104 : QMI8658C Accelerometer X/Y/Z (cyan)
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 104);
      if (qmiOK) {
        tft.print("Ax:");
        tft.print(ax, 2);
        tft.print(" Ay:");
        tft.print(ay, 2);
        tft.print(" Az:");
        tft.print(az, 2);
      } else {
        tft.print("IMU: N/A");
      }

      // Row 7  y=116 : Speed + cumulative distance (white)
      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(10, 116);
      tft.print("Spd:");
      tft.print(speed, 1);
      tft.print("km/h  D:");
      tft.print(totalDistance / 1000.0f, 3);
      tft.print("km");

      // Row 8  y=127 : GPS altitude -- last usable pixel row on 135px screen
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 127);
      tft.print("Alt:");
      tft.print(altM, 0);
      tft.print("m");

      // JSON: broadcast full telemetry over WebSocket and Serial
      char jsonBuf[300];
      snprintf(jsonBuf, sizeof(jsonBuf),
               "{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.2f,\"sats\":%d,"
               "\"temp\":%.1f,\"press\":%.1f,\"altM\":%.1f,\"speed\":%.1f,"
               "\"dist\":%.1f,\"avgSNR\":%.1f,\"gaSats\":%d,"
               "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f}",
               lat, lng, hdop, sats, tempC, press, altM, speed, totalDistance,
               avgGalileoSNR, gaSatsInView, ax, ay, az);
      webSocket.broadcastTXT(jsonBuf);
      Serial.println(jsonBuf);

      // =========================================================================
      //  NO-FIX BRANCH -- still show sensors while waiting for satellites
      // =========================================================================
    } else {
      tft.fillRect(0, 42, 240, 93, ST77XX_BLACK);

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(10, 44);
      tft.println("Searching satellites...");
      tft.setCursor(10, 56);
      tft.print("Sats in view: ");
      tft.println(gps.satellites.value());
      tft.setCursor(10, 68);
      tft.print("Chars RX: ");
      tft.println(gps.charsProcessed());

      // BMP280 still visible during search
      tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
      tft.setCursor(10, 80);
      if (bmpExtOK || bmpOnboardOK) {
        tft.print(bmpExtOK ? "Ext:" : "Tmp:");
        tft.print(tempC, 1);
        tft.print("C  ");
        tft.print(press, 0);
        tft.print("hPa");
      } else {
        tft.print("BMP280: N/A");
      }

      // QMI8658C still visible during search
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 92);
      if (qmiOK) {
        tft.print("Ax:");
        tft.print(ax, 2);
        tft.print(" Ay:");
        tft.print(ay, 2);
        tft.print(" Az:");
        tft.print(az, 2);
      } else {
        tft.print("IMU: N/A");
      }

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    }
  }
}
