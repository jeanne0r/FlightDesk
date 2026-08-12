#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#include "embedded_map.h"

namespace {

constexpr const char* kBuildName = "FlightDesk Waveshare 2.8C LCD test";
constexpr int kWidth = 480;
constexpr int kHeight = 480;

constexpr int kI2cScl = 7;
constexpr int kI2cSda = 15;
constexpr uint8_t kTca9554Address = 0x20;

constexpr int kLcdSck = 2;
constexpr int kLcdMosi = 1;
constexpr int kBacklightPin = 6;

constexpr uint8_t kExioLcdReset = 1;  // Waveshare LCD_RST = EXIO1
constexpr uint8_t kExioTouchReset = 2;  // Waveshare TP_RST = EXIO2 in Arduino demo
constexpr uint8_t kExioLcdCs = 3;     // Waveshare LCD_CS = EXIO3
constexpr uint8_t kExioBuzzer = 8;    // Waveshare buzzer = EXIO8
constexpr uint8_t kOutputMask =
    static_cast<uint8_t>((1U << (kExioLcdReset - 1)) |
                         (1U << (kExioTouchReset - 1)) |
                         (1U << (kExioLcdCs - 1)) |
                         (1U << (kExioBuzzer - 1)));

constexpr uint8_t kGt911AddressPrimary = 0x5D;
constexpr uint8_t kGt911AddressAlt = 0x14;
constexpr uint16_t kGt911ReadXyReg = 0x814E;
constexpr uint16_t kGt911ProductIdReg = 0x8140;
constexpr int kGt911IntPin = 16;
constexpr uint8_t kQmi8658Address = 0x6B;
constexpr uint8_t kQmi8658WhoAmI = 0x00;
constexpr uint8_t kQmi8658Ctrl1 = 0x02;
constexpr uint8_t kQmi8658Ctrl2 = 0x03;
constexpr uint8_t kQmi8658Ctrl3 = 0x04;
constexpr uint8_t kQmi8658Ctrl7 = 0x08;
constexpr uint8_t kQmi8658AxL = 0x35;

spi_device_handle_t lcdSpi = nullptr;
esp_lcd_panel_handle_t lcdPanel = nullptr;
uint16_t* frame = nullptr;
uint16_t* mapFrame = nullptr;
uint16_t* radarBaseFrame = nullptr;
uint8_t* radarAngleBins = nullptr;
uint8_t* tileBuffer = nullptr;
size_t tileBufferSize = 0;
uint8_t* jpegBuffer = nullptr;
size_t jpegBufferSize = 0;
uint16_t* selectedPhotoFrame = nullptr;
int selectedPhotoIndex = -1;
PNG png;
JPEGDEC jpeg;
Preferences prefs;
WebServer setupServer(80);

uint32_t lastStatusMs = 0;
uint32_t lastI2cScanMs = 0;
uint32_t lastRadarMs = 0;
uint32_t lastTouchPollMs = 0;
uint32_t lastImuMs = 0;
uint32_t lastMapFetchMs = 0;
uint32_t mapReloadAfterMs = 0;
uint32_t touchMarkerUntilMs = 0;
uint32_t lastTouchLogMs = 0;
uint32_t bootMs = 0;
float sweepDeg = -75.0f;
bool touchWasDown = false;
bool touchActive = false;
bool menuOpen = false;
int selectedPlane = -1;
uint16_t lastTouchX = 0;
uint16_t lastTouchY = 0;
uint32_t touchCount = 0;
bool gt911Ok = false;
bool qmiOk = false;
uint8_t gt911Address = kGt911AddressPrimary;
float accX = 0.0f;
float accY = 0.0f;
float accZ = 0.0f;
bool mapReady = false;
bool mapDirty = false;
bool radarBaseDirty = true;
bool mapFetchInProgress = false;
uint32_t lastTrafficFetchMs = 0;
bool trafficLive = false;
volatile bool trafficFetchRequested = false;
volatile bool trafficFetchInProgress = false;
volatile bool trafficFrameDirty = false;
TaskHandle_t trafficTaskHandle = nullptr;
volatile bool mapFetchRequested = false;
TaskHandle_t mapTaskHandle = nullptr;
volatile int enrichRequestedPlane = -1;
volatile bool enrichInProgress = false;
TaskHandle_t enrichTaskHandle = nullptr;
bool setupPortalActive = false;
bool otaStarted = false;
char storedSsid[33] = "";
char storedPassword[65] = "";
uint32_t wifiReconnectAtMs = 0;
bool dragCandidate = false;
bool mapDragging = false;
uint16_t touchStartX = 0;
uint16_t touchStartY = 0;
uint16_t dragLastX = 0;
uint16_t dragLastY = 0;

constexpr double kHomeLat = 46.5096;
constexpr double kHomeLon = 6.3077;
constexpr int kRangeKm = 50;
constexpr int kRadarCx = 240;
constexpr int kRadarCy = 240;
constexpr int kRadarRadius = 234;
constexpr int kSweepBins = 240;
constexpr int kSweepTrailBins = 43;
constexpr int kPhotoW = 106;
constexpr int kPhotoH = 70;
int pngTileScreenX = 0;
int pngTileScreenY = 0;
double radarLat = kHomeLat;
double radarLon = kHomeLon;
char currentPostal[8] = "1188";

struct PostalPreset {
  const char* code;
  const char* label;
  double lat;
  double lon;
};

constexpr PostalPreset kPostalPresets[] = {
    {"1188", "GIMEL", 46.5096, 6.3077},
    {"1201", "GENEVE", 46.2100, 6.1420},
    {"1003", "LAUSANNE", 46.5218, 6.6327},
    {"1260", "NYON", 46.3833, 6.2396},
    {"1110", "MORGES", 46.5110, 6.4985},
    {"2000", "NEUCHATEL", 46.9918, 6.9310},
    {"1700", "FRIBOURG", 46.8065, 7.1619}};
int postalIndex = 0;

struct RadarPlane {
  int x;
  int y;
  int heading;
  const char* callsign;
  const char* type;
  const char* route;
  int distanceKm;
  int altitudeM;
  int speedKmh;
};

constexpr RadarPlane kPlanes[] = {
    {122, 116, 32, "SWR3ZK", "AIRBUS A320", "Zurich -> Geneva", 24, 3200, 760},
    {342, 132, 120, "EZS51BG", "AIRBUS A320", "Nice -> Geneva", 36, 2100, 620},
    {190, 310, 205, "AFR45RG", "AIRBUS A320", "Athens -> Paris", 18, 12276, 840},
    {318, 332, 292, "LOT4HT", "BOEING 737", "Warsaw -> Geneva", 42, 6700, 690},
    {252, 162, 15, "HBK0J", "PILATUS PC-12", "Lausanne -> Sion", 8, 1450, 390},
    {106, 262, 278, "BAW74", "AIRBUS A319", "London -> Geneva", 28, 9100, 720},
    {388, 236, 86, "DLH8PN", "EMBRAER 195", "Munich -> Geneva", 50, 7800, 650}};

struct LivePlane {
  int x;
  int y;
  int heading;
  double lat;
  double lon;
  char hex[8];
  char callsign[12];
  char type[20];
  char route[36];
  char photo[16];
  char photoUrl[160];
  int distanceKm;
  int altitudeM;
  int speedKmh;
  bool enriched;
};

constexpr int kMaxLivePlanes = 36;
LivePlane livePlanes[kMaxLivePlanes];
int livePlaneCount = 0;

enum class AppView : uint8_t {
  Radar,
  Search,
  Favorites,
  Settings,
  Assistant
};

AppView appView = AppView::Radar;

const char* activeWifiSsid();
void requestPlaneEnrich(int index);

void pcaWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kTca9554Address);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t pcaRead(uint8_t reg) {
  Wire.beginTransmission(kTca9554Address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return 0xFF;
  }
  Wire.requestFrom(kTca9554Address, static_cast<uint8_t>(1));
  return Wire.available() ? Wire.read() : 0xFF;
}

void setExio(uint8_t pin, bool high) {
  uint8_t output = pcaRead(0x01);
  const uint8_t mask = static_cast<uint8_t>(1U << (pin - 1));
  output = high ? static_cast<uint8_t>(output | mask)
                : static_cast<uint8_t>(output & ~mask);
  pcaWrite(0x01, output);
}

