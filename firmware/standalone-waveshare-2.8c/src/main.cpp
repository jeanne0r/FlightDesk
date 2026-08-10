#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Wire.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

namespace {

constexpr const char* kBuildName = "FlightDesk Waveshare 2.8C bring-up";
constexpr int kI2cScl = 7;
constexpr int kI2cSda = 15;
constexpr uint32_t kI2cClockHz = 400000;
constexpr int kBacklightPin = 6;
constexpr uint8_t kPca9554Address = 0x20;
constexpr uint8_t kPcaLcdResetBit = 0;
constexpr uint8_t kPcaTouchResetBit = 1;
constexpr uint8_t kPcaLcdCsBit = 2;

uint32_t lastStatusMs = 0;
uint32_t lastI2cScanMs = 0;
uint32_t lastRadarMs = 0;
float sweepDeg = 0;

Arduino_DataBus* bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */,
    GFX_NOT_DEFINED /* CS held by PCA9554 */,
    2 /* SCK */,
    1 /* MOSI */,
    GFX_NOT_DEFINED /* MISO */);

Arduino_ESP32RGBPanel* rgbpanel = new Arduino_ESP32RGBPanel(
    40 /* DE */, 39 /* VSYNC */, 38 /* HSYNC */, 41 /* PCLK */,
    46 /* R0 */, 3 /* R1 */, 8 /* R2 */, 18 /* R3 */, 17 /* R4 */,
    14 /* G0 */, 13 /* G1 */, 12 /* G2 */, 11 /* G3 */, 10 /* G4 */, 9 /* G5 */,
    5 /* B0 */, 45 /* B1 */, 48 /* B2 */, 47 /* B3 */, 21 /* B4 */,
    0 /* hsync polarity */, 50 /* hsync front */, 8 /* hsync pulse */, 10 /* hsync back */,
    0 /* vsync polarity */, 8 /* vsync front */, 3 /* vsync pulse */, 8 /* vsync back */,
    0 /* pclk active neg */, 16000000 /* prefer speed */);

Arduino_RGB_Display* gfx = new Arduino_RGB_Display(
    480 /* width */, 480 /* height */, rgbpanel, 0 /* rotation */, false /* auto flush */,
    bus, GFX_NOT_DEFINED /* reset held by PCA9554 */,
    st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

void printHeader() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(kBuildName);
  Serial.println("Target: Waveshare ESP32-S3-Touch-LCD-2.8C, SKU 29086");
  Serial.println("Display: 2.8 inch round capacitive touch LCD, 480x480");
  Serial.println("Bring-up: USB serial + Wi-Fi + official I2C bus scan");
  Serial.println("========================================");
}

void pca9554Write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kPca9554Address);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void setPcaBit(uint8_t& state, uint8_t bit, bool high) {
  if (high) {
    state |= static_cast<uint8_t>(1U << bit);
  } else {
    state &= static_cast<uint8_t>(~(1U << bit));
  }
  pca9554Write(0x01, state);
}

void initPanelControlLines() {
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, LOW);

  uint8_t outputState = 0xFF;
  pca9554Write(0x03, 0x00);  // all PCA9554 pins as outputs
  pca9554Write(0x01, outputState);
  delay(10);

  setPcaBit(outputState, kPcaLcdCsBit, false);     // keep ST7701 command CS active
  setPcaBit(outputState, kPcaTouchResetBit, true); // release touch reset
  setPcaBit(outputState, kPcaLcdResetBit, false);
  delay(80);
  setPcaBit(outputState, kPcaLcdResetBit, true);
  delay(160);

  digitalWrite(kBacklightPin, HIGH);
}

uint16_t green565(uint8_t level) {
  const uint8_t g = level >> 2;
  return static_cast<uint16_t>(g << 5);
}

