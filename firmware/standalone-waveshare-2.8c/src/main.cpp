#include <Arduino.h>
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
constexpr uint8_t kExioLcdCs = 3;     // Waveshare LCD_CS = EXIO3
constexpr uint8_t kExioBuzzer = 8;    // Waveshare buzzer = EXIO8
constexpr uint8_t kOutputMask =
    static_cast<uint8_t>((1U << (kExioLcdReset - 1)) |
                         (1U << (kExioLcdCs - 1)) |
                         (1U << (kExioBuzzer - 1)));

spi_device_handle_t lcdSpi = nullptr;
esp_lcd_panel_handle_t lcdPanel = nullptr;
uint16_t* frame = nullptr;

uint32_t lastStatusMs = 0;
uint32_t lastI2cScanMs = 0;

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

void initExio() {
  uint8_t output = 0xFF;
  output &= static_cast<uint8_t>(~(1U << (kExioLcdCs - 1)));      // LCD CS active
  output &= static_cast<uint8_t>(~(1U << (kExioLcdReset - 1)));   // LCD reset low
  output &= static_cast<uint8_t>(~(1U << (kExioBuzzer - 1)));     // buzzer off
  pcaWrite(0x01, output);
  pcaWrite(0x03, static_cast<uint8_t>(0xFF & ~kOutputMask));
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

void fillCircle(int cx, int cy, int r, uint16_t color) {
  for (int y = -r; y <= r; ++y) {
    const int span = static_cast<int>(sqrtf(r * r - y * y));
    for (int x = -span; x <= span; ++x) {
      putPixel(cx + x, cy + y, color);
    }
  }
}

void drawRadarFrame() {
  if (!frame || !lcdPanel) return;

  const uint16_t dark = rgb565(0, 8, 10);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    frame[i] = dark;
  }

  constexpr int cx = 240;
  constexpr int cy = 240;
  constexpr int r = 226;
  const uint16_t green = rgb565(95, 255, 105);
  const uint16_t dim = rgb565(20, 90, 35);

  fillCircle(cx, cy, r, rgb565(0, 22, 14));
  drawCircle(cx, cy, r, green);
  drawCircle(cx, cy, r - 5, dim);

  for (int ring = 45; ring <= 180; ring += 45) {
    drawCircle(cx, cy, ring, dim);
  }

  for (int a = 0; a < 360; a += 30) {
    const float rad = a * DEG_TO_RAD;
    drawLine(cx, cy, cx + cosf(rad) * r, cy + sinf(rad) * r, dim);
  }

  drawLine(cx, cy, cx + 190, cy - 70, green);
  drawLine(cx, cy, cx + 160, cy - 40, rgb565(35, 170, 70));
  drawLine(cx, cy, cx + 130, cy - 15, rgb565(25, 110, 50));

  const int planes[][3] = {
      {122, 116, 32}, {342, 132, 120}, {190, 310, 205}, {318, 332, 292},
      {252, 162, 15}, {106, 262, 278}, {388, 236, 86}};
  for (const auto& p : planes) {
    const float ar = p[2] * DEG_TO_RAD;
    const int x = p[0];
    const int y = p[1];
    drawLine(x, y, x + cosf(ar) * 18, y + sinf(ar) * 18, green);
    drawLine(x, y, x + cosf(ar + 2.45f) * 9, y + sinf(ar + 2.45f) * 9, green);
    drawLine(x, y, x + cosf(ar - 2.45f) * 9, y + sinf(ar - 2.45f) * 9, green);
  }

  fillCircle(cx, cy, 9, dark);
  drawCircle(cx, cy, 10, green);

  esp_lcd_panel_draw_bitmap(lcdPanel, 0, 0, kWidth, kHeight, frame);
}

void drawCalibrationFrame() {
  if (!frame || !lcdPanel) return;

  const uint16_t black = rgb565(0, 0, 0);
  const uint16_t green = rgb565(80, 255, 95);
  const uint16_t darkGreen = rgb565(0, 35, 18);
  const uint16_t red = rgb565(180, 0, 0);
  const uint16_t blue = rgb565(0, 40, 160);

  for (int i = 0; i < kWidth * kHeight; ++i) {
    frame[i] = black;
  }

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      if (x < 240 && y < 240) {
        putPixel(x, y, darkGreen);
      } else if (x >= 240 && y < 240) {
        putPixel(x, y, rgb565(0, 18, 8));
      } else if (x < 240) {
        putPixel(x, y, rgb565(0, 10, 20));
      } else {
        putPixel(x, y, rgb565(15, 0, 18));
      }
    }
  }

  drawCircle(240, 240, 226, green);
  drawCircle(240, 240, 180, green);
  drawCircle(240, 240, 120, green);
  drawCircle(240, 240, 60, green);
  drawLine(0, 240, 479, 240, green);
  drawLine(240, 0, 240, 479, green);
  drawLine(20, 20, 459, 459, red);
  drawLine(459, 20, 20, 459, blue);

  for (int y = 388; y <= 438; ++y) {
    for (int x = 150; x <= 330; ++x) {
      putPixel(x, y, rgb565(0, 12, 18));
    }
  }
  for (int x = 150; x <= 330; ++x) {
    putPixel(x, 388, green);
    putPixel(x, 438, green);
  }
  for (int y = 388; y <= 438; ++y) {
    putPixel(150, y, green);
    putPixel(330, y, green);
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

  initBacklight();
  drawCalibrationFrame();
  Serial.println("[DISPLAY] OK calibration frame written once");
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

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2500);

  printHeader();
  Wire.begin(kI2cSda, kI2cScl, 400000);
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

  delay(10);
}