bool touchI2cReadAt(uint8_t address, uint16_t reg, uint8_t* data, size_t len) {
  Wire.beginTransmission(address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  const uint8_t got = Wire.requestFrom(address, static_cast<uint8_t>(len));
  if (got != len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

bool touchI2cRead(uint16_t reg, uint8_t* data, size_t len) {
  return touchI2cReadAt(gt911Address, reg, data, len);
}

bool touchI2cWrite(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(gt911Address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool i2cRead8(uint8_t address, uint8_t reg, uint8_t* data, size_t len) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  const uint8_t got = Wire.requestFrom(address, static_cast<uint8_t>(len));
  if (got != len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

bool i2cWrite8(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

void initExio() {
  uint8_t output = 0xFF;
  output &= static_cast<uint8_t>(~(1U << (kExioLcdCs - 1)));      // LCD CS active
  output &= static_cast<uint8_t>(~(1U << (kExioLcdReset - 1)));   // LCD reset low
  output &= static_cast<uint8_t>(~(1U << (kExioBuzzer - 1)));     // buzzer off
  pcaWrite(0x01, output);
  pcaWrite(0x03, static_cast<uint8_t>(0xFF & ~kOutputMask));
}

void initTouch() {
  Serial.println("[TOUCH] Init GT911");
  pinMode(kGt911IntPin, OUTPUT);
  digitalWrite(kGt911IntPin, LOW);
  setExio(kExioTouchReset, false);
  delay(10);
  setExio(kExioTouchReset, true);
  delay(220);
  digitalWrite(kGt911IntPin, HIGH);
  pinMode(kGt911IntPin, INPUT);

  uint8_t product[4] = {};
  gt911Address = kGt911AddressPrimary;
  gt911Ok = touchI2cReadAt(gt911Address, kGt911ProductIdReg, product, 4);
  if (!gt911Ok) {
    gt911Address = kGt911AddressAlt;
    gt911Ok = touchI2cReadAt(gt911Address, kGt911ProductIdReg, product, 4);
  }
  if (gt911Ok) {
    Serial.printf("[TOUCH] GT911 address=0x%02X product=%c%c%c%c\n",
                  gt911Address, product[0], product[1], product[2], product[3]);
  } else {
    Serial.println("[TOUCH] GT911 absent ou lecture KO");
  }
}

void initImu() {
  Serial.println("[IMU] Init QMI8658");
  uint8_t who = 0;
  qmiOk = i2cRead8(kQmi8658Address, kQmi8658WhoAmI, &who, 1);
  if (!qmiOk) {
    Serial.println("[IMU] QMI8658 absent ou lecture KO");
    return;
  }

  Serial.printf("[IMU] QMI8658 who=0x%02X\n", who);
  qmiOk &= i2cWrite8(kQmi8658Address, kQmi8658Ctrl1, 0x40);  // auto increment
  qmiOk &= i2cWrite8(kQmi8658Address, kQmi8658Ctrl2, 0x21);  // +/-4g, low ODR
  qmiOk &= i2cWrite8(kQmi8658Address, kQmi8658Ctrl3, 0x21);  // gyro configured
  qmiOk &= i2cWrite8(kQmi8658Address, kQmi8658Ctrl7, 0x03);  // accel + gyro
}

void pollImu() {
  if (!qmiOk) return;
  uint8_t data[6] = {};
  if (!i2cRead8(kQmi8658Address, kQmi8658AxL, data, sizeof(data))) {
    qmiOk = false;
    return;
  }
  const int16_t rawX = static_cast<int16_t>((data[1] << 8) | data[0]);
  const int16_t rawY = static_cast<int16_t>((data[3] << 8) | data[2]);
  const int16_t rawZ = static_cast<int16_t>((data[5] << 8) | data[4]);
  constexpr float scale = 4.0f / 32768.0f;
  accX = rawX * scale;
  accY = rawY * scale;
  accZ = rawZ * scale;
}

bool readTouch(uint16_t& x, uint16_t& y) {
  uint8_t status = 0;
  if (!touchI2cRead(kGt911ReadXyReg, &status, 1)) {
    return false;
  }
  if ((status & 0x80) == 0) {
    return false;
  }
  const uint8_t points = status & 0x0F;
  if (points == 0 || points > 5) {
    touchI2cWrite(kGt911ReadXyReg, 0);
    return false;
  }

  uint8_t data[8] = {};
  if (!touchI2cRead(kGt911ReadXyReg + 1, data, sizeof(data))) {
    touchI2cWrite(kGt911ReadXyReg, 0);
    return false;
  }
  touchI2cWrite(kGt911ReadXyReg, 0);
  // Starting at 0x814F: track id, X low, X high, Y low, Y high, size low, size high.
  x = static_cast<uint16_t>(data[2] << 8 | data[1]);
  y = static_cast<uint16_t>(data[4] << 8 | data[3]);
  if (x >= kWidth) x = kWidth - 1;
  if (y >= kHeight) y = kHeight - 1;
  return true;
}

void lcdWriteCommand(uint8_t cmd) {
  spi_transaction_t transaction = {};
  transaction.cmd = 0;
  transaction.addr = cmd;
  transaction.length = 0;
  transaction.rxlength = 0;
  spi_device_transmit(lcdSpi, &transaction);
}

void lcdWriteData(uint8_t data) {
  spi_transaction_t transaction = {};
  transaction.cmd = 1;
  transaction.addr = data;
  transaction.length = 0;
  transaction.rxlength = 0;
  spi_device_transmit(lcdSpi, &transaction);
}

void lcdCommandData(uint8_t cmd, const uint8_t* data, size_t len) {
  lcdWriteCommand(cmd);
  for (size_t i = 0; i < len; ++i) {
    lcdWriteData(data[i]);
  }
}

void lcdReset() {
  setExio(kExioLcdReset, false);
  delay(10);
  setExio(kExioLcdReset, true);
  delay(50);
}

void initSt7701Registers() {
  const uint8_t cmd_ff_13[] = {0x77, 0x01, 0x00, 0x00, 0x13};
  lcdCommandData(0xFF, cmd_ff_13, sizeof(cmd_ff_13));
  const uint8_t ef1[] = {0x08};
  lcdCommandData(0xEF, ef1, sizeof(ef1));

  const uint8_t cmd_ff_10[] = {0x77, 0x01, 0x00, 0x00, 0x10};
  lcdCommandData(0xFF, cmd_ff_10, sizeof(cmd_ff_10));
  const uint8_t c0[] = {0x3B, 0x00};
  const uint8_t c1[] = {0x10, 0x0C};
  const uint8_t c2[] = {0x07, 0x0A};
  const uint8_t c7[] = {0x00};
  const uint8_t cc[] = {0x10};
  const uint8_t cd[] = {0x08};
  lcdCommandData(0xC0, c0, sizeof(c0));
  lcdCommandData(0xC1, c1, sizeof(c1));
  lcdCommandData(0xC2, c2, sizeof(c2));
  lcdCommandData(0xC7, c7, sizeof(c7));
  lcdCommandData(0xCC, cc, sizeof(cc));
  lcdCommandData(0xCD, cd, sizeof(cd));

  const uint8_t b0[] = {
      0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09,
      0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11};
  const uint8_t b1[] = {
      0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08,
      0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F};
  lcdCommandData(0xB0, b0, sizeof(b0));
  lcdCommandData(0xB1, b1, sizeof(b1));

  const uint8_t cmd_ff_11[] = {0x77, 0x01, 0x00, 0x00, 0x11};
  lcdCommandData(0xFF, cmd_ff_11, sizeof(cmd_ff_11));
  const uint8_t b0p1[] = {0x5D};
  const uint8_t b1p1[] = {0x3E};
  const uint8_t b2p1[] = {0x81};
  const uint8_t b3p1[] = {0x80};
  const uint8_t b5p1[] = {0x4E};
  const uint8_t b7p1[] = {0x85};
  const uint8_t b8p1[] = {0x20};
  const uint8_t c1p1[] = {0x78};
  const uint8_t c2p1[] = {0x78};
  const uint8_t d0p1[] = {0x88};
  lcdCommandData(0xB0, b0p1, sizeof(b0p1));
  lcdCommandData(0xB1, b1p1, sizeof(b1p1));
  lcdCommandData(0xB2, b2p1, sizeof(b2p1));
  lcdCommandData(0xB3, b3p1, sizeof(b3p1));
  lcdCommandData(0xB5, b5p1, sizeof(b5p1));
  lcdCommandData(0xB7, b7p1, sizeof(b7p1));
  lcdCommandData(0xB8, b8p1, sizeof(b8p1));
  lcdCommandData(0xC1, c1p1, sizeof(c1p1));
  lcdCommandData(0xC2, c2p1, sizeof(c2p1));
  lcdCommandData(0xD0, d0p1, sizeof(d0p1));

  const uint8_t e0[] = {0x00, 0x00, 0x02};
  const uint8_t e1[] = {0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07, 0x30, 0x00, 0x33, 0x33};
  const uint8_t e2[] = {0x11, 0x11, 0x33, 0x33, 0xF4, 0x00, 0x00, 0x00, 0xF4, 0x00, 0x00, 0x00};
  const uint8_t e3[] = {0x00, 0x00, 0x11, 0x11};
  const uint8_t e4[] = {0x44, 0x44};
  const uint8_t e5[] = {0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0,
                        0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0};
  const uint8_t e6[] = {0x00, 0x00, 0x11, 0x11};
  const uint8_t e7[] = {0x44, 0x44};
  const uint8_t e8[] = {0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0,
                        0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0};
  const uint8_t e9[] = {0x36, 0x01};
  const uint8_t eb[] = {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40};
  const uint8_t ed[] = {0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF,
                        0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF};
  const uint8_t ef2[] = {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54};
  lcdCommandData(0xE0, e0, sizeof(e0));
  lcdCommandData(0xE1, e1, sizeof(e1));
  lcdCommandData(0xE2, e2, sizeof(e2));
  lcdCommandData(0xE3, e3, sizeof(e3));
  lcdCommandData(0xE4, e4, sizeof(e4));
  lcdCommandData(0xE5, e5, sizeof(e5));
  lcdCommandData(0xE6, e6, sizeof(e6));
  lcdCommandData(0xE7, e7, sizeof(e7));
  lcdCommandData(0xE8, e8, sizeof(e8));
  lcdCommandData(0xE9, e9, sizeof(e9));
  lcdCommandData(0xEB, eb, sizeof(eb));
  lcdCommandData(0xED, ed, sizeof(ed));
  lcdCommandData(0xEF, ef2, sizeof(ef2));

  const uint8_t cmd_ff_00[] = {0x77, 0x01, 0x00, 0x00, 0x00};
  lcdCommandData(0xFF, cmd_ff_00, sizeof(cmd_ff_00));
  lcdWriteCommand(0x11);
  delay(120);
  const uint8_t colmod[] = {0x66};
  const uint8_t madctl[] = {0x00};
  const uint8_t teon[] = {0x00};
  lcdCommandData(0x3A, colmod, sizeof(colmod));
  lcdCommandData(0x36, madctl, sizeof(madctl));
  lcdCommandData(0x35, teon, sizeof(teon));
  lcdWriteCommand(0x29);
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void putPixel(int x, int y, uint16_t color) {
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  frame[y * kWidth + x] = color;
}

uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha) {
  const uint8_t inv = 255 - alpha;
  const uint8_t dr = ((dst >> 11) & 0x1F) << 3;
  const uint8_t dg = ((dst >> 5) & 0x3F) << 2;
  const uint8_t db = (dst & 0x1F) << 3;
  const uint8_t sr = ((src >> 11) & 0x1F) << 3;
  const uint8_t sg = ((src >> 5) & 0x3F) << 2;
  const uint8_t sb = (src & 0x1F) << 3;
  return rgb565((dr * inv + sr * alpha) / 255,
                (dg * inv + sg * alpha) / 255,
                (db * inv + sb * alpha) / 255);
}

void blendPixel(int x, int y, uint16_t color, uint8_t alpha) {
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  const int index = y * kWidth + x;
  frame[index] = blend565(frame[index], color, alpha);
}

void blendEmbeddedMap(uint8_t alpha) {
  if (!frame) return;

  int pixel = 0;
  for (size_t i = 0; i + 4 < sizeof(kEmbeddedMapPacked); i += 5) {
    uint64_t packed = 0;
    for (int b = 0; b < 5; ++b) {
      packed = (packed << 8) | pgm_read_byte(&kEmbeddedMapPacked[i + b]);
    }

    for (int shift = 35; shift >= 0; shift -= 5) {
      if (pixel >= kWidth * kHeight) return;
      const int x = pixel % kWidth;
      const int y = pixel / kWidth;
      const int dx = x - kRadarCx;
      const int dy = y - kRadarCy;
      if (dx * dx + dy * dy <= (kRadarRadius - 2) * (kRadarRadius - 2)) {
        const uint8_t paletteIndex = static_cast<uint8_t>((packed >> shift) & 0x1F);
        const uint16_t color = pgm_read_word(&kEmbeddedMapPalette[paletteIndex]);
        frame[pixel] = blend565(frame[pixel], color, alpha);
      }
      ++pixel;
    }
  }
}

void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    putPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void blendLine(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    blendPixel(x0, y0, color, alpha);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void blendThickLine(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha) {
  blendLine(x0, y0, x1, y1, color, alpha);
  blendLine(x0 + 1, y0, x1 + 1, y1, color, alpha);
  blendLine(x0 - 1, y0, x1 - 1, y1, color, alpha);
  blendLine(x0, y0 + 1, x1, y1 + 1, color, alpha);
  blendLine(x0, y0 - 1, x1, y1 - 1, color, alpha);
}

void drawThickLine(int x0, int y0, int x1, int y1, uint16_t color) {
  drawLine(x0, y0, x1, y1, color);
  drawLine(x0 + 1, y0, x1 + 1, y1, color);
  drawLine(x0 - 1, y0, x1 - 1, y1, color);
  drawLine(x0, y0 + 1, x1, y1 + 1, color);
  drawLine(x0, y0 - 1, x1, y1 - 1, color);
}

void drawCircle(int cx, int cy, int r, uint16_t color) {
  int x = -r, y = 0, err = 2 - 2 * r;
  do {
    putPixel(cx - x, cy + y, color);
    putPixel(cx - y, cy - x, color);
    putPixel(cx + x, cy - y, color);
    putPixel(cx + y, cy + x, color);
    int e2 = err;
    if (e2 <= y) err += ++y * 2 + 1;
    if (e2 > x || err > y) err += ++x * 2 + 1;
  } while (x < 0);
}

void blendCircle(int cx, int cy, int r, uint16_t color, uint8_t alpha) {
  int x = -r, y = 0, err = 2 - 2 * r;
  do {
    blendPixel(cx - x, cy + y, color, alpha);
    blendPixel(cx - y, cy - x, color, alpha);
    blendPixel(cx + x, cy - y, color, alpha);
    blendPixel(cx + y, cy + x, color, alpha);
    int e2 = err;
    if (e2 <= y) err += ++y * 2 + 1;
    if (e2 > x || err > y) err += ++x * 2 + 1;
  } while (x < 0);
}

void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
  int minX = min(x0, min(x1, x2));
  int maxX = max(x0, max(x1, x2));
  int minY = min(y0, min(y1, y2));
  int maxY = max(y0, max(y1, y2));
  const int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
  if (area == 0) return;

  for (int y = minY; y <= maxY; ++y) {
    for (int x = minX; x <= maxX; ++x) {
      const int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
      const int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
      const int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
      if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
          (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
        putPixel(x, y, color);
      }
    }
  }
}

void fillCircle(int cx, int cy, int r, uint16_t color) {
  for (int y = -r; y <= r; ++y) {
    const int span = static_cast<int>(sqrtf(r * r - y * y));
    for (int x = -span; x <= span; ++x) {
      putPixel(cx + x, cy + y, color);
    }
  }
}

void blendFillCircle(int cx, int cy, int r, uint16_t color, uint8_t alpha) {
  for (int y = -r; y <= r; ++y) {
    const int span = static_cast<int>(sqrtf(r * r - y * y));
    for (int x = -span; x <= span; ++x) {
      blendPixel(cx + x, cy + y, color, alpha);
    }
  }
}

void blendRoundRect(int x, int y, int w, int h, int radius, uint16_t color, uint8_t alpha) {
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) {
      const int anchorX = xx < x + radius ? x + radius : (xx >= x + w - radius ? x + w - radius - 1 : xx);
      const int anchorY = yy < y + radius ? y + radius : (yy >= y + h - radius ? y + h - radius - 1 : yy);
      const int dx = xx - anchorX;
      const int dy = yy - anchorY;
      if (dx * dx + dy * dy <= radius * radius) {
        blendPixel(xx, yy, color, alpha);
      }
    }
  }
}

void putMapPixel(int x, int y, uint16_t color) {
  if (!mapFrame || x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  mapFrame[y * kWidth + x] = color;
}

uint16_t radarTint(uint16_t rgb) {
  const uint8_t r = ((rgb >> 11) & 0x1F) << 3;
  const uint8_t g = ((rgb >> 5) & 0x3F) << 2;
  const uint8_t b = (rgb & 0x1F) << 3;
  const uint8_t lum = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
  return rgb565(1 + lum / 42, 6 + lum / 10, 9 + lum / 13);
}

void clearMapFrame() {
  if (!mapFrame) return;
  for (int i = 0; i < kWidth * kHeight; ++i) {
    mapFrame[i] = rgb565(0, 6, 8);
  }
}

int pngDrawTile(PNGDRAW* draw) {
  static uint16_t line[256];
  png.getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
  const int y = pngTileScreenY + draw->y;
  if (y < 0 || y >= kHeight) return 1;
  for (int x = 0; x < draw->iWidth; ++x) {
    const int sx = pngTileScreenX + x;
    if (sx < 0 || sx >= kWidth) continue;
    putMapPixel(sx, y, radarTint(line[x]));
  }
  return 1;
}

double lonToPixelX(double lon, int zoom) {
  const double scale = 256.0 * static_cast<double>(1UL << zoom);
  return ((lon + 180.0) / 360.0) * scale;
}

double latToPixelY(double lat, int zoom) {
  const double sinLat = sin(lat * DEG_TO_RAD);
  const double scale = 256.0 * static_cast<double>(1UL << zoom);
  return (0.5 - log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * PI)) * scale;
}

int zoomForRange(double lat, int rangeKm, int radiusPx) {
  const double metersPerPixel = (rangeKm * 1000.0) / max(1, radiusPx);
  const double target = 156543.03392 * cos(lat * DEG_TO_RAD) / metersPerPixel;
  int zoom = static_cast<int>(round(log(target) / log(2.0)));
  return constrain(zoom, 6, 13);
}

bool fetchTilePng(int z, int x, int y, int screenX, int screenY) {
  if (!tileBuffer || WiFi.status() != WL_CONNECTED) return false;
  const int tileCount = 1 << z;
  x = (x % tileCount + tileCount) % tileCount;
  if (y < 0 || y >= tileCount) return false;

  char url[96];
  snprintf(url, sizeof(url), "http://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);
  WiFiClient client;
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[MAP] Tile HTTP %d %s\n", code, url);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  const int contentLength = http.getSize();
  size_t total = 0;
  const uint32_t started = millis();
  while (total < tileBufferSize && millis() - started < 12000) {
    const size_t available = stream->available();
    if (available == 0) {
      if (contentLength > 0 && total >= static_cast<size_t>(contentLength)) break;
      if (total > 0 && !stream->connected()) break;
      delay(1);
      continue;
    }
    const size_t room = tileBufferSize - total;
    const size_t chunk = min(available, room);
    const int read = stream->readBytes(tileBuffer + total, chunk);
    if (read <= 0) break;
    total += read;
    if (contentLength > 0 && total >= static_cast<size_t>(contentLength)) break;
  }
  http.end();
  if (total < 64 || total >= tileBufferSize) {
    Serial.printf("[MAP] Tile size KO z=%d x=%d y=%d bytes=%u\n", z, x, y, static_cast<unsigned>(total));
    return false;
  }

  pngTileScreenX = screenX;
  pngTileScreenY = screenY;
  if (png.openRAM(tileBuffer, total, pngDrawTile) != PNG_SUCCESS) {
    Serial.printf("[MAP] PNG decode open KO z=%d x=%d y=%d bytes=%u\n", z, x, y, static_cast<unsigned>(total));
    return false;
  }
  const int decoded = png.decode(nullptr, 0);
  png.close();
  if (decoded != PNG_SUCCESS) {
    Serial.printf("[MAP] PNG decode KO z=%d x=%d y=%d err=%d\n", z, x, y, decoded);
    return false;
  }
  return true;
}

void fetchMapTiles() {
  if (!mapFrame || !tileBuffer || WiFi.status() != WL_CONNECTED || mapFetchInProgress) return;
  mapFetchInProgress = true;
  clearMapFrame();
  const int zoom = zoomForRange(radarLat, kRangeKm, kRadarRadius);
  const double centerX = lonToPixelX(radarLon, zoom);
  const double centerY = latToPixelY(radarLat, zoom);
  const double topLeftX = centerX - kRadarCx;
  const double topLeftY = centerY - kRadarCy;
  const int minTileX = floor(topLeftX / 256.0);
  const int minTileY = floor(topLeftY / 256.0);
  const int maxTileX = floor((topLeftX + kWidth) / 256.0);
  const int maxTileY = floor((topLeftY + kHeight) / 256.0);
  int ok = 0;
  int total = 0;
  Serial.printf("[MAP] Fetch OSM z=%d center=%.5f,%.5f npa=%s\n", zoom, radarLat, radarLon, currentPostal);
  for (int ty = minTileY; ty <= maxTileY; ++ty) {
    for (int tx = minTileX; tx <= maxTileX; ++tx) {
      const int screenX = static_cast<int>(round(tx * 256.0 - topLeftX));
      const int screenY = static_cast<int>(round(ty * 256.0 - topLeftY));
      ++total;
      if (fetchTilePng(zoom, tx, ty, screenX, screenY)) {
        ++ok;
      }
      delay(80);
    }
  }
  mapReady = ok > 0;
  if (mapReady) {
    mapDirty = false;
  }
  radarBaseDirty = true;
  mapFetchInProgress = false;
  Serial.printf("[MAP] Tiles %d/%d ready=%d\n", ok, total, mapReady);
}

double distanceKm(double lat1, double lon1, double lat2, double lon2) {
  const double dLat = (lat2 - lat1) * DEG_TO_RAD;
  const double dLon = (lon2 - lon1) * DEG_TO_RAD;
  const double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
                   cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
                       sin(dLon / 2.0) * sin(dLon / 2.0);
  return 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  const double y = sin((lon2 - lon1) * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD);
  const double x = cos(lat1 * DEG_TO_RAD) * sin(lat2 * DEG_TO_RAD) -
                   sin(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
                       cos((lon2 - lon1) * DEG_TO_RAD);
  double bearing = atan2(y, x) / DEG_TO_RAD;
  if (bearing < 0.0) bearing += 360.0;
  return bearing;
}

void copyClean(char* dst, size_t dstLen, const char* src, const char* fallback) {
  if (!dst || dstLen == 0) return;
  const char* value = (src && strlen(src)) ? src : fallback;
  size_t out = 0;
  while (value && value[0] && out + 1 < dstLen) {
    const char c = *value++;
    if (c >= 32 && c <= 126) {
      dst[out++] = c;
    }
  }
  while (out > 0 && dst[out - 1] == ' ') --out;
  dst[out] = '\0';
}

const char* airportCity(const char* code) {
  if (!code || strlen(code) < 3) return nullptr;
  if (!strncmp(code, "GVA", 3)) return "Geneva";
  if (!strncmp(code, "ZRH", 3)) return "Zurich";
  if (!strncmp(code, "BSL", 3)) return "Basel";
  if (!strncmp(code, "CDG", 3) || !strncmp(code, "ORY", 3)) return "Paris";
  if (!strncmp(code, "NCE", 3)) return "Nice";
  if (!strncmp(code, "LHR", 3) || !strncmp(code, "LGW", 3)) return "London";
  if (!strncmp(code, "AMS", 3)) return "Amsterdam";
  if (!strncmp(code, "FRA", 3)) return "Frankfurt";
  if (!strncmp(code, "MUC", 3)) return "Munich";
  if (!strncmp(code, "MAD", 3)) return "Madrid";
  if (!strncmp(code, "BCN", 3)) return "Barcelona";
  if (!strncmp(code, "ATH", 3)) return "Athens";
  if (!strncmp(code, "MXP", 3) || !strncmp(code, "LIN", 3)) return "Milan";
  if (!strncmp(code, "WAW", 3)) return "Warsaw";
  return nullptr;
}

void formatRoute(char* dst, size_t dstLen, const char* rawRoute) {
  if (!rawRoute || !strlen(rawRoute)) {
    copyClean(dst, dstLen, "Depart -> Arrivee inconnus", "");
    return;
  }

  char route[24];
  copyClean(route, sizeof(route), rawRoute, "");
  char* sep = strchr(route, '-');
  if (!sep) sep = strchr(route, '>');
  if (!sep || sep == route || !sep[1]) {
    copyClean(dst, dstLen, route, "Depart -> Arrivee inconnus");
    return;
  }
  *sep = '\0';
  const char* from = airportCity(route);
  const char* to = airportCity(sep + 1);
  snprintf(dst, dstLen, "%s -> %s", from ? from : route, to ? to : sep + 1);
}

const RadarPlane& fallbackPlane(int index) {
  return kPlanes[index % (sizeof(kPlanes) / sizeof(kPlanes[0]))];
}

int aircraftCount() {
  return trafficLive && livePlaneCount > 0 ? livePlaneCount
                                           : static_cast<int>(sizeof(kPlanes) / sizeof(kPlanes[0]));
}

int planeX(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].x : fallbackPlane(index).x;
}

int planeY(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].y : fallbackPlane(index).y;
}

int planeHeading(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].heading : fallbackPlane(index).heading;
}

const char* planeCallsign(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].callsign : fallbackPlane(index).callsign;
}

const char* planeType(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].type : fallbackPlane(index).type;
}

const char* planeRoute(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].route : fallbackPlane(index).route;
}

int planeDistanceKm(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].distanceKm : fallbackPlane(index).distanceKm;
}