void drawRadarFrame() {
  constexpr int cx = 240;
  constexpr int cy = 240;
  constexpr int r = 222;

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(cx, cy, r, 0x0105);
  gfx->drawCircle(cx, cy, r, green565(220));
  gfx->drawCircle(cx, cy, r - 5, green565(130));

  for (int ring = 48; ring <= 192; ring += 48) {
    gfx->drawCircle(cx, cy, ring, green565(70));
  }

  for (int a = 0; a < 360; a += 30) {
    const float rad = a * DEG_TO_RAD;
    const int x = cx + static_cast<int>(cosf(rad) * r);
    const int y = cy + static_cast<int>(sinf(rad) * r);
    gfx->drawLine(cx, cy, x, y, green565(45));
  }

  const float sweep = sweepDeg * DEG_TO_RAD;
  const int sx = cx + static_cast<int>(cosf(sweep) * (r - 16));
  const int sy = cy + static_cast<int>(sinf(sweep) * (r - 16));
  gfx->drawLine(cx, cy, sx, sy, green565(255));
  for (int i = 1; i < 22; ++i) {
    const float trail = (sweepDeg - i * 2.0f) * DEG_TO_RAD;
    const int tx = cx + static_cast<int>(cosf(trail) * (r - 22));
    const int ty = cy + static_cast<int>(sinf(trail) * (r - 22));
    gfx->drawLine(cx, cy, tx, ty, green565(110 - i * 3));
  }

  gfx->fillCircle(cx, cy, 10, RGB565_BLACK);
  gfx->drawCircle(cx, cy, 10, green565(220));

  const int planes[][3] = {
      {128, 114, 32}, {333, 126, 118}, {185, 300, 204}, {320, 332, 292},
      {250, 162, 15}, {105, 262, 278}, {387, 236, 86}};
  for (const auto& p : planes) {
    const float ar = p[2] * DEG_TO_RAD;
    const int x = p[0];
    const int y = p[1];
    const int x1 = x + static_cast<int>(cosf(ar) * 15);
    const int y1 = y + static_cast<int>(sinf(ar) * 15);
    const int lx = x + static_cast<int>(cosf(ar + 2.45f) * 8);
    const int ly = y + static_cast<int>(sinf(ar + 2.45f) * 8);
    const int rx = x + static_cast<int>(cosf(ar - 2.45f) * 8);
    const int ry = y + static_cast<int>(sinf(ar - 2.45f) * 8);
    gfx->fillTriangle(x1, y1, lx, ly, rx, ry, green565(230));
    gfx->drawTriangle(x1, y1, lx, ly, rx, ry, RGB565_WHITE);
  }

  gfx->setTextColor(green565(245), RGB565_BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(96, 68);
  gfx->print("FLIGHTDESK");
  gfx->setTextSize(2);
  gfx->setCursor(127, 104);
  gfx->print("WAVESHARE 2.8C");
  gfx->setCursor(188, 400);
  gfx->print("MENU");
  gfx->drawRoundRect(154, 382, 172, 52, 18, green565(240));

  gfx->flush();
}

void initDisplay() {
  initPanelControlLines();
  Serial.println("[DISPLAY] Init ST7701S RGB 480x480");
  if (!gfx->begin()) {
    Serial.println("[DISPLAY] KO framebuffer/init");
    return;
  }
  drawRadarFrame();
  Serial.println("[DISPLAY] OK radar test visible");
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
    case 0x38:
      return "touch CST/FT-series possible";
    case 0x51:
      return "RTC PCF85063 possible";
    case 0x6A:
    case 0x6B:
      return "IMU QMI8658 possible";
    case 0x14:
    case 0x5D:
      return "touch controller possible";
    case 0x20:
    case 0x21:
      return "GPIO expander possible";
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
      if (strlen(label) > 0) {
        Serial.printf("  - 0x%02X  %s\n", address, label);
      } else {
        Serial.printf("  - 0x%02X\n", address);
      }
      ++count;
    }
  }

  if (count == 0) {
    Serial.println("  Aucun peripherique detecte");
  }
}

void printStatus() {
  Serial.printf("[SYS] uptime=%lus heap=%u psram=%u wifi=%s",
                millis() / 1000,
                ESP.getFreeHeap(),
                ESP.getFreePsram(),
                WiFi.status() == WL_CONNECTED ? "ok" : "ko");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" ip=%s rssi=%d",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  }
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(800);

  printHeader();
  Wire.begin(kI2cSda, kI2cScl, kI2cClockHz);
  scanI2c();
  initDisplay();
  connectWifi();
  printStatus();
}

void loop() {
  const uint32_t now = millis();

  if (now - lastStatusMs >= 5000) {
    lastStatusMs = now;
    printStatus();
  }

  if (now - lastI2cScanMs >= 30000) {
    lastI2cScanMs = now;
    scanI2c();
  }

  if (now - lastRadarMs >= 80) {
    lastRadarMs = now;
    sweepDeg += 2.0f;
    if (sweepDeg >= 360.0f) {
      sweepDeg -= 360.0f;
    }
    drawRadarFrame();
  }

  delay(10);
}
