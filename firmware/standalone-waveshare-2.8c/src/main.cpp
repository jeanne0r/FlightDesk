#include <Arduino.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <WiFi.h>
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
uint8_t* tileBuffer = nullptr;
size_t tileBufferSize = 0;
PNG png;

uint32_t lastStatusMs = 0;
uint32_t lastI2cScanMs = 0;
uint32_t lastRadarMs = 0;
uint32_t lastTouchPollMs = 0;
uint32_t lastImuMs = 0;
uint32_t lastMapFetchMs = 0;
uint32_t touchMarkerUntilMs = 0;
uint32_t lastTouchLogMs = 0;
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
bool mapFetchInProgress = false;

constexpr double kHomeLat = 46.5096;
constexpr double kHomeLon = 6.3077;
constexpr int kRangeKm = 50;
constexpr int kRadarCx = 240;
constexpr int kRadarCy = 240;
constexpr int kRadarRadius = 218;
int pngTileScreenX = 0;
int pngTileScreenY = 0;

struct RadarPlane {
  int x;
  int y;
  int heading;
  const char* callsign;
};

constexpr RadarPlane kPlanes[] = {
    {122, 116, 32, "SWR3ZK"}, {342, 132, 120, "EZS51BG"}, {190, 310, 205, "AFR45RG"},
    {318, 332, 292, "LOT4HT"}, {252, 162, 15, "HBK0J"}, {106, 262, 278, "BAW74"},
    {388, 236, 86, "DLH8PN"}};

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