int planeAltitudeM(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].altitudeM : fallbackPlane(index).altitudeM;
}

int planeSpeedKmh(int index) {
  return trafficLive && livePlaneCount > 0 ? livePlanes[index].speedKmh : fallbackPlane(index).speedKmh;
}

bool updateLivePlanePosition(LivePlane& p) {
  const double dist = distanceKm(radarLat, radarLon, p.lat, p.lon);
  if (dist > kRangeKm || dist < 0.1) return false;
  const double bearing = bearingDeg(radarLat, radarLon, p.lat, p.lon);
  const double posR = (dist / kRangeKm) * (kRadarRadius - 26);
  p.x = kRadarCx + static_cast<int>(sin(bearing * DEG_TO_RAD) * posR);
  p.y = kRadarCy - static_cast<int>(cos(bearing * DEG_TO_RAD) * posR);
  p.distanceKm = static_cast<int>(round(dist));
  return true;
}

void updateLivePlanePositions() {
  if (!trafficLive || livePlaneCount <= 0) return;
  int write = 0;
  for (int read = 0; read < livePlaneCount; ++read) {
    LivePlane p = livePlanes[read];
    if (!updateLivePlanePosition(p)) continue;
    livePlanes[write++] = p;
  }
  livePlaneCount = write;
  trafficLive = livePlaneCount > 0;
  if (selectedPlane >= livePlaneCount) selectedPlane = -1;
}

