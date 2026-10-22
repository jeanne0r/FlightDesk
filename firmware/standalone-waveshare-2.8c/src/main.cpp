#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
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
#include "google_roots.h"
#include "photo_decode.h"
#include "ui_font.h"

namespace {

constexpr const char* kBuildName = "FlightDesk Waveshare 2.8C web UI port";
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
uint16_t* mapFetchFrame = nullptr;
uint16_t* mapDrawTarget = nullptr;
uint16_t* radarBaseFrame = nullptr;
uint8_t* radarAngleBins = nullptr;
uint8_t* tileBuffer = nullptr;
size_t tileBufferSize = 0;
uint8_t* jpegBuffer = nullptr;
size_t jpegBufferSize = 0;
uint16_t* selectedPhotoFrame = nullptr;
char selectedPhotoHex[8] = "";
PNG png;
Preferences prefs;
WebServer setupServer(80);
WebServer statusServer(8081);
char lastDetailStatus[72] = "aucune requete";
int lastPhotoHttpCode = 0;
char lastPhotoStatus[120] = "aucune photo";
char storedGeminiKey[128] = "";
char setupApPassword[13] = "";
char geminiStatus[64] = "CLE ABSENTE";
int lastGeminiHttpCode = 0;
bool geminiTlsOk = false;
volatile bool geminiTestRequested = false;
volatile bool geminiTestInProgress = false;
TaskHandle_t geminiTaskHandle = nullptr;

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
char selectedHex[8] = "";
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
bool tileCacheReady = false;
bool mapDirty = false;
int embeddedMapOffsetX = 0;
int embeddedMapOffsetY = 0;
uint32_t mapGeneration = 0;
int lastMapTilesLoaded = 0;
int lastMapTilesRequested = 0;
int lastMapTileHttpCode = 0;
bool radarBaseDirty = true;
bool mapFetchInProgress = false;
uint32_t lastTrafficFetchMs = 0;
uint32_t lastTrafficAnimationMs = 0;
bool trafficLive = false;
bool trafficFetchFailed = false;
int lastTrafficHttpCode = 0;
int lastTrafficReceived = 0;
int lastTrafficApiTotal = 0;
int lastTrafficRawHexCount = 0;
int lastTrafficPayloadBytes = 0;
bool lastTrafficJsonOverflow = false;
char lastTrafficProvider[20] = "";

class SpiRamJsonAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  void deallocate(void* ptr) override { heap_caps_free(ptr); }
  void* reallocate(void* ptr, size_t size) override {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
SpiRamJsonAllocator trafficJsonAllocator;
constexpr size_t kTrafficPayloadCapacity = 512 * 1024;
char* trafficPayloadBuffer = nullptr;

class TrafficPayloadStream : public Stream {
 public:
  TrafficPayloadStream(char* data, size_t capacity) : data_(data), capacity_(capacity) {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (size > capacity_ - length_) return 0;
    memcpy(data_ + length_, data, size);
    length_ += size;
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t length() const { return length_; }

 private:
  char* data_;
  size_t capacity_;
  size_t length_ = 0;
};
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
bool setupApStarted = false;
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
int radarRangeKm = 50;
constexpr int kRadarCx = 240;
constexpr int kRadarCy = 240;
// Keep the bright radar rim close to the round display edge.
constexpr int kRadarRadius = 230;
constexpr int kMapRadius = 234;
constexpr int kSweepBins = 240;
constexpr int kSweepTrailBins = 43;
constexpr int kPhotoW = 106;
constexpr int kPhotoH = 70;
int pngTileScreenX = 0;
int pngTileScreenY = 0;
double radarLat = kHomeLat;
double radarLon = kHomeLon;
char currentPostal[8] = "1188";
char homePostal[8] = "1188";
double homeLat = kHomeLat;
double homeLon = kHomeLon;
bool postalKeyboardOpen = false;
char postalInput[5] = "";
char postalError[32] = "";

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
  float screenX;
  float screenY;
  float trackDeg;
  uint32_t sampleMs;
  bool motionKnown;
  double lat;
  double lon;
  char hex[8];
  char callsign[12];
  char type[20];
  char registration[16];
  char airline[28];
  char route[36];
  char photo[16];
  char photoUrl[160];
  int distanceKm;
  int altitudeM;
  int speedKmh;
  bool enriched;
};

constexpr int kMaxLivePlanes = 512;
LivePlane* livePlanes = nullptr;
LivePlane* fetchedPlanes = nullptr;
int livePlaneCount = 0;

enum class AppView : uint8_t {
  Radar,
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
      const int targetX = x + embeddedMapOffsetX;
      const int targetY = y + embeddedMapOffsetY;
      const int dx = targetX - kRadarCx;
      const int dy = targetY - kRadarCy;
      if (targetX >= 0 && targetX < kWidth && targetY >= 0 && targetY < kHeight &&
          dx * dx + dy * dy <= (kMapRadius - 2) * (kMapRadius - 2)) {
        const uint8_t paletteIndex = static_cast<uint8_t>((packed >> shift) & 0x1F);
        const uint16_t color = pgm_read_word(&kEmbeddedMapPalette[paletteIndex]);
        const int target = targetY * kWidth + targetX;
        frame[target] = blend565(frame[target], color, alpha);
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
  if (!mapDrawTarget || x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  mapDrawTarget[y * kWidth + x] = color;
}

uint16_t radarTint(uint16_t rgb) {
  const uint8_t r = ((rgb >> 11) & 0x1F) << 3;
  const uint8_t g = ((rgb >> 5) & 0x3F) << 2;
  const uint8_t b = (rgb & 0x1F) << 3;
  const uint8_t lum = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
  return rgb565(2 + lum / 26, 12 + lum / 3, 6 + lum / 18);
}

void clearMapFrame(uint16_t* target) {
  if (!target) return;
  for (int i = 0; i < kWidth * kHeight; ++i) {
    target[i] = rgb565(2, 5, 3);
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

double pixelYToLat(double pixelY, int zoom) {
  const double scale = 256.0 * static_cast<double>(1UL << zoom);
  const double mercator = PI * (1.0 - 2.0 * pixelY / scale);
  return atan(sinh(mercator)) / DEG_TO_RAD;
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

  char cachePath[48];
  snprintf(cachePath, sizeof(cachePath), "/osm_%d_%d_%d.png", z, x, y);
  size_t total = 0;
  if (tileCacheReady && LittleFS.exists(cachePath)) {
    File cached = LittleFS.open(cachePath, "r");
    if (cached && cached.size() >= 64 && cached.size() < tileBufferSize) {
      total = cached.read(tileBuffer, cached.size());
    }
    cached.close();
  }

  if (total < 64) {
    total = 0;
    char url[96];
    snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setUserAgent("FlightDesk/0.3 (+https://github.com/jeanne0r/FlightDesk)");
    http.setTimeout(8000);
    if (!http.begin(client, url)) return false;
    const int code = http.GET();
    lastMapTileHttpCode = code;
    if (code != HTTP_CODE_OK) {
      Serial.printf("[MAP] Tile HTTP %d %s\n", code, url);
      http.end();
      return false;
    }
    WiFiClient* stream = http.getStreamPtr();
    const int contentLength = http.getSize();
    const uint32_t started = millis();
    while (total < tileBufferSize && millis() - started < 12000) {
      const size_t available = stream->available();
      if (available == 0) {
        if (contentLength > 0 && total >= static_cast<size_t>(contentLength)) break;
        if (total > 0 && !stream->connected()) break;
        delay(1);
        continue;
      }
      const size_t chunk = min(available, tileBufferSize - total);
      const int read = stream->readBytes(tileBuffer + total, chunk);
      if (read <= 0) break;
      total += read;
      if (contentLength > 0 && total >= static_cast<size_t>(contentLength)) break;
    }
    http.end();
    if (total >= 64 && total < tileBufferSize && tileCacheReady) {
      File cached = LittleFS.open(cachePath, "w");
      if (cached) {
        const bool complete = cached.write(tileBuffer, total) == total;
        cached.close();
        if (!complete) LittleFS.remove(cachePath);
      }
    }
  }
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
  if (!mapFrame || !mapFetchFrame || !tileBuffer || WiFi.status() != WL_CONNECTED || mapFetchInProgress) return;
  mapFetchInProgress = true;
  const uint32_t startedGeneration = mapGeneration;
  const double centerLat = radarLat;
  const double centerLon = radarLon;
  const int zoom = zoomForRange(centerLat, radarRangeKm, kRadarRadius);
  mapDrawTarget = mapFetchFrame;
  clearMapFrame(mapDrawTarget);
  const double centerX = lonToPixelX(centerLon, zoom);
  const double centerY = latToPixelY(centerLat, zoom);
  const double topLeftX = centerX - kRadarCx;
  const double topLeftY = centerY - kRadarCy;
  const int minTileX = floor(topLeftX / 256.0);
  const int minTileY = floor(topLeftY / 256.0);
  const int maxTileX = floor((topLeftX + kWidth) / 256.0);
  const int maxTileY = floor((topLeftY + kHeight) / 256.0);
  int ok = 0;
  int total = 0;
  Serial.printf("[MAP] Fetch OSM z=%d center=%.5f,%.5f npa=%s\n", zoom, centerLat, centerLon, currentPostal);
  for (int ty = minTileY; ty <= maxTileY; ++ty) {
    for (int tx = minTileX; tx <= maxTileX; ++tx) {
      if (startedGeneration != mapGeneration || mapDragging) break;
      const int screenX = static_cast<int>(round(tx * 256.0 - topLeftX));
      const int screenY = static_cast<int>(round(ty * 256.0 - topLeftY));
      ++total;
      if (fetchTilePng(zoom, tx, ty, screenX, screenY)) {
        ++ok;
      }
      delay(80);
    }
    if (startedGeneration != mapGeneration || mapDragging) break;
  }
  lastMapTilesLoaded = ok;
  lastMapTilesRequested = total;
  if (ok > 0 && startedGeneration == mapGeneration && !mapDragging) {
    uint16_t* oldFrame = mapFrame;
    mapFrame = mapFetchFrame;
    mapFetchFrame = oldFrame;
    mapReady = true;
    mapDirty = false;
    radarBaseDirty = true;
  }
  mapDrawTarget = nullptr;
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
  if (trafficLive) return livePlaneCount;
  return WiFi.status() == WL_CONNECTED ? 0 : static_cast<int>(sizeof(kPlanes) / sizeof(kPlanes[0]));
}

int planeX(int index) {
  if (trafficLive && livePlaneCount > 0) return livePlanes[index].x;
  return kRadarCx + (fallbackPlane(index).x - kRadarCx) * 50 / radarRangeKm;
}

int planeY(int index) {
  if (trafficLive && livePlaneCount > 0) return livePlanes[index].y;
  return kRadarCy + (fallbackPlane(index).y - kRadarCy) * 50 / radarRangeKm;
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

bool planeVisible(int index) {
  if (planeDistanceKm(index) > radarRangeKm) return false;
  const int dx = planeX(index) - kRadarCx;
  const int dy = planeY(index) - kRadarCy;
  return dx * dx + dy * dy <= (kRadarRadius - 22) * (kRadarRadius - 22);
}

int visibleAircraftCount() {
  int visible = 0;
  for (int i = 0; i < aircraftCount(); ++i) {
    if (planeVisible(i)) ++visible;
  }
  return visible;
}

bool updateLivePlanePosition(LivePlane& p) {
  const double dist = distanceKm(radarLat, radarLon, p.lat, p.lon);
  if (dist > radarRangeKm || dist < 0.1) return false;
  const double bearing = bearingDeg(radarLat, radarLon, p.lat, p.lon);
  const double posR = (dist / radarRangeKm) * (kRadarRadius - 26);
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
    p.screenX = p.x;
    p.screenY = p.y;
    livePlanes[write++] = p;
  }
  livePlaneCount = write;
  if (selectedPlane >= livePlaneCount) selectedPlane = -1;
}

void animateLivePlanePositions(uint32_t now, uint32_t stepMs) {
  if (!trafficLive || !livePlanes || mapDragging || radarRangeKm <= 0) return;
  const float blend = min(1.0f, static_cast<float>(stepMs) / 550.0f);
  for (int i = 0; i < livePlaneCount; ++i) {
    LivePlane& p = livePlanes[i];
    double lat = p.lat;
    double lon = p.lon;
    if (p.motionKnown && p.speedKmh > 0) {
      const uint32_t ageMs = min<uint32_t>(now - p.sampleMs, 45000);
      const double travelKm = p.speedKmh * (static_cast<double>(ageMs) / 3600000.0);
      const double trackRad = p.trackDeg * DEG_TO_RAD;
      lat += cos(trackRad) * travelKm / 111.32;
      const double cosLat = cos(lat * DEG_TO_RAD);
      if (fabs(cosLat) > 0.01) lon += sin(trackRad) * travelKm / (111.32 * cosLat);
    }
    const double dist = distanceKm(radarLat, radarLon, lat, lon);
    const double bearing = bearingDeg(radarLat, radarLon, lat, lon) * DEG_TO_RAD;
    const double radius = (dist / radarRangeKm) * (kRadarRadius - 26);
    const float targetX = kRadarCx + sin(bearing) * radius;
    const float targetY = kRadarCy - cos(bearing) * radius;
    p.screenX += (targetX - p.screenX) * blend;
    p.screenY += (targetY - p.screenY) * blend;
    p.x = static_cast<int>(round(p.screenX));
    p.y = static_cast<int>(round(p.screenY));
    p.distanceKm = static_cast<int>(round(dist));
  }
}

bool fetchTrafficPayload(const char* provider, const char* url, size_t& payloadLength) {
  if (!trafficPayloadBuffer) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setTimeout(12000);
  if (!http.begin(client, url)) {
    Serial.printf("[TRAFFIC] %s begin KO\n", provider);
    return false;
  }
  const int code = http.GET();
  lastTrafficHttpCode = code;
  if (code != HTTP_CODE_OK) {
    Serial.printf("[TRAFFIC] %s HTTP KO code=%d\n", provider, code);
    http.end();
    return false;
  }
  TrafficPayloadStream payload(trafficPayloadBuffer, kTrafficPayloadCapacity);
  const int received = http.writeToStream(&payload);
  http.end();
  if (received < 0 || static_cast<size_t>(received) != payload.length()) {
    Serial.printf("[TRAFFIC] %s body KO code=%d bytes=%u\n", provider, received,
                  static_cast<unsigned>(payload.length()));
    return false;
  }
  payloadLength = payload.length();
  trafficPayloadBuffer[payloadLength] = '\0';
  Serial.printf("[TRAFFIC] payload OK via %s bytes=%u\n", provider,
                static_cast<unsigned>(payloadLength));
  return true;
}

void fetchTraffic() {
  if (!livePlanes || !fetchedPlanes) return;
  if (WiFi.status() != WL_CONNECTED) {
    trafficLive = false;
    livePlaneCount = 0;
    return;
  }

  char url[3][144];
  const int radiusNm = max(1, static_cast<int>(round(radarRangeKm / 1.852)));
  snprintf(url[0], sizeof(url[0]), "https://opendata.adsb.fi/api/v3/lat/%.5f/lon/%.5f/dist/%d",
           radarLat, radarLon, radiusNm);
  snprintf(url[1], sizeof(url[1]), "https://api.adsb.lol/v2/point/%.5f/%.5f/%d",
           radarLat, radarLon, radiusNm);
  snprintf(url[2], sizeof(url[2]), "https://api.airplanes.live/v2/point/%.5f/%.5f/%d",
           radarLat, radarLon, radiusNm);

  size_t payloadLength = 0;
  const char* provider = nullptr;
  const uint32_t generation = mapGeneration;
  const double requestedLat = radarLat;
  const double requestedLon = radarLon;
  const int requestedRange = radarRangeKm;
  if (fetchTrafficPayload("adsb.fi", url[0], payloadLength)) {
    provider = "adsb.fi";
  } else if (fetchTrafficPayload("adsb.lol", url[1], payloadLength)) {
    provider = "adsb.lol";
  } else if (fetchTrafficPayload("airplanes.live", url[2], payloadLength)) {
    provider = "airplanes.live";
  } else {
    trafficFetchFailed = true;
    trafficFrameDirty = true;
    Serial.println("[TRAFFIC] all live providers KO");
    return;
  }

  lastTrafficPayloadBytes = payloadLength;
  lastTrafficRawHexCount = 0;
  for (size_t pos = 0; pos + 6 <= payloadLength; ++pos) {
    if (memcmp(trafficPayloadBuffer + pos, "\"hex\":", 6) == 0) ++lastTrafficRawHexCount;
  }
  JsonDocument filter;
  const char* fields[] = {"lat", "lon", "track", "true_heading", "hex", "flight", "r",
                          "t", "desc", "route", "alt_baro", "alt_geom", "gs"};
  for (const char* field : fields) {
    filter["ac"][0][field] = true;
    filter["aircraft"][0][field] = true;
  }
  filter["total"] = true;
  JsonDocument doc(&trafficJsonAllocator);
  DeserializationError err = deserializeJson(doc, trafficPayloadBuffer, payloadLength,
                                              DeserializationOption::Filter(filter));
  lastTrafficJsonOverflow = doc.overflowed();
  if (err || lastTrafficJsonOverflow) {
    trafficFetchFailed = true;
    Serial.printf("[TRAFFIC] JSON KO %s overflow=%d bytes=%u\n", err.c_str(),
                  lastTrafficJsonOverflow, static_cast<unsigned>(payloadLength));
    return;
  }

  JsonArray aircraft = doc["ac"].as<JsonArray>();
  if (aircraft.isNull()) aircraft = doc["aircraft"].as<JsonArray>();
  lastTrafficReceived = aircraft.size();
  lastTrafficApiTotal = doc["total"] | 0;
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
    const double heading = !ac["track"].isNull() ? ac["track"].as<double>()
                            : !ac["true_heading"].isNull() ? ac["true_heading"].as<double>()
                            : bearing;
    p.heading = static_cast<int>(round(heading)) - 90;
    p.trackDeg = static_cast<float>(heading);
    p.sampleMs = millis();
    p.motionKnown = !ac["track"].isNull() || !ac["true_heading"].isNull();
    p.screenX = p.x;
    p.screenY = p.y;
    copyClean(p.hex, sizeof(p.hex), ac["hex"] | "", "");
    copyClean(p.callsign, sizeof(p.callsign), ac["flight"] | ac["r"] | ac["hex"] | "", "VOL");
    copyClean(p.type, sizeof(p.type), ac["t"] | ac["desc"] | "", "TYPE INCONNU");
    copyClean(p.registration, sizeof(p.registration), ac["r"] | "", "");
    p.airline[0] = '\0';
    formatRoute(p.route, sizeof(p.route), ac["route"] | "");
    copyClean(p.photo, sizeof(p.photo), "", "INFO...");
    p.photoUrl[0] = '\0';
    const double altitudeFt = !ac["alt_baro"].isNull() ? ac["alt_baro"].as<double>()
                               : ac["alt_geom"].as<double>();
    p.altitudeM = static_cast<int>(round(altitudeFt * 0.3048));
    p.speedKmh = ac["gs"].isNull() ? -1 : static_cast<int>(round(ac["gs"].as<double>() * 1.852));
    p.enriched = false;
    ++count;
  }

  if (generation != mapGeneration || requestedLat != radarLat || requestedLon != radarLon ||
      requestedRange != radarRangeKm) {
    lastTrafficFetchMs = 0;
    Serial.println("[TRAFFIC] obsolete response discarded after map change");
    return;
  }

  // Keep downloaded details across the 20-second traffic refresh. Array order
  // changes frequently, so the ICAO hex identifies each aircraft.
  for (int i = 0; i < count; ++i) {
    LivePlane& next = fetchedPlanes[i];
    if (!next.hex[0]) continue;
    for (int old = 0; old < livePlaneCount; ++old) {
      const LivePlane& previous = livePlanes[old];
      if (strcmp(next.hex, previous.hex) != 0) continue;
      next.screenX = previous.screenX;
      next.screenY = previous.screenY;
      next.x = previous.x;
      next.y = previous.y;
      if (previous.enriched) {
        copyClean(next.type, sizeof(next.type), previous.type, next.type);
        copyClean(next.registration, sizeof(next.registration), previous.registration, next.registration);
        copyClean(next.airline, sizeof(next.airline), previous.airline, "");
        copyClean(next.route, sizeof(next.route), previous.route, next.route);
        copyClean(next.photo, sizeof(next.photo), previous.photo, "");
        copyClean(next.photoUrl, sizeof(next.photoUrl), previous.photoUrl, "");
        next.enriched = true;
      }
      break;
    }
  }

  for (int i = 0; i < count; ++i) {
    livePlanes[i] = fetchedPlanes[i];
  }
  livePlaneCount = count;
  trafficLive = true;
  trafficFetchFailed = false;
  copyClean(lastTrafficProvider, sizeof(lastTrafficProvider), provider, "");
  if (selectedPlane >= 0) {
    selectedPlane = -1;
    for (int i = 0; i < livePlaneCount; ++i) {
      if (selectedHex[0] && strcmp(selectedHex, livePlanes[i].hex) == 0) {
        selectedPlane = i;
        break;
      }
    }
  }
  trafficFrameDirty = true;
  Serial.printf("[TRAFFIC] %d/%d avion(s) live via %s, %d bytes\n", livePlaneCount,
                lastTrafficReceived, provider, lastTrafficPayloadBytes);
}

bool httpsGetJson(const char* url, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setTimeout(7000);
  if (!http.begin(client, url)) {
    snprintf(lastDetailStatus, sizeof(lastDetailStatus), "begin KO: %.40s", url);
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(lastDetailStatus, sizeof(lastDetailStatus), "HTTP %d: %.40s", code, url);
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    snprintf(lastDetailStatus, sizeof(lastDetailStatus), "JSON %s: %.36s", error.c_str(), url);
  } else {
    snprintf(lastDetailStatus, sizeof(lastDetailStatus), "OK: %.50s", url);
  }
  return !error;
}

void clearSelectedPhoto(uint16_t color = rgb565(5, 28, 22)) {
  if (!selectedPhotoFrame) return;
  selectedPhotoHex[0] = '\0';
  for (int i = 0; i < kPhotoW * kPhotoH; ++i) {
    selectedPhotoFrame[i] = color;
  }
}

bool downloadSelectedPhoto(const char* url) {
  if (!url || !strlen(url) || !jpegBuffer || !selectedPhotoFrame || WiFi.status() != WL_CONNECTED) {
    copyClean(lastPhotoStatus, sizeof(lastPhotoStatus), "conditions KO", "");
    return false;
  }
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  WiFiClient& client = strncmp(url, "http://", 7) == 0
                           ? static_cast<WiFiClient&>(plainClient)
                           : static_cast<WiFiClient&>(secureClient);
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(5500);
  if (!http.begin(client, url)) {
    copyClean(lastPhotoStatus, sizeof(lastPhotoStatus), "begin KO", "");
    return false;
  }
  const int code = http.GET();
  lastPhotoHttpCode = code;
  if (code != HTTP_CODE_OK) {
    snprintf(lastPhotoStatus, sizeof(lastPhotoStatus), "HTTP %d", code);
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
  if (total < 128 || total >= jpegBufferSize) {
    snprintf(lastPhotoStatus, sizeof(lastPhotoStatus), "octets=%u attendus=%d", total, contentLength);
    return false;
  }

  clearSelectedPhoto();
  int sourceWidth = 0;
  int sourceHeight = 0;
  const bool decoded = decodePhotoJpeg(jpegBuffer, total, selectedPhotoFrame,
                                       kPhotoW, kPhotoH, &sourceWidth, &sourceHeight);
  if (decoded) {
    snprintf(lastPhotoStatus, sizeof(lastPhotoStatus), "JPEG %dx%d rendu %dx%d",
             sourceWidth, sourceHeight, kPhotoW, kPhotoH);
  } else {
    snprintf(lastPhotoStatus, sizeof(lastPhotoStatus), "JPEG KO: %.70s",
             photoDecodeError() ? photoDecodeError() : "inconnu");
  }
  return decoded;
}

void enrichPlane(int index) {
  if (!trafficLive || index < 0 || index >= livePlaneCount || livePlanes[index].enriched) return;
  LivePlane p = livePlanes[index];
  copyClean(p.photo, sizeof(p.photo), "", "INFO...");
  bool photoReady = false;

  char url[128];
  JsonDocument doc;
  if (strlen(p.hex)) {
    // ADSBdb can return the airframe and route in one request.
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s?callsign=%s", p.hex, p.callsign);
    if (httpsGetJson(url, doc)) {
      const char* type = doc["response"]["aircraft"]["type"].as<const char*>();
      const char* registration = doc["response"]["aircraft"]["registration"].as<const char*>();
      const char* owner = doc["response"]["aircraft"]["registered_owner"].as<const char*>();
      const char* photo = doc["response"]["aircraft"]["url_photo_thumbnail"].as<const char*>();
      const char* from = doc["response"]["flightroute"]["origin"]["municipality"].as<const char*>();
      const char* to = doc["response"]["flightroute"]["destination"]["municipality"].as<const char*>();
      const char* airline = doc["response"]["flightroute"]["airline"]["name"].as<const char*>();
      if (type && *type) copyClean(p.type, sizeof(p.type), type, p.type);
      if (registration) copyClean(p.registration, sizeof(p.registration), registration, p.registration);
      if (owner) copyClean(p.airline, sizeof(p.airline), owner, "");
      if (airline) copyClean(p.airline, sizeof(p.airline), airline, p.airline);
      if (photo && *photo) copyClean(p.photoUrl, sizeof(p.photoUrl), photo, "");
      if (from && to) snprintf(p.route, sizeof(p.route), "%s -> %s", from, to);
    }

    if (strstr(p.route, "inconnus") && p.callsign[0]) {
      doc.clear();
      snprintf(url, sizeof(url), "https://hexdb.io/api/v1/route/icao/%s", p.callsign);
      if (httpsGetJson(url, doc)) {
        const char* route = doc["route"].as<const char*>();
        if (route && strchr(route, '-')) {
          copyClean(p.route, sizeof(p.route), route, p.route);
        }
      }
    }

    // HexDB covers smaller aircraft absent from ADSBdb, including FSF718Z.
    if (!p.registration[0] || strcmp(p.type, "TYPE INCONNU") == 0) {
      doc.clear();
      snprintf(url, sizeof(url), "https://hexdb.io/api/v1/aircraft/%s", p.hex);
      if (httpsGetJson(url, doc)) {
        const char* type = doc["Type"].as<const char*>();
        const char* registration = doc["Registration"].as<const char*>();
        const char* owner = doc["RegisteredOwners"].as<const char*>();
        if (type && *type) copyClean(p.type, sizeof(p.type), type, p.type);
        if (registration && *registration) copyClean(p.registration, sizeof(p.registration), registration, p.registration);
        if (owner && *owner) copyClean(p.airline, sizeof(p.airline), owner, p.airline);
      }
    }

    // Planespotters has photos for airframes with no ADSBdb record.
    if (!p.photoUrl[0]) {
      doc.clear();
      snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", p.hex);
      if (httpsGetJson(url, doc)) {
        JsonArray photos = doc["photos"].as<JsonArray>();
        const char* photo = photos.size()
                                ? photos[static_cast<size_t>(0)]["thumbnail"]["src"].as<const char*>()
                                : nullptr;
        snprintf(lastDetailStatus, sizeof(lastDetailStatus), "photos=%u url=%s", photos.size(), photo ? "oui" : "non");
        if (photo && *photo) copyClean(p.photoUrl, sizeof(p.photoUrl), photo, "");
      }
    }
    if (p.photoUrl[0]) {
      photoReady = downloadSelectedPhoto(p.photoUrl);
      copyClean(p.photo, sizeof(p.photo), photoReady ? "PHOTO OK" : "PHOTO KO", "PHOTO KO");
    } else {
      copyClean(p.photo, sizeof(p.photo), "SANS PHOTO", "SANS PHOTO");
    }
  }

  p.enriched = true;
  for (int current = 0; current < livePlaneCount; ++current) {
    if (!p.hex[0] || strcmp(livePlanes[current].hex, p.hex) != 0) continue;
    LivePlane& target = livePlanes[current];
    copyClean(target.route, sizeof(target.route), p.route, target.route);
    copyClean(target.type, sizeof(target.type), p.type, target.type);
    copyClean(target.registration, sizeof(target.registration), p.registration, target.registration);
    copyClean(target.airline, sizeof(target.airline), p.airline, "");
    copyClean(target.photoUrl, sizeof(target.photoUrl), p.photoUrl, "");
    copyClean(target.photo, sizeof(target.photo), p.photo, "");
    target.enriched = true;
    if (photoReady && selectedHex[0] && strcmp(selectedHex, p.hex) == 0) {
      copyClean(selectedPhotoHex, sizeof(selectedPhotoHex), p.hex, "");
    }
    Serial.printf("[DETAIL] %s route=%s photo=%s\n", p.callsign, p.route, p.photo);
    break;
  }
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
  const UiGlyph* glyphs = scale >= 3 ? kUiGlyphs27 :
                          scale == 2 ? kUiGlyphs18 :
                          scale == 1 ? kUiGlyphs12 : kUiGlyphs9;
  int width = 0;
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
    const int index = *c >= 32 && *c <= 126 ? *c - 32 : '?' - 32;
    UiGlyph glyph;
    memcpy_P(&glyph, &glyphs[index], sizeof(glyph));
    width += glyph.advance;
  }
  return width;
}

void drawText(int x, int y, const char* text, uint16_t color, int scale = 1) {
  const UiGlyph* glyphs = scale >= 3 ? kUiGlyphs27 :
                          scale == 2 ? kUiGlyphs18 :
                          scale == 1 ? kUiGlyphs12 : kUiGlyphs9;
  const uint8_t* pixels = scale >= 3 ? kUiPixels27 :
                          scale == 2 ? kUiPixels18 :
                          scale == 1 ? kUiPixels12 : kUiPixels9;
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
    const int index = *c >= 32 && *c <= 126 ? *c - 32 : '?' - 32;
    UiGlyph glyph;
    memcpy_P(&glyph, &glyphs[index], sizeof(glyph));
    for (int row = 0; row < glyph.height; ++row) {
      for (int col = 0; col < glyph.width; ++col) {
        const int pixel = row * glyph.width + col;
        const uint8_t packed = pgm_read_byte(&pixels[glyph.offset + pixel / 2]);
        const uint8_t level = pixel & 1 ? packed & 0x0F : packed >> 4;
        if (level == 15) {
          putPixel(x + glyph.x + col, y + glyph.y + row, color);
        } else if (level) {
          blendPixel(x + glyph.x + col, y + glyph.y + row, color, level * 17);
        }
      }
    }
    x += glyph.advance;
  }
}

void drawTextCentered(int cx, int y, const char* text, uint16_t color, int scale = 1) {
  drawText(cx - textWidth(text, scale) / 2, y, text, color, scale);
}

void drawTextOnUpperArc(int cx, int cy, int radius, const char* text, uint16_t color) {
  const float totalWidth = static_cast<float>(textWidth(text, 0));
  float advance = -totalWidth / 2.0f;
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
    const int index = *c >= 32 && *c <= 126 ? *c - 32 : '?' - 32;
    UiGlyph glyph;
    memcpy_P(&glyph, &kUiGlyphs9[index], sizeof(glyph));
    const float angle = (advance + glyph.advance / 2.0f) / radius;
    const float sine = sinf(angle);
    const float cosine = cosf(angle);
    const float originX = cx + radius * sine;
    const float originY = cy - radius * cosine;
    for (int row = 0; row < glyph.height; ++row) {
      for (int col = 0; col < glyph.width; ++col) {
        const int pixel = row * glyph.width + col;
        const uint8_t packed = pgm_read_byte(&kUiPixels9[glyph.offset + pixel / 2]);
        const uint8_t level = pixel & 1 ? packed & 0x0F : packed >> 4;
        if (!level) continue;
        const float localX = glyph.x + col - glyph.advance / 2.0f;
        const float localY = glyph.y + row - 5.0f;
        const int x = static_cast<int>(roundf(originX + localX * cosine - localY * sine));
        const int y = static_cast<int>(roundf(originY + localX * sine + localY * cosine));
        if (level == 15) putPixel(x, y, color);
        else blendPixel(x, y, color, level * 17);
      }
    }
    advance += glyph.advance;
  }
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
  if (appView == AppView::Settings && postalKeyboardOpen) {
    blendRoundRect(72, 70, 336, 340, 22, rgb565(0, 10, 9), 242);
    drawRoundRect(72, 70, 336, 340, 22, green);
    drawTextCentered(240, 90, "CODE POSTAL", green, 2);
    drawTextCentered(240, 126, postalInput[0] ? postalInput : "____", text, 2);
    drawTextCentered(240, 153, postalError, green, 1);
    drawTextCentered(382, 88, "X", text, 2);
    constexpr int keyX[] = {100, 194, 288};
    const char* keys[4][3] = {{"1", "2", "3"}, {"4", "5", "6"},
                              {"7", "8", "9"}, {"<", "0", "OK"}};
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 3; ++col) {
        const int y = 174 + row * 49;
        fillRoundRect(keyX[col], y, 86, 42, 10, rgb565(12, 26, 16));
        drawRoundRect(keyX[col], y, 86, 42, 10, green);
        drawTextCentered(keyX[col] + 43, y + 11, keys[row][col], text, 2);
      }
    }
    return;
  }
  blendRoundRect(72, 96, 336, 276, 22, rgb565(0, 10, 9), 232);
  drawRoundRect(72, 96, 336, 276, 22, rgb565(62, 176, 98));
  if (appView == AppView::Settings) {
    drawTextCentered(240, 120, "REGLAGES", green, 2);
    drawText(100, 151, "CODE POSTAL", text, 1);
    fillRoundRect(154, 159, 172, 36, 10, rgb565(12, 26, 16));
    drawRoundRect(154, 159, 172, 36, 10, green);
    drawTextCentered(240, 168, currentPostal, green, 2);
    drawText(100, 199, "RAYON RADAR", text, 1);
    constexpr int ranges[] = {20, 50, 100, 250};
    for (int i = 0; i < 4; ++i) {
      const int x = 100 + i * 72;
      const bool active = radarRangeKm == ranges[i];
      fillRoundRect(x, 214, 64, 34, 11, active ? rgb565(19, 63, 30) : rgb565(14, 19, 16));
      drawRoundRect(x, 214, 64, 34, 11, active ? green : rgb565(55, 75, 58));
      char range[5];
      snprintf(range, sizeof(range), "%d", ranges[i]);
      drawTextCentered(x + 32, 225, range, active ? green : text, 1);
    }
    fillRoundRect(100, 296, 128, 30, 10, rgb565(12, 26, 16));
    drawRoundRect(100, 296, 128, 30, 10, green);
    drawTextCentered(164, 306, "WIFI SETUP", text, 1);
    fillRoundRect(252, 296, 112, 30, 10, rgb565(12, 26, 16));
    drawRoundRect(252, 296, 112, 30, 10, green);
    drawTextCentered(308, 306, "FERMER", text, 1);
    if (setupPortalActive) {
      drawTextCentered(240, 337, "FlightDesk-Setup  192.168.4.1", text, 1);
      drawTextCentered(240, 352, setupApPassword, green, 1);
    } else {
      drawTextCentered(240, 344, WiFi.status() == WL_CONNECTED ? activeWifiSsid() : "WIFI HORS LIGNE", text, 1);
    }
    return;
  }

  drawTextCentered(240, 122, "ASSISTANT IA", green, 2);
  drawTextCentered(240, 164, geminiStatus, text, 1);
  drawTextCentered(240, 188, "Gemini 3.5 Flash-Lite", green, 1);
  if (setupPortalActive) {
    drawTextCentered(240, 222, "FlightDesk-Setup", text, 1);
    drawTextCentered(240, 241, setupApPassword, green, 1);
    drawTextCentered(240, 260, setupApStarted ? "192.168.4.1/ai" : "AP INDISPONIBLE", text, 1);
    if (WiFi.status() == WL_CONNECTED) {
      char lanUrl[44];
      snprintf(lanUrl, sizeof(lanUrl), "ou %s/ai", WiFi.localIP().toString().c_str());
      drawTextCentered(240, 278, lanUrl, text, 1);
    }
  } else {
    drawTextCentered(240, 241, "CONFIG ouvre le Wi-Fi local", text, 1);
  }
  constexpr int buttonX[] = {94, 192, 290};
  const char* labels[] = {"CONFIG", "TEST", "FERMER"};
  for (int i = 0; i < 3; ++i) {
    fillRoundRect(buttonX[i], 304, 92, 42, 12, rgb565(12, 26, 16));
    drawRoundRect(buttonX[i], 304, 92, 42, 12, green);
    drawTextCentered(buttonX[i] + 46, 317, labels[i], text, 1);
  }
}

void drawAircraftPopup(uint16_t green, uint16_t text, uint16_t panel) {
  if (selectedPlane < 0 || selectedPlane >= aircraftCount()) return;
  const LivePlane* details = trafficLive ? &livePlanes[selectedPlane] : nullptr;
  char line[40];
  blendRoundRect(52, 124, 376, 214, 18, rgb565(0, 10, 9), 238);
  drawRoundRect(52, 124, 376, 214, 18, rgb565(76, 210, 102));
  drawText(82, 148, "AVION SELECTIONNE", green, 1);
  drawText(82, 178, planeCallsign(selectedPlane), green, 3);
  if (details && details->airline[0]) {
    snprintf(line, sizeof(line), "%.24s", details->airline);
    drawText(82, 213, line, green, 1);
  }
  snprintf(line, sizeof(line), "%.15s  %.11s", planeType(selectedPlane),
           details ? details->registration : "");
  drawText(82, 233, line, text, 1);
  snprintf(line, sizeof(line), "%.27s", planeRoute(selectedPlane));
  drawText(82, 252, line, text, 1);
  if (planeSpeedKmh(selectedPlane) >= 0) {
    snprintf(line, sizeof(line), "%dKM  %dKM/H", planeDistanceKm(selectedPlane), planeSpeedKmh(selectedPlane));
  } else {
    snprintf(line, sizeof(line), "%dKM  -- KM/H", planeDistanceKm(selectedPlane));
  }
  drawText(82, 280, line, text, 1);
  snprintf(line, sizeof(line), "%dM  %dDEG", planeAltitudeM(selectedPlane), planeHeading(selectedPlane) + 90);
  drawText(82, 303, line, text, 1);

  blendRoundRect(286, 218, 106, 70, 12, rgb565(8, 32, 26), 222);
  drawRoundRect(286, 218, 106, 70, 12, rgb565(88, 220, 116));
  const bool hasPhoto = trafficLive && selectedPhotoFrame && selectedHex[0] &&
                        !strcmp(selectedPhotoHex, selectedHex) &&
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
    drawTextCentered(339, 257, trafficLive ? livePlanes[selectedPlane].photo : "INDISPONIBLE", text, 1);
  }