void putMapPixel(int x, int y, uint16_t color) {
  if (!mapFrame || x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  mapFrame[y * kWidth + x] = color;
}

uint16_t radarTint(uint16_t rgb) {
  const uint8_t r = ((rgb >> 11) & 0x1F) << 3;
  const uint8_t g = ((rgb >> 5) & 0x3F) << 2;
  const uint8_t b = (rgb & 0x1F) << 3;
  const uint8_t lum = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
  return rgb565(1 + lum / 24, 13 + lum / 5, 12 + lum / 7);
}

void clearMapFrame() {
  if (!mapFrame) return;
  for (int i = 0; i < kWidth * kHeight; ++i) {
    mapFrame[i] = rgb565(1, 10, 10);
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
  const int zoom = zoomForRange(kHomeLat, kRangeKm, kRadarRadius);
  const double centerX = lonToPixelX(kHomeLon, zoom);
  const double centerY = latToPixelY(kHomeLat, zoom);
  const double topLeftX = centerX - kRadarCx;
  const double topLeftY = centerY - kRadarCy;
  const int minTileX = floor(topLeftX / 256.0);
  const int minTileY = floor(topLeftY / 256.0);
  const int maxTileX = floor((topLeftX + kWidth) / 256.0);
  const int maxTileY = floor((topLeftY + kHeight) / 256.0);
  int ok = 0;
  int total = 0;
  Serial.printf("[MAP] Fetch OSM z=%d center=%.5f,%.5f\n", zoom, kHomeLat, kHomeLon);
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
  mapFetchInProgress = false;
  Serial.printf("[MAP] Tiles %d/%d ready=%d\n", ok, total, mapReady);
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
              putPixel(x + i * 6 * scale + col * scale + xx, y + row * scale + yy, color);
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
  blendFillCircle(122, 126, 58, forest, 24);
  blendFillCircle(314, 116, 72, forest, 18);
  blendFillCircle(112, 330, 88, forest, 18);
  blendFillCircle(344, 318, 88, forest, 21);

  const int contours[][4] = {
      {18, 124, 116, 100}, {116, 100, 250, 112}, {250, 112, 452, 76},
      {22, 318, 122, 282}, {122, 282, 244, 292}, {244, 292, 458, 342},
      {78, 404, 188, 350}, {188, 350, 342, 386}, {28, 206, 160, 224},
      {160, 224, 242, 184}, {242, 184, 398, 204}, {318, 368, 456, 282},
      {64, 150, 168, 188}, {168, 188, 264, 170}, {264, 170, 406, 150},
      {42, 248, 150, 252}, {150, 252, 250, 230}, {250, 230, 422, 252}};
  for (const auto& l : contours) {
    blendLine(l[0], l[1], l[2], l[3], terrain, 40);
  }

  const int roads[][4] = {
      {24, 286, 174, 246}, {174, 246, 318, 262}, {318, 262, 462, 228},
      {72, 392, 190, 342}, {190, 342, 300, 372}, {300, 372, 434, 338},
      {24, 214, 118, 244}, {118, 244, 226, 224}, {226, 224, 360, 236}};
  for (const auto& l : roads) {
    blendLine(l[0], l[1], l[2], l[3], road, 58);
  }

  const int ridge[][2] = {{28, 330}, {96, 292}, {134, 252}, {174, 222}, {216, 194}, {276, 168}, {354, 142}, {452, 108}};
  drawBlendPolyline(ridge, 8, border, 48);
  drawText(134, 252, "PARC", label, 1);
  drawText(142, 266, "JURA", label, 1);
  drawText(174, 226, "GIMEL", label, 1);
  drawText(306, 312, "GLAND", label, 1);
}

void drawMenuPanel(uint16_t green, uint16_t text, uint16_t panel) {
  fillRoundRect(108, 106, 264, 244, 22, panel);
  drawRoundRect(108, 106, 264, 244, 22, rgb565(68, 190, 92));
  drawTextCentered(240, 134, "MENU", green, 3);
  fillRoundRect(144, 174, 192, 42, 14, rgb565(2, 20, 15));
  drawRoundRect(144, 174, 192, 42, 14, rgb565(70, 220, 104));
  drawTextCentered(240, 187, "RADAR", text, 2);
  fillRoundRect(144, 228, 192, 42, 14, rgb565(2, 20, 15));
  drawRoundRect(144, 228, 192, 42, 14, rgb565(70, 220, 104));
  drawTextCentered(240, 241, "REGLAGES", text, 2);
  fillRoundRect(144, 282, 192, 42, 14, rgb565(2, 20, 15));
  drawRoundRect(144, 282, 192, 42, 14, rgb565(70, 220, 104));
  drawTextCentered(240, 295, "FERMER", text, 2);
}

void drawAircraftPopup(uint16_t green, uint16_t text, uint16_t panel) {
  if (selectedPlane < 0 || selectedPlane >= static_cast<int>(sizeof(kPlanes) / sizeof(kPlanes[0]))) return;
  const RadarPlane& plane = kPlanes[selectedPlane];
  fillRoundRect(78, 132, 324, 196, 18, panel);
  drawRoundRect(78, 132, 324, 196, 18, rgb565(76, 210, 102));
  drawText(108, 158, "AVION SELECTIONNE", green, 2);
  drawText(108, 194, plane.callsign, green, 4);
  drawText(108, 240, "AIRBUS A320", text, 2);
  drawText(108, 274, "24KM  760KMH", text, 2);
  drawText(108, 298, "3200M  218DEG", text, 2);
  fillRoundRect(332, 150, 44, 44, 15, rgb565(2, 24, 16));
  drawRoundRect(332, 150, 44, 44, 15, green);
  drawTextCentered(354, 162, "X", text, 3);
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
    const bool active = i == 0;
    fillRoundRect(x, y, width, height, 12, active ? rgb565(4, 28, 18) : panel);
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

void drawRadarFrame() {
  if (!frame || !lcdPanel) return;

  constexpr int cx = kRadarCx;
  constexpr int cy = kRadarCy;
  constexpr int r = kRadarRadius;
  const uint16_t black = rgb565(0, 2, 3);
  const uint16_t green = rgb565(132, 255, 116);
  const uint16_t softGreen = rgb565(76, 214, 114);
  const uint16_t glow = rgb565(42, 150, 62);
  const uint16_t dim = rgb565(5, 38, 30);
  const uint16_t map = rgb565(12, 66, 42);
  const uint16_t panel = rgb565(1, 11, 10);
  const uint16_t text = rgb565(225, 244, 228);

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int dx = x - cx;
      const int dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > r * r) {
        frame[y * kWidth + x] = black;
      } else {
        const uint8_t shade = d2 < 120 * 120 ? 18 : (d2 < 190 * 190 ? 13 : 8);
        const uint8_t blue = d2 < 160 * 160 ? 18 : 13;
        frame[y * kWidth + x] = rgb565(1, shade, blue);
      }
    }
  }

  fillCircle(cx, cy, r - 8, rgb565(1, 11, 12));
  blendFillCircle(cx, cy, 194, rgb565(6, 43, 28), 50);
  blendFillCircle(cx, cy, 118, rgb565(8, 56, 34), 38);
  if (mapReady && mapFrame) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= (r - 4) * (r - 4)) {
          const int index = y * kWidth + x;
          frame[index] = blend565(frame[index], mapFrame[index], 220);
        }
      }
    }
  } else {
    drawMapWatermark(map, rgb565(6, 54, 32), rgb565(28, 112, 66), rgb565(34, 132, 78), rgb565(66, 138, 74));
  }

  drawCircle(cx, cy, r, green);
  drawCircle(cx, cy, r - 1, softGreen);
  drawCircle(cx, cy, r - 5, dim);
  drawCircle(cx, cy, r - 12, rgb565(1, 18, 16));

  for (int ring = r / 4; ring <= r; ring += r / 4) {
    drawCircle(cx, cy, ring, dim);
  }

  for (int a = 0; a < 360; a += 30) {
    const float rad = a * DEG_TO_RAD;
    blendLine(cx + cosf(rad) * 16, cy + sinf(rad) * 16,
              cx + cosf(rad) * r, cy + sinf(rad) * r, softGreen, 42);
  }

  fillWedge(cx, cy, r - 12, sweepDeg - 45.0f, sweepDeg - 30.0f, rgb565(2, 30, 29));
  fillWedge(cx, cy, r - 12, sweepDeg - 30.0f, sweepDeg - 15.0f, rgb565(4, 52, 45));
  fillWedge(cx, cy, r - 12, sweepDeg - 15.0f, sweepDeg, rgb565(10, 82, 62));
  const float sweepRad = sweepDeg * DEG_TO_RAD;
  blendThickLine(cx, cy,
                 cx + static_cast<int>(cosf(sweepRad) * (r - 8)),
                 cy + static_cast<int>(sinf(sweepRad) * (r - 8)),
                 green, 205);

  int planeIndex = 0;
  for (const auto& p : kPlanes) {
    const float ar = p.heading * DEG_TO_RAD;
    const int x = p.x;
    const int y = p.y;
    const int noseX = x + static_cast<int>(cosf(ar) * 17);
    const int noseY = y + static_cast<int>(sinf(ar) * 17);
    const int leftX = x + static_cast<int>(cosf(ar + 2.55f) * 10);
    const int leftY = y + static_cast<int>(sinf(ar + 2.55f) * 10);
    const int rightX = x + static_cast<int>(cosf(ar - 2.55f) * 10);
    const int rightY = y + static_cast<int>(sinf(ar - 2.55f) * 10);
    if (planeIndex == selectedPlane) {
      drawCircle(x, y, 24, green);
      blendThickLine(cx, cy, x, y, glow, 140);
    }
    blendFillCircle(x, y, planeIndex == selectedPlane ? 20 : 13, green, planeIndex == selectedPlane ? 28 : 12);
    fillTriangle(noseX + 1, noseY + 1, leftX + 1, leftY + 1, rightX + 1, rightY + 1, rgb565(0, 8, 6));
    fillTriangle(noseX, noseY, leftX, leftY, rightX, rightY, rgb565(205, 255, 202));
    drawLine(noseX, noseY, leftX, leftY, green);
    drawLine(noseX, noseY, rightX, rightY, green);
    ++planeIndex;
  }

  drawTextCentered(cx, 48, "18:47", text, 1);
  drawTextCentered(cx, 67, "LIVE OPENSKY", softGreen, 1);
  drawTextCentered(cx, 88, "7", green, 3);
  drawTextCentered(cx, 120, "AVIONS", green, 2);
  drawText(cx + r * 41 / 100, cy + 3, "20", softGreen, 1);
  drawText(cx + r * 72 / 100, cy + 3, "50", softGreen, 1);
  drawText(cx + r * 73 / 100, cy + 20, "KM", softGreen, 1);

  fillCircle(cx, cy, 9, black);
  drawCircle(cx, cy, 10, green);
  drawCircle(cx, cy, 16, dim);

  if (menuOpen) {
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
  tileBufferSize = 220 * 1024;
  tileBuffer = static_cast<uint8_t*>(heap_caps_malloc(tileBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!tileBuffer) {
    Serial.println("[DISPLAY] Tile buffer PSRAM KO");
    return false;
  }
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

void connectWifi() {
  if (strlen(FLIGHTDESK_WIFI_SSID) == 0) {
    Serial.println("[WIFI] Aucun SSID configure dans include/secrets.h");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(FLIGHTDESK_WIFI_SSID, FLIGHTDESK_WIFI_PASSWORD);

  Serial.printf("[WIFI] Connexion a %s", FLIGHTDESK_WIFI_SSID);
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
  }
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

int nearestPlaneAt(uint16_t x, uint16_t y) {
  int best = -1;
  int bestD2 = 34 * 34;
  for (int i = 0; i < static_cast<int>(sizeof(kPlanes) / sizeof(kPlanes[0])); ++i) {
    const int dx = static_cast<int>(x) - kPlanes[i].x;
    const int dy = static_cast<int>(y) - kPlanes[i].y;
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

  if (menuOpen) {
    if (inRect(x, y, 154, 178, 172, 42) || inRect(x, y, 154, 290, 172, 42)) {
      menuOpen = false;
      selectedPlane = -1;
    }
    return;
  }

  if (selectedPlane >= 0) {
    if (inRect(x, y, 308, 132, 84, 84) || inRect(x, y, 328, 152, 42, 42)) {
      selectedPlane = -1;
      return;
    }
    if (!inRect(x, y, 84, 136, 312, 188)) {
      selectedPlane = -1;
    }
    return;
  }

  if (inRect(x, y, 263, 344, 72, 56)) {
    menuOpen = true;
    return;
  }
  if (inRect(x, y, 93, 344, 72, 56) || inRect(x, y, 149, 344, 72, 56) ||
      inRect(x, y, 205, 344, 72, 56) || inRect(x, y, 319, 344, 72, 56)) {
    menuOpen = false;
    selectedPlane = -1;
    return;
  }

  const int plane = nearestPlaneAt(x, y);
  if (plane >= 0) {
    selectedPlane = plane;
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
    handleTap(x, y);
  }
  touchWasDown = down;
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
  connectWifi();
  fetchMapTiles();
  printStatus();
}

void loop() {
  const uint32_t now = millis();

  if (now - lastStatusMs >= 5000) {
    lastStatusMs = now;
    printStatus();
  }

  if (!mapReady && WiFi.status() == WL_CONNECTED && now - lastMapFetchMs >= 60000) {
    lastMapFetchMs = now;
    fetchMapTiles();
    drawRadarFrame();
  }

  if (now - lastI2cScanMs >= 30000) {
    lastI2cScanMs = now;
    scanI2c();
  }

  if (now - lastTouchPollMs >= 30) {
    lastTouchPollMs = now;
    pollTouch();
  }

  if (now - lastImuMs >= 200) {
    lastImuMs = now;
    pollImu();
  }

  if (now - lastRadarMs >= 300) {
    lastRadarMs = now;
    sweepDeg += 4.0f;
    if (sweepDeg >= 360.0f) {
      sweepDeg -= 360.0f;
    }
    drawRadarFrame();
  }

  delay(10);
}