void fetchTraffic() {
  if (WiFi.status() != WL_CONNECTED) {
    trafficLive = false;
    livePlaneCount = 0;
    return;
  }

  char url[128];
  const int radiusNm = max(1, static_cast<int>(round(kRangeKm / 1.852)));
  snprintf(url, sizeof(url), "https://api.airplanes.live/v2/point/%.5f/%.5f/%d",
           radarLat, radarLon, radiusNm);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setTimeout(4500);
  if (!http.begin(client, url)) {
    Serial.println("[TRAFFIC] HTTP begin KO");
    return;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[TRAFFIC] HTTP KO code=%d\n", code);
    http.end();
    return;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[TRAFFIC] JSON KO %s bytes=%u\n", err.c_str(), payload.length());
    return;
  }

  JsonArray aircraft = doc["ac"].as<JsonArray>();
  static LivePlane fetchedPlanes[kMaxLivePlanes];
  int count = 0;
  for (JsonObject ac : aircraft) {
    if (count >= kMaxLivePlanes) break;
    if (!ac["lat"].is<double>() || !ac["lon"].is<double>()) continue;

    const double lat = ac["lat"].as<double>();
    const double lon = ac["lon"].as<double>();
    LivePlane& p = fetchedPlanes[count];
    p.lat = lat;
    p.lon = lon;
    if (!updateLivePlanePosition(p)) continue;
    const double bearing = bearingDeg(radarLat, radarLon, lat, lon);
    p.heading = static_cast<int>(round(ac["track"] | ac["true_heading"] | bearing)) - 90;
    copyClean(p.hex, sizeof(p.hex), ac["hex"] | "", "");
    copyClean(p.callsign, sizeof(p.callsign), ac["flight"] | ac["r"] | ac["hex"] | "", "VOL");
    copyClean(p.type, sizeof(p.type), ac["t"] | ac["desc"] | "", "TYPE INCONNU");
    formatRoute(p.route, sizeof(p.route), ac["route"] | "");
    copyClean(p.photo, sizeof(p.photo), "", "INFO...");
    p.photoUrl[0] = '\0';
    p.altitudeM = static_cast<int>(round((ac["alt_baro"] | ac["alt_geom"] | 0) * 0.3048));
    p.speedKmh = static_cast<int>(round((ac["gs"] | 0) * 1.852));
    p.enriched = false;
    ++count;
  }

  for (int i = 0; i < count; ++i) {
    livePlanes[i] = fetchedPlanes[i];
  }
  livePlaneCount = count;
  trafficLive = count > 0;
  if (selectedPlane >= aircraftCount()) selectedPlane = -1;
  trafficFrameDirty = true;
  Serial.printf("[TRAFFIC] %d avion(s) live via airplanes.live\n", livePlaneCount);
}

bool httpsGetJson(const char* url, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setTimeout(7000);
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();
  return !deserializeJson(doc, payload);
}

int jpegDrawPhoto(JPEGDRAW* draw) {
  if (!selectedPhotoFrame) return 0;
  const int dstX = draw->x;
  const int dstY = draw->y;
  for (int y = 0; y < draw->iHeight; ++y) {
    const int py = dstY + y;
    if (py < 0 || py >= kPhotoH) continue;
    for (int x = 0; x < draw->iWidth; ++x) {
      const int px = dstX + x;
      if (px < 0 || px >= kPhotoW) continue;
      selectedPhotoFrame[py * kPhotoW + px] = draw->pPixels[y * draw->iWidth + x];
    }
  }
  return 1;
}

void clearSelectedPhoto(uint16_t color = rgb565(5, 28, 22)) {
  if (!selectedPhotoFrame) return;
  selectedPhotoIndex = -1;
  for (int i = 0; i < kPhotoW * kPhotoH; ++i) {
    selectedPhotoFrame[i] = color;
  }
}

bool downloadSelectedPhoto(const char* url) {
  if (!url || !strlen(url) || !jpegBuffer || !selectedPhotoFrame || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(5500);
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  const int contentLength = http.getSize();
  size_t total = 0;
  const uint32_t started = millis();
  while (total < jpegBufferSize && millis() - started < 7000) {
    const size_t available = stream->available();
    if (available == 0) {
      if (contentLength > 0 && total >= static_cast<size_t>(contentLength)) break;
      if (total > 0 && !stream->connected()) break;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    const size_t chunk = min(available, jpegBufferSize - total);
    const int read = stream->readBytes(jpegBuffer + total, chunk);
    if (read <= 0) break;
    total += read;
    if (contentLength > 0 && total >= static_cast<size_t>(contentLength)) break;
  }
  http.end();
  if (total < 128 || total >= jpegBufferSize) return false;

  clearSelectedPhoto();
  if (!jpeg.openRAM(jpegBuffer, static_cast<int>(total), jpegDrawPhoto)) {
    return false;
  }
  const int jw = jpeg.getWidth();
  const int jh = jpeg.getHeight();
  int scale = 0;
  if (jw > kPhotoW * 2 || jh > kPhotoH * 2) {
    scale = JPEG_SCALE_QUARTER;
  } else if (jw > kPhotoW || jh > kPhotoH) {
    scale = JPEG_SCALE_HALF;
  }
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  const int result = jpeg.decode(0, 0, scale);
  jpeg.close();
  return result == JPEG_SUCCESS;
}

void enrichPlane(int index) {
  if (!trafficLive || index < 0 || index >= livePlaneCount || livePlanes[index].enriched) return;
  LivePlane& p = livePlanes[index];
  copyClean(p.photo, sizeof(p.photo), "", "INFO...");

  char url[128];
  JsonDocument doc;
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", p.callsign);
  if (httpsGetJson(url, doc)) {
    const char* from = doc["response"]["flightroute"]["origin"]["municipality"] | nullptr;
    const char* to = doc["response"]["flightroute"]["destination"]["municipality"] | nullptr;
    if (from && to) {
      snprintf(p.route, sizeof(p.route), "%s -> %s", from, to);
    }
  }

  if (strlen(p.hex)) {
    doc.clear();
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", p.hex);
    if (httpsGetJson(url, doc)) {
      const char* photo = doc["response"]["aircraft"]["url_photo_thumbnail"] | nullptr;
      if (photo && strlen(photo)) {
        copyClean(p.photoUrl, sizeof(p.photoUrl), photo, "");
        copyClean(p.photo, sizeof(p.photo), "PHOTO...", "PHOTO...");
        if (downloadSelectedPhoto(p.photoUrl)) {
          selectedPhotoIndex = index;
          copyClean(p.photo, sizeof(p.photo), "PHOTO OK", "PHOTO OK");
        } else {
          copyClean(p.photo, sizeof(p.photo), "PHOTO KO", "PHOTO KO");
        }
      } else {
        copyClean(p.photo, sizeof(p.photo), "SANS PHOTO", "SANS PHOTO");
      }
      const char* type = doc["response"]["aircraft"]["type"] | nullptr;
      if (type && strlen(type)) {
        copyClean(p.type, sizeof(p.type), type, p.type);
      }
    } else {
      copyClean(p.photo, sizeof(p.photo), "PHOTO KO", "PHOTO KO");
    }
  } else {
    copyClean(p.photo, sizeof(p.photo), "SANS PHOTO", "SANS PHOTO");
  }

  p.enriched = true;
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) {
      putPixel(xx, yy, color);
    }
  }
}

void drawRect(int x, int y, int w, int h, uint16_t color) {
  drawLine(x, y, x + w - 1, y, color);
  drawLine(x, y + h - 1, x + w - 1, y + h - 1, color);
  drawLine(x, y, x, y + h - 1, color);
  drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void fillRoundRect(int x, int y, int w, int h, int radius, uint16_t color) {
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) {
      int cx = xx < x + radius ? x + radius : (xx >= x + w - radius ? x + w - radius - 1 : xx);
      int cy = yy < y + radius ? y + radius : (yy >= y + h - radius ? y + h - radius - 1 : yy);
      const int dx = xx - cx;
      const int dy = yy - cy;
      if (dx * dx + dy * dy <= radius * radius) {
        putPixel(xx, yy, color);
      }
    }
  }
}

void drawRoundRect(int x, int y, int w, int h, int radius, uint16_t color) {
  drawLine(x + radius, y, x + w - radius - 1, y, color);
  drawLine(x + radius, y + h - 1, x + w - radius - 1, y + h - 1, color);
  drawLine(x, y + radius, x, y + h - radius - 1, color);
  drawLine(x + w - 1, y + radius, x + w - 1, y + h - radius - 1, color);
  for (int i = 0; i <= radius; ++i) {
    const int j = static_cast<int>(sqrtf(radius * radius - i * i));
    putPixel(x + radius - i, y + radius - j, color);
    putPixel(x + radius - j, y + radius - i, color);
    putPixel(x + w - radius - 1 + i, y + radius - j, color);
    putPixel(x + w - radius - 1 + j, y + radius - i, color);
    putPixel(x + radius - i, y + h - radius - 1 + j, color);
    putPixel(x + radius - j, y + h - radius - 1 + i, color);
    putPixel(x + w - radius - 1 + i, y + h - radius - 1 + j, color);
    putPixel(x + w - radius - 1 + j, y + h - radius - 1 + i, color);
  }
}