  blendRoundRect(378, 144, 34, 34, 13, rgb565(2, 24, 16), 226);
  drawRoundRect(378, 144, 34, 34, 13, green);
  drawTextCentered(395, 153, "X", text, 2);
}

void drawScreenNav(uint16_t green, uint16_t text, uint16_t panel) {
  const char* labels[] = {"HOME", "REGL", "IA"};
  constexpr int width = 86;
  constexpr int height = 32;
  constexpr int gap = 6;
  constexpr int startX = 101;
  constexpr int y = 356;
  for (int i = 0; i < 3; ++i) {
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
      const uint8_t alpha = static_cast<uint8_t>(4 + 30.0f * angular * angular * radial);
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
  const uint16_t black = rgb565(2, 5, 3);
  const uint16_t green = rgb565(141, 255, 111);
  const uint16_t softGreen = rgb565(82, 224, 121);
  const uint16_t dim = rgb565(27, 75, 37);

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int dx = x - cx;
      const int dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > kMapRadius * kMapRadius) {
        frame[y * kWidth + x] = black;
      } else {
        frame[y * kWidth + x] = rgb565(2, 11, 5);
      }
    }
  }

  if (!mapReady) {
    blendEmbeddedMap(170);
  }
  if (mapReady && mapFrame) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const int dx = x - cx;
        const int dy = y - cy;
        const int d2 = dx * dx + dy * dy;
        if (d2 <= (kMapRadius - 2) * (kMapRadius - 2)) {
          const int index = y * kWidth + x;
          frame[index] = blend565(frame[index], mapFrame[index], 156);
        }
      }
    }
  }
  blendFillCircle(cx, cy, kMapRadius - 2, rgb565(1, 5, 2), 72);
  blendFillCircle(cx, cy, r / 2, rgb565(24, 112, 43), 6);

  blendCircle(cx, cy, kMapRadius - 1, softGreen, 46);
  blendCircle(cx, cy, r, green, 135);
  blendCircle(cx, cy, r - 1, softGreen, 60);
  blendCircle(cx, cy, r - 8, dim, 55);

  for (int ring = r / 4; ring <= r; ring += r / 4) {
    blendCircle(cx, cy, ring, softGreen, 28);
  }

  for (int a = 0; a < 360; a += 30) {
    const float rad = a * DEG_TO_RAD;
    blendLine(cx + cosf(rad) * 16, cy + sinf(rad) * 16,
              cx + cosf(rad) * r, cy + sinf(rad) * r, softGreen, 23);
  }

  drawTextOnUpperArc(cx, cy, 216, "(c) OpenStreetMap contributors", rgb565(100, 159, 107));

  frame = drawFrame;
  radarBaseDirty = false;
}