uint8_t glyphRow(char c, int row) {
  if (c >= 'a' && c <= 'z') c -= 32;
  static const uint8_t digits[10][7] = {
      {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
      {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
      {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
      {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
      {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
      {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
      {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
      {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
      {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
      {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
  if (c >= '0' && c <= '9') return digits[c - '0'][row];
  static const uint8_t letters[26][7] = {
      {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
      {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
      {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
      {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
      {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
      {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
      {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
      {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
      {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
      {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
      {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
      {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
      {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}};
  if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
  if (c == '.') return row == 6 ? 0x04 : 0x00;
  if (c == ':') return (row == 2 || row == 5) ? 0x04 : 0x00;
  return 0x00;
}

int textWidth(const char* text, int scale) {
  int count = 0;
  while (text[count]) ++count;
  return count ? count * 6 * scale - scale : 0;
}

void drawText(int x, int y, const char* text, uint16_t color, int scale = 1) {
  for (int i = 0; text[i]; ++i) {
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = glyphRow(text[i], row);
      for (int col = 0; col < 5; ++col) {
        if (bits & (1 << (4 - col))) {
          for (int yy = 0; yy < scale; ++yy) {
            for (int xx = 0; xx < scale; ++xx) {
              const int px = x + i * 6 * scale + col * scale + xx;
              const int py = y + row * scale + yy;
              if (scale <= 2) {
                blendPixel(px - 1, py, color, 34);
                blendPixel(px + 1, py, color, 34);
                blendPixel(px, py - 1, color, 28);
                blendPixel(px, py + 1, color, 28);
              }
              putPixel(px, py, color);
            }
          }
        }
      }
    }
  }
}

void drawTextCentered(int cx, int y, const char* text, uint16_t color, int scale = 1) {
  drawText(cx - textWidth(text, scale) / 2, y, text, color, scale);
}

void drawPolyline(const int points[][2], int count, uint16_t color, int thickness = 1) {
  for (int i = 0; i < count - 1; ++i) {
    if (thickness > 1) {
      drawThickLine(points[i][0], points[i][1], points[i + 1][0], points[i + 1][1], color);
    } else {
      drawLine(points[i][0], points[i][1], points[i + 1][0], points[i + 1][1], color);
    }
  }
}

void drawBlendPolyline(const int points[][2], int count, uint16_t color, uint8_t alpha) {
  for (int i = 0; i < count - 1; ++i) {
    blendLine(points[i][0], points[i][1], points[i + 1][0], points[i + 1][1], color, alpha);
  }
}

void drawMapWatermark(uint16_t terrain, uint16_t forest, uint16_t road, uint16_t border, uint16_t label) {
  blendFillCircle(116, 104, 92, forest, 56);
  blendFillCircle(302, 118, 112, forest, 42);
  blendFillCircle(122, 318, 118, forest, 36);
  blendFillCircle(350, 312, 128, forest, 42);
  blendFillCircle(240, 230, 164, rgb565(12, 74, 42), 28);

  const int contours[][4] = {
      {0, 116, 116, 100}, {116, 100, 250, 112}, {250, 112, 480, 76},
      {0, 318, 122, 282}, {122, 282, 244, 292}, {244, 292, 480, 342},
      {48, 414, 188, 350}, {188, 350, 342, 386}, {0, 206, 160, 224},
      {160, 224, 242, 184}, {242, 184, 398, 204}, {318, 368, 480, 282},
      {44, 150, 168, 188}, {168, 188, 264, 170}, {264, 170, 426, 150},
      {0, 248, 150, 252}, {150, 252, 250, 230}, {250, 230, 450, 252},
      {88, 74, 166, 132}, {166, 132, 258, 148}, {258, 148, 372, 108},
      {86, 354, 210, 312}, {210, 312, 324, 338}, {324, 338, 440, 318}};
  for (const auto& l : contours) {
    blendLine(l[0], l[1], l[2], l[3], terrain, 54);
  }

  const int roads[][4] = {
      {0, 286, 174, 246}, {174, 246, 318, 262}, {318, 262, 480, 228},
      {72, 392, 190, 342}, {190, 342, 300, 372}, {300, 372, 434, 338},
      {0, 214, 118, 244}, {118, 244, 226, 224}, {226, 224, 392, 236},
      {268, 304, 356, 300}, {356, 300, 462, 270}};
  for (const auto& l : roads) {
    blendThickLine(l[0], l[1], l[2], l[3], road, 80);
  }

  const int ridge[][2] = {{0, 330}, {96, 292}, {134, 252}, {174, 222}, {216, 194}, {276, 168}, {354, 142}, {480, 108}};
  drawBlendPolyline(ridge, 8, border, 48);
  drawText(156, 156, "PARC", label, 2);
  drawText(118, 180, "NATUREL", label, 2);
  drawText(118, 204, "REGIONAL", label, 2);
  drawText(124, 228, "JURA", label, 2);
  drawText(116, 252, "VAUDOIS", label, 2);
  drawText(176, 290, "GIMEL", label, 1);
  drawText(308, 314, "GLAND", label, 1);
  drawText(388, 256, "A1", label, 1);
}

void drawMenuPanel(uint16_t green, uint16_t text, uint16_t panel) {
  blendRoundRect(72, 96, 336, 276, 22, rgb565(0, 10, 9), 232);
  drawRoundRect(72, 96, 336, 276, 22, rgb565(62, 176, 98));
  if (appView == AppView::Settings) {
    drawTextCentered(240, 120, "REGLAGES", green, 2);
    drawText(104, 150, "NPA", green, 2);
    drawText(188, 150, "<", green, 2);
    drawText(224, 150, currentPostal, text, 2);
    drawText(292, 150, ">", green, 2);
    drawTextCentered(250, 170, strcmp(currentPostal, "MANUEL") == 0 ? "MANUEL" : kPostalPresets[postalIndex].label, text, 1);
    drawText(104, 178, "RAYON", green, 2);
    drawText(214, 178, "50 KM", text, 2);
    drawText(104, 206, "WIFI", green, 2);
    drawText(214, 206, WiFi.status() == WL_CONNECTED ? "OK" : "KO", text, 2);
    drawText(104, 234, "SOURCE", green, 2);
    drawText(214, 234, trafficLive ? "LIVE" : "SIM", text, 2);
    blendRoundRect(126, 260, 228, 34, 12, rgb565(1, 15, 12), 232);
    drawRoundRect(126, 260, 228, 34, 12, rgb565(70, 220, 104));
    drawTextCentered(240, 271, "WIFI SETUP", text, 1);
    if (setupPortalActive) {
      drawTextCentered(240, 302, "AP FLIGHTDESK-SETUP", green, 1);
      drawTextCentered(240, 318, "192.168.4.1", text, 1);
    } else {
      drawTextCentered(240, 310, activeWifiSsid(), text, 1);
    }
    blendRoundRect(144, 334, 192, 30, 12, rgb565(1, 15, 12), 232);
    drawRoundRect(144, 334, 192, 30, 12, rgb565(70, 220, 104));
    drawTextCentered(240, 345, "FERMER", text, 1);
    return;
  }

  if (appView == AppView::Search) {
    drawTextCentered(240, 122, "RECHERCHE", green, 2);
    drawText(112, 170, "Touchez un avion", text, 2);
    drawText(112, 200, "pour afficher le vol.", text, 2);
    drawText(112, 246, "Recherche texte a venir", green, 1);
  } else if (appView == AppView::Favorites) {
    drawTextCentered(240, 122, "FAVORIS", green, 2);
    drawText(112, 170, "Aucun favori local", text, 2);
    drawText(112, 206, "Etoile dans popup.", green, 1);
  } else {
    drawTextCentered(240, 122, "ASSISTANT IA", green, 2);
    drawText(112, 168, "Version simple", text, 2);
    drawText(112, 198, "sans audio ici.", text, 2);
  }
  blendRoundRect(144, 304, 192, 42, 14, rgb565(1, 15, 12), 232);
  drawRoundRect(144, 304, 192, 42, 14, rgb565(70, 220, 104));
  drawTextCentered(240, 317, "FERMER", text, 2);
}

void drawAircraftPopup(uint16_t green, uint16_t text, uint16_t panel) {
  if (selectedPlane < 0 || selectedPlane >= aircraftCount()) return;
  char line[40];
  blendRoundRect(52, 124, 376, 214, 18, rgb565(0, 10, 9), 238);
  drawRoundRect(52, 124, 376, 214, 18, rgb565(76, 210, 102));
  drawText(82, 148, "AVION SELECTIONNE", green, 1);
  drawText(82, 178, planeCallsign(selectedPlane), green, 3);
  drawText(82, 222, planeType(selectedPlane), text, 1);
  drawText(82, 244, planeRoute(selectedPlane), text, 1);
  snprintf(line, sizeof(line), "%dKM  %dKMH", planeDistanceKm(selectedPlane), planeSpeedKmh(selectedPlane));
  drawText(82, 278, line, text, 1);
  snprintf(line, sizeof(line), "%dM  %dDEG", planeAltitudeM(selectedPlane), planeHeading(selectedPlane) + 90);
  drawText(82, 304, line, text, 1);

  blendRoundRect(286, 218, 106, 70, 12, rgb565(8, 32, 26), 222);
  drawRoundRect(286, 218, 106, 70, 12, rgb565(88, 220, 116));
  const bool hasPhoto = trafficLive && selectedPhotoFrame && selectedPhotoIndex == selectedPlane &&
                        !strcmp(livePlanes[selectedPlane].photo, "PHOTO OK");
  if (hasPhoto) {
    for (int py = 0; py < kPhotoH; ++py) {
      for (int px = 0; px < kPhotoW; ++px) {
        const int dx = px - kPhotoW / 2;
        const int dy = py - kPhotoH / 2;
        if (dx * dx / 4 + dy * dy <= 2100) {
          putPixel(286 + px, 218 + py, selectedPhotoFrame[py * kPhotoW + px]);
        }
      }
    }
    blendRoundRect(286, 218, 106, 70, 12, rgb565(0, 30, 18), 54);
  } else {
    drawTextCentered(339, 238, "PHOTO", green, 1);
    drawTextCentered(339, 256, trafficLive ? livePlanes[selectedPlane].photo : "SIM", text, 1);
    const int px = 338;
    const int py = 274;
    drawThickLine(px - 34, py, px + 34, py - 12, green);
    drawThickLine(px - 4, py - 4, px + 20, py + 15, green);
  }

  blendRoundRect(336, 144, 34, 34, 13, rgb565(2, 24, 16), 226);
  drawRoundRect(336, 144, 34, 34, 13, green);
  drawTextCentered(353, 153, "*", text, 2);
  blendRoundRect(378, 144, 34, 34, 13, rgb565(2, 24, 16), 226);
  drawRoundRect(378, 144, 34, 34, 13, green);
  drawTextCentered(395, 153, "X", text, 2);
}

void drawScreenNav(uint16_t green, uint16_t text, uint16_t panel) {
  const char* labels[] = {"RADAR", "RECH", "FAV", "REGL", "IA"};
  constexpr int width = 50;
  constexpr int height = 28;
  constexpr int gap = 6;
  constexpr int startX = 101;
  constexpr int y = 356;
  for (int i = 0; i < 5; ++i) {
    const int x = startX + i * (width + gap);
    const bool active = static_cast<int>(appView) == i;
    blendRoundRect(x, y, width, height, 12, active ? rgb565(4, 28, 18) : rgb565(1, 10, 10), active ? 232 : 210);
    drawRoundRect(x, y, width, height, 12, active ? green : rgb565(20, 58, 38));
    drawTextCentered(x + width / 2, y + 10, labels[i], active ? green : rgb565(178, 206, 188), 1);
  }
}

void drawDiagnostics(uint16_t green, uint16_t text, uint16_t panel) {
  char line[48];
  fillRoundRect(126, 446, 228, 20, 8, panel);
  snprintf(line, sizeof(line), "GT%s QMI%s", gt911Ok ? "OK" : "KO", qmiOk ? "OK" : "KO");
  drawTextCentered(240, 452, line, gt911Ok && qmiOk ? green : text, 1);
}

void fillWedge(int cx, int cy, int radius, float startDeg, float endDeg, uint16_t color) {
  constexpr float stepDeg = 3.0f;
  for (float a = startDeg; a < endDeg; a += stepDeg) {
    const float a0 = a * DEG_TO_RAD;
    const float a1 = min(a + stepDeg, endDeg) * DEG_TO_RAD;
    fillTriangle(cx, cy,
                 cx + static_cast<int>(cosf(a0) * radius),
                 cy + static_cast<int>(sinf(a0) * radius),
                 cx + static_cast<int>(cosf(a1) * radius),
                 cy + static_cast<int>(sinf(a1) * radius),
                 color);
  }
}

uint8_t angleToBin(float angleDeg) {
  while (angleDeg < 0.0f) angleDeg += 360.0f;
  while (angleDeg >= 360.0f) angleDeg -= 360.0f;
  int bin = static_cast<int>(angleDeg * (static_cast<float>(kSweepBins) / 360.0f) + 0.5f);
  if (bin >= kSweepBins) bin -= kSweepBins;
  return static_cast<uint8_t>(bin);
}

void buildRadarAngleBins() {
  if (!radarAngleBins) return;
  constexpr int cx = kRadarCx;
  constexpr int cy = kRadarCy;
  constexpr int r = kRadarRadius;
  const int rr = (r - 14) * (r - 14);
  const int inner = 15 * 15;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int dx = x - cx;
      const int dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      uint8_t bin = 0xFF;
      if (d2 <= rr && d2 >= inner) {
        float a = atan2f(static_cast<float>(dy), static_cast<float>(dx)) / DEG_TO_RAD;
        if (a < 0.0f) a += 360.0f;
        bin = angleToBin(a);
      }
      radarAngleBins[y * kWidth + x] = bin;
    }
  }
}

void blendRadarSweep(int cx, int cy, int radius, float angleDeg, uint16_t color) {
  if (!radarAngleBins) return;
  const uint8_t headBin = angleToBin(angleDeg);
  const int rr = radius * radius;
  const int inner = 15 * 15;
  for (int y = cy - radius; y <= cy + radius; y += 2) {
    for (int x = cx - radius; x <= cx + radius; x += 2) {
      const int dx = x - cx;
      const int dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > rr || d2 < inner) continue;
      const uint8_t pixelBin = radarAngleBins[y * kWidth + x];
      if (pixelBin == 0xFF) continue;
      const uint8_t lag = static_cast<uint8_t>((headBin + kSweepBins - pixelBin) % kSweepBins);
      if (lag > kSweepTrailBins) continue;

      const float angular = 1.0f - static_cast<float>(lag) / static_cast<float>(kSweepTrailBins);
      const float radial = 0.35f + 0.65f * (static_cast<float>(d2) / rr);
      const uint8_t alpha = static_cast<uint8_t>(6 + 48.0f * angular * angular * radial);
      blendPixel(x, y, color, alpha);
      blendPixel(x + 1, y, color, alpha);
      blendPixel(x, y + 1, color, alpha);
      blendPixel(x + 1, y + 1, color, alpha);
    }
  }
}

void buildRadarBaseFrame() {
  if (!radarBaseFrame) return;

  uint16_t* drawFrame = frame;
  frame = radarBaseFrame;

  constexpr int cx = kRadarCx;
  constexpr int cy = kRadarCy;
  constexpr int r = kRadarRadius;
  const uint16_t black = rgb565(0, 2, 3);
  const uint16_t green = rgb565(108, 255, 170);
  const uint16_t softGreen = rgb565(48, 205, 146);
  const uint16_t dim = rgb565(9, 54, 56);

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int dx = x - cx;
      const int dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > r * r) {
        frame[y * kWidth + x] = black;
      } else {
        const uint8_t shade = d2 < 120 * 120 ? 14 : (d2 < 190 * 190 ? 10 : 7);
        const uint8_t blue = d2 < 160 * 160 ? 20 : 15;
        frame[y * kWidth + x] = rgb565(1, shade, blue);
      }
    }
  }

  fillCircle(cx, cy, r - 8, rgb565(1, 9, 12));
  if (!mapReady) {
    blendEmbeddedMap(92);
  }
  if (mapReady && mapFrame) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= (r - 4) * (r - 4)) {
          const int index = y * kWidth + x;
          frame[index] = blend565(frame[index], mapFrame[index], 58);
        }
      }
    }
  }
  blendFillCircle(cx, cy, r - 8, rgb565(0, 4, 6), 86);
  blendFillCircle(cx, cy, r - 24, rgb565(7, 42, 38), 20);
  blendFillCircle(cx, cy, r / 2, rgb565(8, 54, 42), 9);

  drawCircle(cx, cy, r, green);
  blendCircle(cx, cy, r - 1, green, 140);
  blendCircle(cx, cy, r - 3, softGreen, 80);
  blendCircle(cx, cy, r - 8, dim, 90);
  blendCircle(cx, cy, r - 16, rgb565(1, 24, 20), 120);

  for (int ring = r / 4; ring <= r; ring += r / 4) {
    blendCircle(cx, cy, ring, softGreen, 58);
    blendCircle(cx, cy, ring + 1, dim, 38);
  }

  for (int a = 0; a < 360; a += 30) {
    const float rad = a * DEG_TO_RAD;
    blendLine(cx + cosf(rad) * 16, cy + sinf(rad) * 16,
              cx + cosf(rad) * r, cy + sinf(rad) * r, softGreen, 48);
  }

  frame = drawFrame;
  radarBaseDirty = false;
}

void drawRadarFrame() {
  if (!frame || !lcdPanel) return;

  constexpr int cx = kRadarCx;
  constexpr int cy = kRadarCy;
  constexpr int r = kRadarRadius;
  const uint16_t black = rgb565(0, 2, 3);
  const uint16_t green = rgb565(108, 255, 170);
  const uint16_t softGreen = rgb565(48, 205, 146);
  const uint16_t glow = rgb565(18, 165, 110);
  const uint16_t dim = rgb565(9, 54, 56);
  const uint16_t panel = rgb565(1, 11, 10);
  const uint16_t text = rgb565(225, 244, 228);

  if (!radarBaseFrame || radarBaseDirty) {
    buildRadarBaseFrame();
  }
  if (radarBaseFrame) {
    memcpy(frame, radarBaseFrame, kWidth * kHeight * sizeof(uint16_t));
  } else {
    fillCircle(cx, cy, r, black);
  }

  blendRadarSweep(cx, cy, r - 14, sweepDeg, rgb565(22, 245, 178));
  const float sweepRad = sweepDeg * DEG_TO_RAD;
  blendLine(cx, cy,
            cx + static_cast<int>(cosf(sweepRad) * (r - 10)),
            cy + static_cast<int>(sinf(sweepRad) * (r - 10)),
            green, 170);
  blendLine(cx + 1, cy,
            cx + 1 + static_cast<int>(cosf(sweepRad) * (r - 10)),
            cy + static_cast<int>(sinf(sweepRad) * (r - 10)),
            green, 70);

  const int count = aircraftCount();
  for (int planeIndex = 0; planeIndex < count; ++planeIndex) {
    const float ar = planeHeading(planeIndex) * DEG_TO_RAD;
    const int x = planeX(planeIndex);
    const int y = planeY(planeIndex);
    const int noseX = x + static_cast<int>(cosf(ar) * 17);
    const int noseY = y + static_cast<int>(sinf(ar) * 17);
    const int leftX = x + static_cast<int>(cosf(ar + 2.55f) * 10);
    const int leftY = y + static_cast<int>(sinf(ar + 2.55f) * 10);
    const int rightX = x + static_cast<int>(cosf(ar - 2.55f) * 10);
    const int rightY = y + static_cast<int>(sinf(ar - 2.55f) * 10);
    if (planeIndex == selectedPlane) {
      drawCircle(x, y, 22, softGreen);
      blendThickLine(cx, cy, x, y, glow, 110);
    }
    blendFillCircle(x, y, planeIndex == selectedPlane ? 19 : 13, glow, planeIndex == selectedPlane ? 46 : 20);
    blendFillCircle(x, y, planeIndex == selectedPlane ? 14 : 9, green, planeIndex == selectedPlane ? 28 : 12);
    fillTriangle(noseX + 1, noseY + 1, leftX + 1, leftY + 1, rightX + 1, rightY + 1, rgb565(0, 8, 6));
    fillTriangle(noseX, noseY, leftX, leftY, rightX, rightY, rgb565(205, 255, 202));
    drawLine(noseX, noseY, leftX, leftY, green);
    drawLine(noseX, noseY, rightX, rightY, green);
  }

  char countText[12];
  snprintf(countText, sizeof(countText), "%d", count);
  drawTextCentered(cx, 48, "18:47", text, 1);
  drawTextCentered(cx, 67, trafficLive ? "LIVE AIRPLANES" : "TRAFIC SIMULE", softGreen, 1);
  drawTextCentered(cx, 90, countText, green, 2);
  drawTextCentered(cx, 116, "AVIONS", green, 2);
  drawText(cx + r * 41 / 100, cy + 3, "20", softGreen, 1);
  drawText(cx + r * 72 / 100, cy + 3, "50", softGreen, 1);
  drawText(cx + r * 73 / 100, cy + 20, "KM", softGreen, 1);

  fillCircle(cx, cy, 9, black);
  drawCircle(cx, cy, 10, green);
  drawCircle(cx, cy, 16, dim);

  if (appView != AppView::Radar) {
    drawMenuPanel(green, text, panel);
  } else if (selectedPlane >= 0) {
    drawAircraftPopup(green, text, panel);
  } else {
    drawScreenNav(green, text, rgb565(1, 12, 12));
  }

  if (millis() < touchMarkerUntilMs) {
    drawCircle(lastTouchX, lastTouchY, 16, rgb565(120, 255, 130));
  }

  if (!gt911Ok || !qmiOk) {
    drawDiagnostics(green, text, panel);
  }

  esp_lcd_panel_draw_bitmap(lcdPanel, 0, 0, kWidth, kHeight, frame);
}

void initBacklight() {
  ledcSetup(1, 20000, 10);
  ledcAttachPin(kBacklightPin, 1);
  ledcWrite(1, 800);
}

bool initDisplay() {
  Serial.println("[DISPLAY] Init officielle Waveshare ST7701S");

  initExio();
  lcdReset();

  spi_bus_config_t busConfig = {};
  busConfig.mosi_io_num = kLcdMosi;
  busConfig.miso_io_num = -1;
  busConfig.sclk_io_num = kLcdSck;
  busConfig.quadwp_io_num = -1;
  busConfig.quadhd_io_num = -1;
  busConfig.max_transfer_sz = 64;
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &busConfig, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[DISPLAY] SPI bus KO: %d\n", err);
    return false;
  }

  spi_device_interface_config_t deviceConfig = {};
  deviceConfig.command_bits = 1;
  deviceConfig.address_bits = 8;
  deviceConfig.mode = SPI_MODE0;
  deviceConfig.clock_speed_hz = 40000000;
  deviceConfig.spics_io_num = -1;
  deviceConfig.queue_size = 1;
  err = spi_bus_add_device(SPI2_HOST, &deviceConfig, &lcdSpi);
  if (err != ESP_OK) {
    Serial.printf("[DISPLAY] SPI device KO: %d\n", err);
    return false;
  }

  setExio(kExioLcdCs, false);
  initSt7701Registers();
  setExio(kExioLcdCs, true);

  esp_lcd_rgb_panel_config_t rgbConfig = {};
  rgbConfig.clk_src = LCD_CLK_SRC_PLL240M;
  // Lower than the official 30 MHz demo while we are still using the older
  // Arduino-bundled esp_lcd driver without its bounce-buffer option.
  rgbConfig.timings.pclk_hz = 12 * 1000 * 1000;
  rgbConfig.timings.h_res = kHeight;
  rgbConfig.timings.v_res = kWidth;
  rgbConfig.timings.hsync_pulse_width = 8;
  rgbConfig.timings.hsync_back_porch = 10;
  rgbConfig.timings.hsync_front_porch = 50;
  rgbConfig.timings.vsync_pulse_width = 2;
  rgbConfig.timings.vsync_back_porch = 18;
  rgbConfig.timings.vsync_front_porch = 8;
  rgbConfig.timings.flags.pclk_active_neg = 0;
  rgbConfig.data_width = 16;
  rgbConfig.psram_trans_align = 64;
  rgbConfig.hsync_gpio_num = 38;
  rgbConfig.vsync_gpio_num = 39;
  rgbConfig.de_gpio_num = 40;
  rgbConfig.pclk_gpio_num = 41;
  rgbConfig.disp_gpio_num = -1;
  const int dataPins[] = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17};
  for (int i = 0; i < 16; ++i) {
    rgbConfig.data_gpio_nums[i] = dataPins[i];
  }
  rgbConfig.flags.fb_in_psram = true;

  err = esp_lcd_new_rgb_panel(&rgbConfig, &lcdPanel);
  if (err != ESP_OK) {
    Serial.printf("[DISPLAY] RGB panel KO: %d\n", err);
    return false;
  }
  ESP_ERROR_CHECK(esp_lcd_panel_reset(lcdPanel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(lcdPanel));

  frame = static_cast<uint16_t*>(heap_caps_malloc(kWidth * kHeight * sizeof(uint16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frame) {
    Serial.println("[DISPLAY] Framebuffer PSRAM KO");
    return false;
  }
  mapFrame = static_cast<uint16_t*>(heap_caps_malloc(kWidth * kHeight * sizeof(uint16_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!mapFrame) {
    Serial.println("[DISPLAY] Map framebuffer PSRAM KO");
    return false;
  }
  radarBaseFrame = static_cast<uint16_t*>(heap_caps_malloc(kWidth * kHeight * sizeof(uint16_t),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!radarBaseFrame) {
    Serial.println("[DISPLAY] Radar base framebuffer PSRAM KO");
    return false;
  }
  radarAngleBins = static_cast<uint8_t*>(heap_caps_malloc(kWidth * kHeight,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!radarAngleBins) {
    Serial.println("[DISPLAY] Radar angle LUT PSRAM KO");
    return false;
  }
  buildRadarAngleBins();
  tileBufferSize = 220 * 1024;
  tileBuffer = static_cast<uint8_t*>(heap_caps_malloc(tileBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!tileBuffer) {
    Serial.println("[DISPLAY] Tile buffer PSRAM KO");
    return false;
  }
  jpegBufferSize = 96 * 1024;
  jpegBuffer = static_cast<uint8_t*>(heap_caps_malloc(jpegBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!jpegBuffer) {
    Serial.println("[DISPLAY] JPEG buffer PSRAM KO");
    return false;
  }
  selectedPhotoFrame = static_cast<uint16_t*>(heap_caps_malloc(kPhotoW * kPhotoH * sizeof(uint16_t),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!selectedPhotoFrame) {
    Serial.println("[DISPLAY] Photo framebuffer PSRAM KO");
    return false;
  }
  clearSelectedPhoto();
  clearMapFrame();

  initBacklight();
  drawRadarFrame();
  Serial.println("[DISPLAY] OK stable radar frame");
  return true;
}

void printHeader() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(kBuildName);
  Serial.println("Source LCD: demo officiel Waveshare 2.8C");
  Serial.println("========================================");
}

void loadWifiSettings() {
  storedSsid[0] = '\0';
  storedPassword[0] = '\0';
  if (!prefs.begin("flightdesk", true)) {
    Serial.println("[WIFI] NVS lecture KO");
    return;
  }
  const String ssid = prefs.getString("ssid", "");
  const String password = prefs.getString("pass", "");
  prefs.end();
  copyClean(storedSsid, sizeof(storedSsid), ssid.c_str(), "");
  copyClean(storedPassword, sizeof(storedPassword), password.c_str(), "");
}

const char* activeWifiSsid() {
  return strlen(storedSsid) ? storedSsid : FLIGHTDESK_WIFI_SSID;
}

const char* activeWifiPassword() {
  return strlen(storedSsid) ? storedPassword : FLIGHTDESK_WIFI_PASSWORD;
}

void applyPostalIndex(int index) {
  const int count = sizeof(kPostalPresets) / sizeof(kPostalPresets[0]);
  postalIndex = (index % count + count) % count;
  copyClean(currentPostal, sizeof(currentPostal), kPostalPresets[postalIndex].code, "1188");
  radarLat = kPostalPresets[postalIndex].lat;
  radarLon = kPostalPresets[postalIndex].lon;
}

void saveLocationSettings() {
  if (!prefs.begin("flightdesk", false)) {
    Serial.println("[LOC] NVS ecriture KO");
    return;
  }
  prefs.putString("npa", currentPostal);
  prefs.putDouble("lat", radarLat);
  prefs.putDouble("lon", radarLon);
  prefs.end();
}

void loadLocationSettings() {
  if (!prefs.begin("flightdesk", true)) {
    Serial.println("[LOC] NVS lecture KO");
    return;
  }
  const String npa = prefs.getString("npa", "1188");
  const double lat = prefs.getDouble("lat", kHomeLat);
  const double lon = prefs.getDouble("lon", kHomeLon);
  prefs.end();

  int preset = 0;
  const int count = sizeof(kPostalPresets) / sizeof(kPostalPresets[0]);
  for (int i = 0; i < count; ++i) {
    if (npa == kPostalPresets[i].code) {
      preset = i;
      break;
    }
  }
  postalIndex = preset;
  copyClean(currentPostal, sizeof(currentPostal), npa.c_str(), "1188");
  radarLat = lat;
  radarLon = lon;
  radarBaseDirty = true;
}

void changePostal(int delta) {
  applyPostalIndex(postalIndex + delta);
  saveLocationSettings();
  mapReady = false;
  radarBaseDirty = true;
  mapReloadAfterMs = 0;
  lastMapFetchMs = 0;
  lastTrafficFetchMs = 0;
  trafficLive = false;
  livePlaneCount = 0;
  selectedPlane = -1;
  Serial.printf("[LOC] NPA %s %.5f,%.5f\n", currentPostal, radarLat, radarLon);
}

String htmlEscape(const char* value) {
  String out;
  while (value && *value) {
    switch (*value) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      default: out += *value; break;
    }
    ++value;
  }
  return out;
}

void handleSetupRoot() {
  String html;
  html.reserve(1800);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>FlightDesk Wi-Fi</title><style>");
  html += F("body{margin:0;background:#050b08;color:#eaffea;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif}");
  html += F("main{max-width:420px;margin:0 auto;padding:28px}h1{color:#86ff78;font-size:28px;margin:0 0 8px}");
  html += F("p{color:#9fb7a4;line-height:1.4}label{display:block;margin:18px 0 8px;color:#86ff78;font-weight:700}");
  html += F("input{box-sizing:border-box;width:100%;padding:14px;border-radius:10px;border:1px solid #245a38;background:#07130d;color:#fff;font-size:18px}");
  html += F("button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:12px;background:#58f07b;color:#031009;font-size:18px;font-weight:800}");
  html += F(".card{border:1px solid #234d34;border-radius:16px;padding:18px;background:#09130f}");
  html += F("</style></head><body><main><h1>FlightDesk</h1><p>Configuration Wi-Fi locale stockee dans l'ESP32.</p>");
  html += F("<form class='card' method='post' action='/save'><label>Nom du Wi-Fi</label><input name='ssid' maxlength='32' value='");
  html += htmlEscape(activeWifiSsid());
  html += F("' autofocus><label>Mot de passe</label><input name='pass' type='password' maxlength='64' placeholder='laisser vide pour garder'>");
  html += F("<button type='submit'>Enregistrer</button></form>");
  html += F("<p>Apres sauvegarde, FlightDesk reconnecte le radar avec ces identifiants.</p></main></body></html>");
  setupServer.send(200, "text/html", html);
}

void handleSetupSave() {
  const String ssid = setupServer.arg("ssid");
  const String password = setupServer.arg("pass");
  const String savedPassword = password.length() ? password : String(storedPassword);
  if (!prefs.begin("flightdesk", false)) {
    setupServer.send(500, "text/plain", "NVS KO");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", savedPassword);
  prefs.end();
  copyClean(storedSsid, sizeof(storedSsid), ssid.c_str(), "");
  copyClean(storedPassword, sizeof(storedPassword), savedPassword.c_str(), "");
  wifiReconnectAtMs = millis() + 800;
  setupServer.send(200, "text/html",
                   "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<body style='background:#050b08;color:#eaffea;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;padding:28px'>"
                   "<h1 style='color:#86ff78'>Wi-Fi enregistre</h1>"
                   "<p>FlightDesk tente la reconnexion. Vous pouvez revenir au radar.</p></body>");
  Serial.printf("[WIFI] Nouveau SSID stocke: %s\n", storedSsid);
}

void startWifiPortal() {
  if (setupPortalActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("FlightDesk-Setup");
  setupServer.on("/", HTTP_GET, handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.onNotFound(handleSetupRoot);
  setupServer.begin();
  setupPortalActive = true;
  Serial.printf("[WIFI] Portail actif: FlightDesk-Setup http://%s\n",
                WiFi.softAPIP().toString().c_str());
}

void connectWifi() {
  const char* ssid = activeWifiSsid();
  const char* password = activeWifiPassword();
  if (strlen(ssid) == 0) {
    Serial.println("[WIFI] Aucun SSID configure; portail setup actif");
    startWifiPortal();
    return;
  }

  WiFi.mode(setupPortalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  Serial.printf("[WIFI] Connexion a %s", ssid);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] OK IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.printf("[WIFI] KO status=%d\n", WiFi.status());
    startWifiPortal();
  }
}

void initOta() {
  if (otaStarted || WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname("flightdesk-waveshare-28c");
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Start"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] End"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[OTA] Error %u\n", error); });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("[OTA] Actif: flightdesk-waveshare-28c.local");
}

const char* knownI2cDevice(uint8_t address) {
  switch (address) {
    case 0x20:
      return "TCA9554 GPIO expander";
    case 0x51:
      return "PCF85063 RTC possible";
    case 0x5D:
      return "GT911 touch possible";
    case 0x6A:
    case 0x6B:
      return "QMI8658 IMU possible";
    default:
      return "";
  }
}

void scanI2c() {
  Serial.println("[I2C] Scan GPIO7=SCL GPIO15=SDA");
  uint8_t count = 0;

  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();
    if (error == 0) {
      const char* label = knownI2cDevice(address);
      Serial.printf("  - 0x%02X%s%s\n", address, strlen(label) ? "  " : "", label);
      ++count;
    }
  }

  if (count == 0) {
    Serial.println("  Aucun peripherique detecte");
  }
}

void printStatus() {
  Serial.printf("[SYS] uptime=%lus heap=%u psram=%u display=%s wifi=%s\n",
                millis() / 1000,
                ESP.getFreeHeap(),
                ESP.getFreePsram(),
                lcdPanel ? "ok" : "ko",
                WiFi.status() == WL_CONNECTED ? "ok" : "ko");
}

bool inRect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

bool inRadarCircle(uint16_t x, uint16_t y) {
  const int dx = static_cast<int>(x) - kRadarCx;
  const int dy = static_cast<int>(y) - kRadarCy;
  return dx * dx + dy * dy <= (kRadarRadius - 12) * (kRadarRadius - 12);
}

bool inBottomNav(uint16_t x, uint16_t y) {
  return inRect(x, y, 93, 344, 314, 58);
}

void invalidateLiveData() {
  mapReady = false;
  mapDirty = false;
  mapReloadAfterMs = 0;
  lastMapFetchMs = 0;
  lastTrafficFetchMs = 0;
  trafficLive = false;
  livePlaneCount = 0;
  selectedPlane = -1;
  radarBaseDirty = true;
}

void shiftMapFrame(int dx, int dy) {
  if (!mapFrame || !mapReady || (dx == 0 && dy == 0)) return;
  const uint16_t bg = rgb565(0, 6, 8);
  const int yStart = dy > 0 ? kHeight - 1 : 0;
  const int yEnd = dy > 0 ? -1 : kHeight;
  const int yStep = dy > 0 ? -1 : 1;
  const int xStart = dx > 0 ? kWidth - 1 : 0;
  const int xEnd = dx > 0 ? -1 : kWidth;
  const int xStep = dx > 0 ? -1 : 1;
  for (int y = yStart; y != yEnd; y += yStep) {
    for (int x = xStart; x != xEnd; x += xStep) {
      const int sx = x - dx;
      const int sy = y - dy;
      mapFrame[y * kWidth + x] = (sx >= 0 && sx < kWidth && sy >= 0 && sy < kHeight)
                                     ? mapFrame[sy * kWidth + sx]
                                     : bg;
    }
  }
}

void moveMapByPixels(int dx, int dy) {
  if (dx == 0 && dy == 0) return;
  const double metersPerPixel = (kRangeKm * 1000.0) / max(1, kRadarRadius);
  const double metersEast = -dx * metersPerPixel;
  const double metersNorth = dy * metersPerPixel;
  const double latMeters = 111320.0;
  const double lonMeters = 111320.0 * fmax(0.2, cos(radarLat * DEG_TO_RAD));
  radarLat += metersNorth / latMeters;
  radarLon += metersEast / lonMeters;
  radarLat = constrain(radarLat, -80.0, 80.0);
  if (radarLon > 180.0) radarLon -= 360.0;
  if (radarLon < -180.0) radarLon += 360.0;
  copyClean(currentPostal, sizeof(currentPostal), "MANUEL", "MANUEL");
  shiftMapFrame(dx, dy);
  mapDirty = true;
  radarBaseDirty = true;
  mapReloadAfterMs = millis() + 900;
  lastMapFetchMs = 0;
  updateLivePlanePositions();
  selectedPlane = -1;
}

int nearestPlaneAt(uint16_t x, uint16_t y) {
  int best = -1;
  int bestD2 = 34 * 34;
  for (int i = 0; i < aircraftCount(); ++i) {
    const int dx = static_cast<int>(x) - planeX(i);
    const int dy = static_cast<int>(y) - planeY(i);
    const int d2 = dx * dx + dy * dy;
    if (d2 < bestD2) {
      bestD2 = d2;
      best = i;
    }
  }
  return best;
}

void handleTap(uint16_t x, uint16_t y) {
  lastTouchX = x;
  lastTouchY = y;
  touchMarkerUntilMs = millis() + 900;
  Serial.printf("[TOUCH] tap x=%u y=%u menu=%d selected=%d\n", x, y, menuOpen, selectedPlane);

  if (appView != AppView::Radar) {
    if (appView == AppView::Settings && inRect(x, y, 162, 136, 64, 50)) {
      changePostal(-1);
      return;
    }
    if (appView == AppView::Settings && inRect(x, y, 274, 136, 64, 50)) {
      changePostal(1);
      return;
    }
    if (appView == AppView::Settings && inRect(x, y, 116, 248, 248, 58)) {
      startWifiPortal();
      return;
    }
    if (appView == AppView::Settings && inRect(x, y, 118, 324, 244, 58)) {
      appView = AppView::Radar;
      selectedPlane = -1;
      return;
    }
    if (appView != AppView::Settings &&
        (inRect(x, y, 130, 292, 220, 68) || inRect(x, y, 88, 348, 304, 56))) {
      appView = AppView::Radar;
      selectedPlane = -1;
    }
    return;
  }

  if (selectedPlane >= 0) {
    if (inRect(x, y, 368, 132, 58, 58) || inRect(x, y, 378, 144, 34, 34)) {
      selectedPlane = -1;
      return;
    }
    if (!inRect(x, y, 52, 124, 376, 214)) {
      selectedPlane = -1;
    }
    return;
  }

  if (inRect(x, y, 93, 344, 314, 58)) {
    if (x < 157) {
      appView = AppView::Radar;
    } else if (x < 213) {
      appView = AppView::Search;
    } else if (x < 269) {
      appView = AppView::Favorites;
    } else if (x < 325) {
      appView = AppView::Settings;
    } else {
      appView = AppView::Assistant;
    }
    selectedPlane = -1;
    return;
  }

  const int plane = nearestPlaneAt(x, y);
  if (plane >= 0) {
    selectedPlane = plane;
    clearSelectedPhoto();
    requestPlaneEnrich(selectedPlane);
  }
}

void pollTouch() {
  uint16_t x = 0;
  uint16_t y = 0;
  const bool down = readTouch(x, y);
  const uint32_t now = millis();
  touchActive = down;
  if (down) {
    lastTouchX = x;
    lastTouchY = y;
    touchMarkerUntilMs = now + 700;
    if (now - lastTouchLogMs >= 180) {
      lastTouchLogMs = now;
      Serial.printf("[TOUCH] raw x=%u y=%u down=%d count=%lu\n",
                    x, y, down, static_cast<unsigned long>(touchCount));
    }
  }
  if (down && !touchWasDown) {
    ++touchCount;
    touchStartX = x;
    touchStartY = y;
    dragLastX = x;
    dragLastY = y;
    mapDragging = false;
    dragCandidate = appView == AppView::Radar && selectedPlane < 0 && inRadarCircle(x, y) && !inBottomNav(x, y);
  } else if (down && touchWasDown && dragCandidate) {
    const int totalDx = static_cast<int>(x) - static_cast<int>(touchStartX);
    const int totalDy = static_cast<int>(y) - static_cast<int>(touchStartY);
    if (!mapDragging && totalDx * totalDx + totalDy * totalDy > 9 * 9) {
      mapDragging = true;
      selectedPlane = -1;
    }
    if (mapDragging) {
      const int dx = static_cast<int>(x) - static_cast<int>(dragLastX);
      const int dy = static_cast<int>(y) - static_cast<int>(dragLastY);
      moveMapByPixels(dx, dy);
      dragLastX = x;
      dragLastY = y;
    }
  } else if (!down && touchWasDown) {
    if (mapDragging) {
      saveLocationSettings();
      mapReloadAfterMs = millis() + 250;
      lastTrafficFetchMs = 0;
      Serial.printf("[MAP] Centre manuel %.5f,%.5f\n", radarLat, radarLon);
    } else {
      handleTap(touchStartX, touchStartY);
    }
    dragCandidate = false;
    mapDragging = false;
  }
  touchWasDown = down;
}

void trafficFetchTask(void*) {
  for (;;) {
    if (trafficFetchRequested && !trafficFetchInProgress) {
      trafficFetchRequested = false;
      trafficFetchInProgress = true;
      fetchTraffic();
      trafficFrameDirty = true;
      trafficFetchInProgress = false;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

void mapFetchTask(void*) {
  for (;;) {
    if (mapFetchRequested && !mapFetchInProgress) {
      mapFetchRequested = false;
      fetchMapTiles();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void enrichTask(void*) {
  for (;;) {
    const int request = enrichRequestedPlane;
    if (request >= 0 && !enrichInProgress) {
      enrichRequestedPlane = -1;
      enrichInProgress = true;
      enrichPlane(request);
      enrichInProgress = false;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

void startTrafficTask() {
  if (trafficTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(trafficFetchTask,
                          "flightdesk-traffic",
                          12288,
                          nullptr,
                          1,
                          &trafficTaskHandle,
                          0);
}

void startMapTask() {
  if (mapTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(mapFetchTask,
                          "flightdesk-map",
                          12288,
                          nullptr,
                          1,
                          &mapTaskHandle,
                          0);
}

void startEnrichTask() {
  if (enrichTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(enrichTask,
                          "flightdesk-enrich",
                          14336,
                          nullptr,
                          1,
                          &enrichTaskHandle,
                          0);
}

void requestTrafficFetch() {
  if (trafficFetchInProgress || trafficFetchRequested) return;
  trafficFetchRequested = true;
}

void requestMapFetch() {
  if (mapFetchInProgress || mapFetchRequested) return;
  mapFetchRequested = true;
}

void requestPlaneEnrich(int index) {
  if (!trafficLive || index < 0 || index >= livePlaneCount) return;
  if (livePlanes[index].enriched) return;
  copyClean(livePlanes[index].photo, sizeof(livePlanes[index].photo), "INFO...", "INFO...");
  enrichRequestedPlane = index;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2500);

  printHeader();
  Wire.begin(kI2cSda, kI2cScl, 400000);
  scanI2c();
  initDisplay();
  initTouch();
  initImu();
  loadWifiSettings();
  loadLocationSettings();
  connectWifi();
  initOta();
  startTrafficTask();
  startMapTask();
  startEnrichTask();
  bootMs = millis();
  printStatus();
}

void loop() {
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    initOta();
    ArduinoOTA.handle();
  }

  if (setupPortalActive) {
    setupServer.handleClient();
  }

  if (wifiReconnectAtMs && now >= wifiReconnectAtMs) {
    wifiReconnectAtMs = 0;
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false);
    delay(50);
    WiFi.begin(activeWifiSsid(), activeWifiPassword());
    Serial.printf("[WIFI] Reconnexion avec SSID stocke: %s\n", activeWifiSsid());
  }

  if (now - lastStatusMs >= 15000) {
    lastStatusMs = now;
    printStatus();
  }

  if ((!mapReady || mapDirty) && WiFi.status() == WL_CONNECTED && now - bootMs >= 6000 &&
      (mapReloadAfterMs == 0 || now >= mapReloadAfterMs) &&
      (lastMapFetchMs == 0 || mapDirty || now - lastMapFetchMs >= 60000)) {
    lastMapFetchMs = now;
    requestMapFetch();
  }

  if (WiFi.status() == WL_CONNECTED && now - bootMs >= 3000 &&
      (lastTrafficFetchMs == 0 || now - lastTrafficFetchMs >= 20000)) {
    lastTrafficFetchMs = now;
    requestTrafficFetch();
  } else if (WiFi.status() != WL_CONNECTED && trafficLive) {
    trafficFetchRequested = false;
    trafficLive = false;
    livePlaneCount = 0;
    selectedPlane = -1;
  }

  if (trafficFrameDirty) {
    trafficFrameDirty = false;
  }

  if (now - lastTouchPollMs >= 30) {
    lastTouchPollMs = now;
    pollTouch();
  }

  if (now - lastImuMs >= 200) {
    lastImuMs = now;
    pollImu();
  }

  if (now - lastRadarMs >= 16) {
    const uint32_t elapsedMs = lastRadarMs == 0 ? 16 : now - lastRadarMs;
    lastRadarMs = now;
    sweepDeg += min<uint32_t>(elapsedMs, 80) * 0.115f;
    if (sweepDeg >= 360.0f) {
      sweepDeg -= 360.0f;
    }
    drawRadarFrame();
  }
  yield();
}