void drawRadarFrame() {
  if (!frame || !lcdPanel) return;

  constexpr int cx = kRadarCx;
  constexpr int cy = kRadarCy;
  constexpr int r = kRadarRadius;
  const uint16_t black = rgb565(2, 5, 3);
  const uint16_t green = rgb565(141, 255, 111);
  const uint16_t softGreen = rgb565(82, 224, 121);
  const uint16_t glow = rgb565(82, 224, 121);
  const uint16_t dim = rgb565(27, 75, 37);
  const uint16_t panel = rgb565(4, 10, 6);
  const uint16_t text = rgb565(238, 244, 239);

  if (!radarBaseFrame || radarBaseDirty) {
    buildRadarBaseFrame();
  }
  if (radarBaseFrame) {
    memcpy(frame, radarBaseFrame, kWidth * kHeight * sizeof(uint16_t));
  } else {
    fillCircle(cx, cy, r, black);
  }

  blendRadarSweep(cx, cy, r - 14, sweepDeg, softGreen);
  const float sweepRad = sweepDeg * DEG_TO_RAD;
  blendLine(cx, cy,
            cx + static_cast<int>(cosf(sweepRad) * (r - 10)),
            cy + static_cast<int>(sinf(sweepRad) * (r - 10)),
            green, 128);
  blendLine(cx + 1, cy,
            cx + 1 + static_cast<int>(cosf(sweepRad) * (r - 10)),
            cy + static_cast<int>(sinf(sweepRad) * (r - 10)),
            green, 45);

  const int count = aircraftCount();
  for (int planeIndex = 0; planeIndex < count; ++planeIndex) {
    if (!planeVisible(planeIndex)) continue;
    const float ar = planeHeading(planeIndex) * DEG_TO_RAD;
    const int x = planeX(planeIndex);
    const int y = planeY(planeIndex);
    const int noseX = x + static_cast<int>(cosf(ar) * 11);
    const int noseY = y + static_cast<int>(sinf(ar) * 11);
    const int leftX = x + static_cast<int>(cosf(ar + 2.55f) * 7);
    const int leftY = y + static_cast<int>(sinf(ar + 2.55f) * 7);
    const int rightX = x + static_cast<int>(cosf(ar - 2.55f) * 7);
    const int rightY = y + static_cast<int>(sinf(ar - 2.55f) * 7);
    if (planeIndex == selectedPlane) {
      drawCircle(x, y, 17, softGreen);
      blendThickLine(cx, cy, x, y, glow, 75);
    }
    blendFillCircle(x, y, planeIndex == selectedPlane ? 15 : 10, glow, planeIndex == selectedPlane ? 34 : 12);
    blendFillCircle(x, y, planeIndex == selectedPlane ? 11 : 7, green, planeIndex == selectedPlane ? 20 : 8);
    fillTriangle(noseX + 1, noseY + 1, leftX + 1, leftY + 1, rightX + 1, rightY + 1, rgb565(0, 8, 6));
    fillTriangle(noseX, noseY, leftX, leftY, rightX, rightY, green);
    drawLine(noseX, noseY, leftX, leftY, green);
    drawLine(noseX, noseY, rightX, rightY, green);
  }

  drawTextCentered(cx, 77, "18:47", text, 1);
  char innerRange[8];
  char outerRange[8];
  snprintf(innerRange, sizeof(innerRange), "%d", radarRangeKm * 2 / 5);
  snprintf(outerRange, sizeof(outerRange), "%d", radarRangeKm);
  drawText(cx + r * 41 / 100, cy + 3, innerRange, softGreen, 1);
  drawText(cx + r * 72 / 100, cy + 3, outerRange, softGreen, 1);
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

  if (!gt911Ok) {
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
  mapFetchFrame = static_cast<uint16_t*>(heap_caps_malloc(kWidth * kHeight * sizeof(uint16_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!mapFetchFrame) {
    Serial.println("[DISPLAY] Map staging framebuffer PSRAM KO");
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
  clearMapFrame(mapFrame);

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
  const String geminiKey = prefs.getString("gemini_key", FLIGHTDESK_GEMINI_API_KEY);
  const String apPassword = prefs.getString("ap_pass", "");
  prefs.end();
  copyClean(storedSsid, sizeof(storedSsid), ssid.c_str(), "");
  copyClean(storedPassword, sizeof(storedPassword), password.c_str(), "");
  copyClean(storedGeminiKey, sizeof(storedGeminiKey), geminiKey.c_str(), "");
  copyClean(setupApPassword, sizeof(setupApPassword), apPassword.c_str(), "");
  if (storedGeminiKey[0]) copyClean(geminiStatus, sizeof(geminiStatus), "PRET A TESTER", "");
  if (strlen(setupApPassword) < 8 && prefs.begin("flightdesk", false)) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 12; ++i) setupApPassword[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    setupApPassword[12] = '\0';
    prefs.putString("ap_pass", setupApPassword);
    prefs.end();
  }
}

const char* activeWifiSsid() {
  return strlen(storedSsid) ? storedSsid : FLIGHTDESK_WIFI_SSID;
}

const char* activeWifiPassword() {
  return strlen(storedSsid) ? storedPassword : FLIGHTDESK_WIFI_PASSWORD;
}

void testGeminiConnection() {
  geminiTlsOk = false;
  lastGeminiHttpCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    copyClean(geminiStatus, sizeof(geminiStatus), "WIFI HORS LIGNE", "");
    return;
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  const uint32_t started = millis();
  while (time(nullptr) < 1700000000 && millis() - started < 12000) delay(250);
  if (time(nullptr) < 1700000000) {
    copyClean(geminiStatus, sizeof(geminiStatus), "HORLOGE INDISPONIBLE", "");
    return;
  }

  WiFiClientSecure client;
  client.setCACert(kGoogleRoots);
  HTTPClient http;
  http.setTimeout(12000);
  const bool hasKey = storedGeminiKey[0] != '\0';
  const char* url = hasKey
      ? "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash-lite:generateContent"
      : "https://generativelanguage.googleapis.com/v1beta/models";
  if (!http.begin(client, url)) {
    copyClean(geminiStatus, sizeof(geminiStatus), "CONNEXION IMPOSSIBLE", "");
    return;
  }
  int code;
  if (hasKey) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-goog-api-key", storedGeminiKey);
    code = http.POST("{\"contents\":[{\"parts\":[{\"text\":\"Reponds uniquement OK.\"}]}],\"generationConfig\":{\"maxOutputTokens\":64}}");
  } else {
    code = http.GET();
  }
  lastGeminiHttpCode = code;
  geminiTlsOk = code > 0;
  if (!hasKey) {
    copyClean(geminiStatus, sizeof(geminiStatus),
              geminiTlsOk ? "RESEAU OK - CLE ABSENTE" : "TLS OU RESEAU KO", "");
  } else if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    const String response = http.getString();
    const DeserializationError error = deserializeJson(doc, response);
    const char* answer = doc["candidates"][0]["content"]["parts"][0]["text"] | "";
    copyClean(geminiStatus, sizeof(geminiStatus),
              !error && answer[0] ? "CONNEXION GEMINI OK" : "REPONSE GEMINI VIDE", "");
  } else if (code == 400 || code == 401 || code == 403) {
    copyClean(geminiStatus, sizeof(geminiStatus), "CLE REFUSEE", "");
  } else if (code == 404) {
    copyClean(geminiStatus, sizeof(geminiStatus), "MODELE INDISPONIBLE", "");
  } else if (code == 429) {
    copyClean(geminiStatus, sizeof(geminiStatus), "QUOTA DEPASSE", "");
  } else {
    copyClean(geminiStatus, sizeof(geminiStatus), "SERVICE INDISPONIBLE", "");
  }
  http.end();
  Serial.printf("[GEMINI] test HTTP=%d tls=%d status=%s\n", code, geminiTlsOk, geminiStatus);
}

void requestGeminiTest() {
  if (geminiTestRequested || geminiTestInProgress) return;
  copyClean(geminiStatus, sizeof(geminiStatus), "TEST EN COURS", "");
  geminiTestRequested = true;
}

void saveLocationSettings() {
  if (!prefs.begin("flightdesk", false)) {
    Serial.println("[LOC] NVS ecriture KO");
    return;
  }
  prefs.putString("npa", currentPostal);
  prefs.putDouble("lat", radarLat);
  prefs.putDouble("lon", radarLon);
  prefs.putString("home_npa", homePostal);
  prefs.putDouble("home_lat", homeLat);
  prefs.putDouble("home_lon", homeLon);
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
  const String savedHomePostal = prefs.getString("home_npa", npa.length() == 4 ? npa : "1188");
  homeLat = prefs.getDouble("home_lat", npa.length() == 4 ? lat : kHomeLat);
  homeLon = prefs.getDouble("home_lon", npa.length() == 4 ? lon : kHomeLon);
  const int savedRange = prefs.getInt("range", 50);
  radarRangeKm = (savedRange == 20 || savedRange == 50 || savedRange == 100 || savedRange == 250)
                     ? savedRange : 50;
  prefs.end();
  copyClean(homePostal, sizeof(homePostal), savedHomePostal.c_str(), "1188");
  copyClean(currentPostal, sizeof(currentPostal), npa.c_str(), "1188");
  radarLat = lat;
  radarLon = lon;
  radarBaseDirty = true;
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
  html += F("<p>Apres sauvegarde, FlightDesk reconnecte le radar avec ces identifiants.</p>");
  html += F("<p><a href='/ai' style='color:#86ff78'>Configurer Gemini</a></p></main></body></html>");
  setupServer.send(200, "text/html", html);
}

void handleGeminiRoot() {
  String html;
  html.reserve(1900);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>FlightDesk Gemini</title><style>body{margin:0;background:#050b08;color:#eaffea;font-family:system-ui}");
  html += F("main{max-width:420px;margin:auto;padding:28px}h1{color:#86ff78}p{line-height:1.5}");
  html += F("input,button{box-sizing:border-box;width:100%;padding:14px;margin:8px 0;border-radius:10px;font-size:18px}");
  html += F("input{background:#07130d;border:1px solid #245a38;color:white}button{border:0;background:#58f07b;color:#031009;font-weight:bold}");
  html += F("a{color:#86ff78}</style></head><body><main><h1>Gemini</h1><p>Cle API : ");
  html += storedGeminiKey[0] ? F("enregistree") : F("absente");
  html += F("</p><p>Diagnostic : ");
  html += htmlEscape(geminiStatus);
  html += F("</p><form method='post' action='/ai/save'><label>Cle API Gemini</label>");
  html += F("<input name='key' type='password' minlength='20' maxlength='127' autocomplete='off' required>");
  html += F("<button type='submit'>Enregistrer et tester</button></form>");
  html += F("<form method='post' action='/ai/test'><button type='submit'>Retester la connexion</button></form>");
  html += F("<p><a href='/'>Configuration Wi-Fi</a></p></main></body></html>");
  setupServer.sendHeader("Cache-Control", "no-store");
  setupServer.send(200, "text/html", html);
}

void handleGeminiSave() {
  String key = setupServer.arg("key");
  key.trim();
  if (key.length() < 20 || key.length() >= sizeof(storedGeminiKey) || key.indexOf(' ') >= 0) {
    setupServer.send(400, "text/plain", "Cle invalide");
    return;
  }
  if (!prefs.begin("flightdesk", false)) {
    setupServer.send(500, "text/plain", "NVS KO");
    return;
  }
  prefs.putString("gemini_key", key);
  prefs.end();
  copyClean(storedGeminiKey, sizeof(storedGeminiKey), key.c_str(), "");
  key = "";
  requestGeminiTest();
  setupServer.sendHeader("Location", "/ai");
  setupServer.send(303, "text/plain", "");
}

void handleGeminiTest() {
  requestGeminiTest();
  setupServer.sendHeader("Location", "/ai");
  setupServer.send(303, "text/plain", "");
}

void handleStatus() {
  JsonDocument doc;
  doc["build"] = kBuildName;
  doc["display"] = lcdPanel != nullptr;
  doc["touch"] = gt911Ok;
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  doc["traffic_live"] = trafficLive;
  doc["aircraft_visible"] = visibleAircraftCount();
  doc["traffic_received"] = lastTrafficReceived;
  doc["traffic_api_total"] = lastTrafficApiTotal;
  doc["traffic_raw_hex_count"] = lastTrafficRawHexCount;
  doc["traffic_json_overflow"] = lastTrafficJsonOverflow;
  doc["traffic_capacity"] = kMaxLivePlanes;
  doc["traffic_provider"] = lastTrafficProvider;
  doc["traffic_http_code"] = lastTrafficHttpCode;
  doc["traffic_payload_bytes"] = lastTrafficPayloadBytes;
  doc["traffic_fetch_failed"] = trafficFetchFailed;
  for (int i = 0; i < livePlaneCount; ++i) {
    const LivePlane& p = livePlanes[i];
    if (!p.motionKnown || p.speedKmh <= 0) continue;
    doc["motion_plane"]["hex"] = p.hex;
    doc["motion_plane"]["x"] = p.x;
    doc["motion_plane"]["y"] = p.y;
    doc["motion_plane"]["speed_kmh"] = p.speedKmh;
    doc["motion_plane"]["sample_age_ms"] = millis() - p.sampleMs;
    break;
  }
  doc["range_km"] = radarRangeKm;
  doc["npa"] = currentPostal;
  doc["home_npa"] = homePostal;
  doc["postal_keyboard_open"] = postalKeyboardOpen;
  doc["center_lat"] = radarLat;
  doc["center_lon"] = radarLon;
  doc["map_ready"] = mapReady;
  doc["map_dragging"] = mapDragging;
  doc["map_tiles_loaded"] = lastMapTilesLoaded;
  doc["map_tiles_requested"] = lastMapTilesRequested;
  doc["map_tile_http_code"] = lastMapTileHttpCode;
  doc["detail_status"] = lastDetailStatus;
  doc["photo_http_code"] = lastPhotoHttpCode;
  doc["photo_status"] = lastPhotoStatus;
  doc["gemini_key_configured"] = storedGeminiKey[0] != '\0';
  doc["gemini_status"] = geminiStatus;
  doc["gemini_http_code"] = lastGeminiHttpCode;
  doc["gemini_tls_ok"] = geminiTlsOk;
  doc["setup_portal_active"] = setupPortalActive;
  doc["setup_ap_started"] = setupApStarted;
  doc["setup_ap_clients"] = setupApStarted ? WiFi.softAPgetStationNum() : 0;
  if (selectedPlane >= 0 && trafficLive && selectedPlane < livePlaneCount) {
    const LivePlane& p = livePlanes[selectedPlane];
    doc["selected"]["hex"] = p.hex;
    doc["selected"]["callsign"] = p.callsign;
    doc["selected"]["type"] = p.type;
    doc["selected"]["registration"] = p.registration;
    doc["selected"]["route"] = p.route;
    doc["selected"]["photo"] = p.photo;
    doc["selected"]["photo_visible"] = selectedPhotoHex[0] && !strcmp(selectedPhotoHex, p.hex) &&
                                           !strcmp(p.photo, "PHOTO OK");
    doc["selected"]["enriched"] = p.enriched;
  }
  String body;
  serializeJson(doc, body);
  statusServer.send(200, "application/json", body);
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
  setupApStarted = WiFi.softAP("FlightDesk-Setup", setupApPassword);
  setupServer.on("/", HTTP_GET, handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.on("/ai", HTTP_GET, handleGeminiRoot);
  setupServer.on("/ai/save", HTTP_POST, handleGeminiSave);
  setupServer.on("/ai/test", HTTP_POST, handleGeminiTest);
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
  ++mapGeneration;
  embeddedMapOffsetX = 0;
  embeddedMapOffsetY = 0;
  mapReady = false;
  mapDirty = false;
  mapReloadAfterMs = 0;
  lastMapFetchMs = 0;
  lastTrafficFetchMs = 0;
  trafficLive = false;
  trafficFetchFailed = false;
  livePlaneCount = 0;
  selectedPlane = -1;
  radarBaseDirty = true;
}

void setRadarRange(int rangeKm) {
  if (rangeKm != 20 && rangeKm != 50 && rangeKm != 100 && rangeKm != 250) return;
  if (radarRangeKm == rangeKm) return;
  radarRangeKm = rangeKm;
  if (prefs.begin("flightdesk", false)) {
    prefs.putInt("range", radarRangeKm);
    prefs.end();
  }
  invalidateLiveData();
  Serial.printf("[RADAR] Rayon %d km\n", radarRangeKm);
}

void recenterMap() {
  copyClean(currentPostal, sizeof(currentPostal), homePostal, "1188");
  radarLat = homeLat;
  radarLon = homeLon;
  saveLocationSettings();
  invalidateLiveData();
}

bool resolvePostalCode(const char* code, double& lat, double& lon) {
  for (const PostalPreset& preset : kPostalPresets) {
    if (strcmp(code, preset.code) == 0) {
      lat = preset.lat;
      lon = preset.lon;
      return true;
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    copyClean(postalError, sizeof(postalError), "WIFI REQUIS", "");
    return false;
  }

  char url[200];
  snprintf(url, sizeof(url),
           "https://api3.geo.admin.ch/rest/services/api/SearchServer?searchText=%s&type=locations&origins=zipcode&sr=4326&limit=10",
           code);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("FlightDesk/0.2 (https://github.com/jeanne0r/FlightDesk)");
  http.setTimeout(9000);
  if (!http.begin(client, url)) {
    copyClean(postalError, sizeof(postalError), "RESEAU INDISPONIBLE", "");
    return false;
  }
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    copyClean(postalError, sizeof(postalError), "RESEAU INDISPONIBLE", "");
    return false;
  }
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) {
    copyClean(postalError, sizeof(postalError), "REPONSE INVALIDE", "");
    return false;
  }
  for (JsonObject result : doc["results"].as<JsonArray>()) {
    JsonObject attrs = result["attrs"].as<JsonObject>();
    if (strcmp(attrs["detail"] | "", code) != 0) continue;
    if (attrs["lat"].isNull() || attrs["lon"].isNull()) continue;
    lat = attrs["lat"].as<double>();
    lon = attrs["lon"].as<double>();
    return true;
  }
  copyClean(postalError, sizeof(postalError), "NPA INTROUVABLE", "");
  return false;
}

void applyPostalCode(const char* code, double lat, double lon) {
  copyClean(homePostal, sizeof(homePostal), code, "1188");
  homeLat = lat;
  homeLon = lon;
  copyClean(currentPostal, sizeof(currentPostal), code, "1188");
  radarLat = lat;
  radarLon = lon;
  invalidateLiveData();
  saveLocationSettings();
  Serial.printf("[LOC] NPA %s %.5f,%.5f\n", currentPostal, radarLat, radarLon);
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
  const int zoom = zoomForRange(radarLat, radarRangeKm, kRadarRadius);
  const double scale = 256.0 * static_cast<double>(1UL << zoom);
  const double centerX = lonToPixelX(radarLon, zoom) - dx;
  const double centerY = latToPixelY(radarLat, zoom) - dy;
  radarLat = pixelYToLat(centerY, zoom);
  radarLon = centerX / scale * 360.0 - 180.0;
  radarLat = constrain(radarLat, -80.0, 80.0);
  if (radarLon > 180.0) radarLon -= 360.0;
  if (radarLon < -180.0) radarLon += 360.0;
  copyClean(currentPostal, sizeof(currentPostal), "MANUEL", "MANUEL");
  if (mapReady) {
    shiftMapFrame(dx, dy);
  } else {
    embeddedMapOffsetX += dx;
    embeddedMapOffsetY += dy;
  }
  ++mapGeneration;
  mapDirty = true;
  radarBaseDirty = true;
  mapReloadAfterMs = millis() + 900;
  selectedPlane = -1;
}

int nearestPlaneAt(uint16_t x, uint16_t y) {
  int best = -1;
  int bestD2 = 34 * 34;
  for (int i = 0; i < aircraftCount(); ++i) {
    if (!planeVisible(i)) continue;
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

void handlePostalKeyboardTap(uint16_t x, uint16_t y) {
  if (inRect(x, y, 356, 72, 48, 50)) {
    postalKeyboardOpen = false;
    return;
  }
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col) {
      if (!inRect(x, y, 100 + col * 94, 174 + row * 49, 86, 42)) continue;
      const size_t length = strlen(postalInput);
      postalError[0] = '\0';
      if (row == 3 && col == 0) {
        if (length) postalInput[length - 1] = '\0';
      } else if (row == 3 && col == 2) {
        if (length != 4) {
          copyClean(postalError, sizeof(postalError), "4 CHIFFRES REQUIS", "");
          return;
        }
        copyClean(postalError, sizeof(postalError), "RECHERCHE...", "");
        drawRadarFrame();
        double lat = 0.0;
        double lon = 0.0;
        if (resolvePostalCode(postalInput, lat, lon)) {
          applyPostalCode(postalInput, lat, lon);
          postalKeyboardOpen = false;
          postalError[0] = '\0';
        }
      } else if (length < 4) {
        const char digit = row == 3 ? '0' : static_cast<char>('1' + row * 3 + col);
        postalInput[length] = digit;
        postalInput[length + 1] = '\0';
      }
      return;
    }
  }
}

void handleTap(uint16_t x, uint16_t y) {
  lastTouchX = x;
  lastTouchY = y;
  touchMarkerUntilMs = millis() + 900;
  Serial.printf("[TOUCH] tap x=%u y=%u menu=%d selected=%d\n", x, y, menuOpen, selectedPlane);

  if (appView != AppView::Radar) {
    if (appView == AppView::Assistant) {
      if (inRect(x, y, 94, 298, 92, 54)) { startWifiPortal(); return; }
      if (inRect(x, y, 192, 298, 92, 54)) { requestGeminiTest(); return; }
      if (inRect(x, y, 290, 298, 92, 54)) { appView = AppView::Radar; return; }
      return;
    }
    if (appView == AppView::Settings) {
      if (postalKeyboardOpen) {
        handlePostalKeyboardTap(x, y);
        return;
      }
      if (inRect(x, y, 150, 153, 180, 48)) {
        postalInput[0] = '\0';
        postalError[0] = '\0';
        postalKeyboardOpen = true;
        return;
      }
      if (inRect(x, y, 100, 210, 280, 44)) {
        constexpr int ranges[] = {20, 50, 100, 250};
        const int index = (x - 100) / 72;
        if (index < 4) setRadarRange(ranges[index]);
        return;
      }
      if (inRect(x, y, 96, 290, 138, 44)) { startWifiPortal(); return; }
      if (inRect(x, y, 246, 290, 128, 44)) {
        appView = AppView::Radar;
        return;
      }
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
      selectedHex[0] = '\0';
      return;
    }
    if (!inRect(x, y, 52, 124, 376, 214)) {
      selectedPlane = -1;
      selectedHex[0] = '\0';
    }
    return;
  }

  if (inRect(x, y, 93, 344, 314, 58)) {
    if (x < 191) {
      appView = AppView::Radar;
      recenterMap();
    } else if (x < 283) {
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
    copyClean(selectedHex, sizeof(selectedHex), trafficLive ? livePlanes[plane].hex : "", "");
    if (strcmp(selectedPhotoHex, selectedHex) != 0) clearSelectedPhoto();
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
      lastMapFetchMs = 0;
      lastTrafficFetchMs = 0;
      updateLivePlanePositions();
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

void geminiTestTask(void*) {
  for (;;) {
    if (geminiTestRequested && !geminiTestInProgress) {
      geminiTestRequested = false;
      geminiTestInProgress = true;
      testGeminiConnection();
      geminiTestInProgress = false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void startGeminiTask() {
  if (geminiTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(geminiTestTask, "flightdesk-gemini", 12288, nullptr,
                          1, &geminiTaskHandle, 0);
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
  mapFetchRequested = true;
}

void requestPlaneEnrich(int index) {
  if (!trafficLive || index < 0 || index >= livePlaneCount) return;
  if (livePlanes[index].enriched) {
    const bool photoMissing = !strcmp(livePlanes[index].photo, "PHOTO KO") ||
                              (!strcmp(livePlanes[index].photo, "PHOTO OK") &&
                               strcmp(selectedPhotoHex, livePlanes[index].hex) != 0);
    if (!photoMissing) return;
    livePlanes[index].enriched = false;
  }
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
  livePlanes = static_cast<LivePlane*>(heap_caps_calloc(kMaxLivePlanes, sizeof(LivePlane),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  fetchedPlanes = static_cast<LivePlane*>(heap_caps_calloc(kMaxLivePlanes, sizeof(LivePlane),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  trafficPayloadBuffer = static_cast<char*>(heap_caps_malloc(kTrafficPayloadCapacity + 1,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!livePlanes || !fetchedPlanes) {
    Serial.println("[TRAFFIC] PSRAM plane buffers KO");
  }
  if (!trafficPayloadBuffer) Serial.println("[TRAFFIC] PSRAM payload buffer KO");
  tileCacheReady = LittleFS.begin(true);
  initTouch();
  initImu();
  loadWifiSettings();
  loadLocationSettings();
  connectWifi();
  statusServer.on("/status", HTTP_GET, handleStatus);
  statusServer.begin();
  initOta();
  startTrafficTask();
  startMapTask();
  startEnrichTask();
  startGeminiTask();
  requestGeminiTest();
  bootMs = millis();
  printStatus();
}

void loop() {
  const uint32_t now = millis();
  statusServer.handleClient();

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

  if ((!mapReady || mapDirty) && !mapDragging && WiFi.status() == WL_CONNECTED && now - bootMs >= 6000 &&
      (mapReloadAfterMs == 0 || now >= mapReloadAfterMs) &&
      (lastMapFetchMs == 0 || mapDirty || now - lastMapFetchMs >= 60000)) {
    lastMapFetchMs = now;
    requestMapFetch();
  }

  if (WiFi.status() == WL_CONNECTED && now - bootMs >= 3000 &&
      (lastTrafficFetchMs == 0 || now - lastTrafficFetchMs >= (trafficFetchFailed ? 5000U : 20000U))) {
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

  if (now - lastTrafficAnimationMs >= 80) {
    const uint32_t stepMs = lastTrafficAnimationMs == 0 ? 80 : now - lastTrafficAnimationMs;
    lastTrafficAnimationMs = now;
    animateLivePlanePositions(now, stepMs);
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
