#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define FLIGHTDESK_WIFI_SSID ""
#define FLIGHTDESK_WIFI_PASSWORD ""
#define FLIGHTDESK_GEMINI_API_KEY ""
#endif

namespace {

constexpr int SCREEN = 240;
constexpr int CX = 120;
constexpr int CY = 120;
constexpr int RADAR_R = 112;
constexpr int PHOTO_W = 66;
constexpr int PHOTO_H = 44;
constexpr bool PHOTO_DOWNLOAD_ENABLED = true;

constexpr int PIN_I2C_SDA_TOUCH = 11;
constexpr int PIN_I2C_SCL_TOUCH = 7;
constexpr int PIN_TOUCH_RST = 6;
constexpr int PIN_TOUCH_INT = 12;
constexpr int CST816_ADDR = 0x15;

constexpr int PIN_LCD_CS = 5;
constexpr int PIN_LCD_DC = 47;
constexpr int PIN_LCD_RST = 38;
constexpr int PIN_LCD_SCK = 4;
constexpr int PIN_LCD_MOSI = 2;
constexpr int PIN_LCD_BL = 42;

constexpr uint16_t COL_BG = 0x0000;
constexpr uint16_t COL_PANEL = 0x0204;
constexpr uint16_t COL_GREEN = 0x7FEF;
constexpr uint16_t COL_DIM = 0x2D86;
constexpr uint16_t COL_TEXT = 0xE7FF;
constexpr uint16_t COL_GLOW = 0x4FE8;
constexpr uint32_t TRAFFIC_INTERVAL_MS = 30000;
constexpr uint32_t FRAME_INTERVAL_MS = 50;

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, GFX_NOT_DEFINED, HSPI);
Arduino_GFX *display = new Arduino_GC9A01(bus, PIN_LCD_RST, 0, true);
Arduino_Canvas *canvas = new Arduino_Canvas(SCREEN, SCREEN, display);
Arduino_GFX *gfx = canvas;
JPEGDEC photo_jpeg;

struct Settings {
  float lat = 46.5197f;
  float lon = 6.6323f;
  String postal = "1188";
  String place = "Gimel";
  int range_km = 50;
};

struct Aircraft {
  String hex;
  String callsign;
  String type;
  String registration;
  String operator_name;
  float lat = 0;
  float lon = 0;
  float distance_km = 0;
  float bearing_deg = 0;
  float altitude_m = 0;
  float speed_kmh = 0;
  float heading_deg = 0;
};

Settings settings;
Aircraft aircraft[96];
int aircraft_count = 0;
int selected_index = -1;
String ai_answer = "IA prête.";
String ai_status = "LOCAL";
String screen_mode = "radar";
int postal_page = 0;
float sweep_deg = 0;
uint32_t last_traffic_ms = 0;
uint32_t last_frame_ms = 0;
uint32_t last_touch_ms = 0;
uint16_t photo_pixels[PHOTO_W * PHOTO_H];
String photo_hex;
String photo_status = "PHOTO";
bool photo_ready = false;
bool photo_loading = false;
int photo_offset_x = 0;
int photo_offset_y = 0;
int pending_photo_index = -1;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

float deg2rad(float value) {
  return value * PI / 180.0f;
}

float haversine(float lat1, float lon1, float lat2, float lon2) {
  const float earth = 6371.0f;
  float p1 = deg2rad(lat1), p2 = deg2rad(lat2);
  float dp = deg2rad(lat2 - lat1), dl = deg2rad(lon2 - lon1);
  float a = sinf(dp / 2) * sinf(dp / 2) + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
  return earth * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
}

float bearing(float lat1, float lon1, float lat2, float lon2) {
  float p1 = deg2rad(lat1), p2 = deg2rad(lat2), dl = deg2rad(lon2 - lon1);
  float y = sinf(dl) * cosf(p2);
  float x = cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl);
  float deg = atan2f(y, x) * 180.0f / PI;
  return fmodf(deg + 360.0f, 360.0f);
}

void polarToScreen(float bearing_deg, float distance_km, int range_km, int &x, int &y) {
  float a = deg2rad(bearing_deg - 90.0f);
  float r = RADAR_R * distance_km / max(1, range_km);
  x = CX + cosf(a) * r;
  y = CY + sinf(a) * r;
}

String cityForPostal(const String &postal) {
  if (postal == "1188") return "Gimel";
  if (postal == "1000") return "Lausanne";
  if (postal == "1200") return "Genève";
  if (postal == "1400") return "Yverdon";
  if (postal == "2000") return "Neuchâtel";
  if (postal == "3000") return "Berne";
  if (postal == "8000") return "Zurich";
  return postal;
}

const char *jsonString(JsonVariantConst value, const char *fallback = "") {
  if (value.is<const char *>()) return value.as<const char *>();
  return fallback;
}

float jsonFloat(JsonVariantConst value, float fallback = NAN) {
  if (value.is<float>() || value.is<int>() || value.is<long>()) return value.as<float>();
  if (value.is<const char *>()) {
    const char *text = value.as<const char *>();
    if (!text || !text[0] || strcmp(text, "ground") == 0) return fallback;
    return atof(text);
  }
  return fallback;
}

String firstText(JsonVariantConst a, JsonVariantConst b, JsonVariantConst c, const char *fallback) {
  const char *av = jsonString(a, "");
  if (av[0]) return String(av);
  const char *bv = jsonString(b, "");
  if (bv[0]) return String(bv);
  const char *cv = jsonString(c, "");
  if (cv[0]) return String(cv);
  return String(fallback);
}

void applyPostal(const String &postal) {
  settings.postal = postal;
  settings.place = cityForPostal(postal);
  if (postal == "1188") { settings.lat = 46.5197f; settings.lon = 6.6323f; }
  else if (postal == "1000") { settings.lat = 46.5197f; settings.lon = 6.6323f; }
  else if (postal == "1200") { settings.lat = 46.2044f; settings.lon = 6.1432f; }
  else if (postal == "1400") { settings.lat = 46.7785f; settings.lon = 6.6412f; }
  else if (postal == "2000") { settings.lat = 46.9918f; settings.lon = 6.9310f; }
  else if (postal == "3000") { settings.lat = 46.9480f; settings.lon = 7.4474f; }
  else if (postal == "8000") { settings.lat = 47.3769f; settings.lon = 8.5417f; }
}

void drawCentered(const String &text, int y, uint16_t color, int size = 1) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  gfx->setCursor((SCREEN - w) / 2, y);
  gfx->print(text);
}

void drawButton(int x, int y, int w, int h, const String &label, bool active = false) {
  gfx->fillRoundRect(x, y, w, h, 10, active ? rgb565(16, 58, 22) : rgb565(3, 12, 6));
  gfx->drawRoundRect(x, y, w, h, 10, active ? COL_GREEN : COL_DIM);
  gfx->setTextSize(1);
  gfx->setTextColor(active ? COL_GREEN : COL_TEXT);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  gfx->print(label);
}

void clearPhotoCache() {
  photo_hex = "";
  photo_status = "PHOTO";
  photo_ready = false;
  photo_loading = false;
  pending_photo_index = -1;
}

int jpegPhotoDraw(JPEGDRAW *pDraw) {
  for (int yy = 0; yy < pDraw->iHeight; ++yy) {
    int dy = pDraw->y + yy + photo_offset_y;
    if (dy < 0 || dy >= PHOTO_H) continue;
    for (int xx = 0; xx < pDraw->iWidthUsed; ++xx) {
      int dx = pDraw->x + xx + photo_offset_x;
      if (dx < 0 || dx >= PHOTO_W) continue;
      photo_pixels[dy * PHOTO_W + dx] = pDraw->pPixels[yy * pDraw->iWidth + xx];
    }
  }
  return 1;
}

String photoUrlFromJson(const String &hex) {
  if (WiFi.status() != WL_CONNECTED || !hex.length()) return "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.planespotters.net/pub/photos/hex/" + hex;
  if (!http.begin(client, url)) return "";
  http.setTimeout(4000);
  http.setUserAgent("FlightDesk/0.1 (+https://github.com/jeanne0r/FlightDesk)");
  int code = http.GET();
  if (code != 200) {
    http.end();
    return "";
  }
  DynamicJsonDocument doc(24576);
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) return "";
  JsonVariantConst photo = doc["photos"][0];
  const char *thumb = photo["thumbnail"]["src"] | "";
  const char *large = photo["thumbnail_large"]["src"] | "";
  String src = thumb[0] ? String(thumb) : String(large);
  if (!src.length()) return "";
  src.replace("https://", "");
  src.replace("http://", "");
  return "https://images.weserv.nl/?url=" + src + "&w=66&h=44&fit=cover&output=jpg&q=70";
}

bool downloadPhotoBytes(const String &url, uint8_t *&buffer, size_t &length) {
  buffer = nullptr;
  length = 0;
  if (!url.length()) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  http.setUserAgent("FlightDesk/0.1 (+https://github.com/jeanne0r/FlightDesk)");
  http.addHeader("Accept", "image/jpeg,image/*");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  int size = http.getSize();
  const size_t max_len = 60000;
  if (size > (int)max_len) {
    http.end();
    return false;
  }
  size_t capacity = size > 0 ? (size_t)size : max_len;
  buffer = (uint8_t *)malloc(capacity);
  if (!buffer) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint32_t start = millis();
  while ((size <= 0 || length < (size_t)size) && length < capacity && millis() - start < 8000) {
    size_t available = stream->available();
    if (available) {
      size_t room = capacity - length;
      size_t wanted = min(available, room);
      int got = stream->readBytes(buffer + length, wanted);
      if (got > 0) {
        length += got;
        start = millis();
      }
    } else if (!stream->connected()) {
      break;
    } else {
      delay(5);
      yield();
    }
  }
  http.end();
  if (length < 256 || buffer[0] != 0xFF || buffer[1] != 0xD8) {
    free(buffer);
    buffer = nullptr;
    return false;
  }
  return true;
}

void loadPhotoForAircraft(const Aircraft &a) {
  if (photo_hex == a.hex) return;
  photo_hex = "";
  photo_ready = false;
  photo_loading = true;
  photo_hex = a.hex;
  if (!PHOTO_DOWNLOAD_ENABLED) {
    photo_status = "PHOTO OFF";
    photo_loading = false;
    return;
  }
  photo_status = "PHOTO...";
  yield();
  String url = photoUrlFromJson(a.hex);
  if (!url.length()) {
    photo_status = "NO PHOTO";
    photo_loading = false;
    return;
  }
  uint8_t *jpeg_bytes = nullptr;
  size_t jpeg_len = 0;
  if (!downloadPhotoBytes(url, jpeg_bytes, jpeg_len)) {
    photo_status = "DL KO";
    photo_loading = false;
    return;
  }

  for (int i = 0; i < PHOTO_W * PHOTO_H; ++i) photo_pixels[i] = rgb565(1, 9, 5);
  bool ok = false;
  if (photo_jpeg.openRAM(jpeg_bytes, jpeg_len, jpegPhotoDraw)) {
    int scale = 0;
    int out_w = photo_jpeg.getWidth();
    int out_h = photo_jpeg.getHeight();
    photo_offset_x = (PHOTO_W - out_w) / 2;
    photo_offset_y = (PHOTO_H - out_h) / 2;
    ok = photo_jpeg.decode(0, 0, scale);
    photo_jpeg.close();
  }
  free(jpeg_bytes);
  photo_ready = ok;
  photo_status = ok ? "" : "PHOTO KO";
  photo_loading = false;
}

void queuePhotoForAircraft(int index) {
  if (index < 0 || index >= aircraft_count) return;
  if (photo_hex == aircraft[index].hex) return;
  pending_photo_index = index;
  photo_hex = aircraft[index].hex;
  photo_status = "PHOTO...";
  photo_ready = false;
  photo_loading = false;
}

void servicePendingPhoto() {
  if (photo_loading || pending_photo_index < 0 || pending_photo_index >= aircraft_count) return;
  int index = pending_photo_index;
  pending_photo_index = -1;
  String hex = aircraft[index].hex;
  photo_hex = "";
  loadPhotoForAircraft(aircraft[index]);
  if (selected_index < 0 || selected_index >= aircraft_count || aircraft[selected_index].hex != hex) {
    clearPhotoCache();
  }
}

void drawMapWatermark() {
  uint16_t contour = rgb565(2, 20, 12);
  uint16_t road = rgb565(4, 32, 18);
  for (int line = -3; line <= 3; ++line) {
    int last_x = -1, last_y = -1;
    for (int t = -RADAR_R; t <= RADAR_R; t += 8) {
      float seed = settings.lat * 0.71f + settings.lon * 1.37f + line * 0.9f;
      int x = CX + t;
      int y = CY + line * 25 + sinf(t * 0.04f + seed) * 11 + cosf(t * 0.018f + seed) * 6;
      if (hypotf(x - CX, y - CY) > RADAR_R - 8) {
        last_x = -1;
        continue;
      }
      if (last_x >= 0) gfx->drawLine(last_x, last_y, x, y, contour);
      last_x = x;
      last_y = y;
    }
  }
  for (int line = -1; line <= 1; ++line) {
    int last_x = -1, last_y = -1;
    for (int t = -RADAR_R; t <= RADAR_R; t += 8) {
      float seed = settings.lon * 0.83f + line * 1.4f;
      int x = CX + t;
      int y = CY + 30 + line * 26 + sinf(t * 0.028f + seed) * 16;
      if (hypotf(x - CX, y - CY) > RADAR_R - 10) {
        last_x = -1;
        continue;
      }
      if (last_x >= 0) gfx->drawLine(last_x, last_y, x, y, road);
      last_x = x;
      last_y = y;
    }
  }
}

void drawRadarBase() {
  gfx->fillScreen(rgb565(0, 2, 2));
  gfx->fillCircle(CX, CY, 119, rgb565(0, 12, 8));
  gfx->fillCircle(CX, CY, RADAR_R, rgb565(1, 12, 10));
  gfx->fillCircle(CX, CY, 88, rgb565(1, 17, 11));
  drawMapWatermark();
  gfx->drawCircle(CX, CY, 116, rgb565(23, 86, 38));
  gfx->drawCircle(CX, CY, RADAR_R, rgb565(56, 158, 70));
  gfx->drawCircle(CX, CY, 84, rgb565(8, 58, 27));
  gfx->drawCircle(CX, CY, 56, rgb565(8, 58, 27));
  gfx->drawCircle(CX, CY, 28, rgb565(8, 58, 27));
  for (int deg = 0; deg < 360; deg += 30) {
    float a = deg2rad(deg);
    gfx->drawLine(CX, CY, CX + cosf(a) * RADAR_R, CY + sinf(a) * RADAR_R, rgb565(5, 40, 23));
  }

  gfx->setTextColor(rgb565(108, 212, 104));
  gfx->setCursor(170, 116);
  gfx->print(settings.range_km / 2);
  gfx->setCursor(203, 116);
  gfx->print(settings.range_km);
  gfx->setCursor(203, 130);
  gfx->print("KM");
}

void drawSweep() {
  float head = deg2rad(sweep_deg - 90.0f);
  for (int i = 6; i >= 1; --i) {
    float a1 = deg2rad(sweep_deg - i * 5.5f - 90.0f);
    float a2 = deg2rad(sweep_deg - (i - 1) * 5.5f - 90.0f);
    uint16_t color = rgb565(0, 18 + i * 5, 12 + i * 3);
    gfx->fillTriangle(CX, CY, CX + cosf(a1) * 102, CY + sinf(a1) * 102, CX + cosf(a2) * 102, CY + sinf(a2) * 102, color);
  }
  gfx->drawLine(CX, CY, CX + cosf(head) * 108, CY + sinf(head) * 108, rgb565(134, 255, 128));
}

void drawAircraftSymbol(const Aircraft &a, bool selected) {
  int x, y;
  polarToScreen(a.bearing_deg, a.distance_km, settings.range_km, x, y);
  float h = deg2rad((a.heading_deg ? a.heading_deg : a.bearing_deg) - 90.0f);
  int scale = selected ? 11 : 9;
  int x1 = x + cosf(h) * scale;
  int y1 = y + sinf(h) * scale;
  int x2 = x + cosf(h + 2.45f) * (scale - 2);
  int y2 = y + sinf(h + 2.45f) * (scale - 2);
  int x3 = x + cosf(h - 2.45f) * (scale - 2);
  int y3 = y + sinf(h - 2.45f) * (scale - 2);
  if (selected) {
    gfx->drawCircle(x, y, 13, rgb565(28, 118, 38));
    gfx->drawLine(CX, CY, x, y, rgb565(58, 190, 76));
  }
  gfx->drawTriangle(x1 - 1, y1, x2 - 1, y2, x3 - 1, y3, rgb565(18, 76, 28));
  gfx->drawTriangle(x1 + 1, y1, x2 + 1, y2, x3 + 1, y3, rgb565(18, 76, 28));
  gfx->fillTriangle(x1, y1, x2, y2, x3, y3, selected ? COL_TEXT : COL_GLOW);
  gfx->drawTriangle(x1, y1, x2, y2, x3, y3, COL_GREEN);
}

void drawPopup() {
  if (selected_index < 0 || selected_index >= aircraft_count) return;
  const Aircraft &a = aircraft[selected_index];
  gfx->fillRoundRect(16, 68, 208, 126, 13, rgb565(1, 10, 6));
  gfx->drawRoundRect(16, 68, 208, 126, 13, COL_GREEN);
  gfx->setTextColor(COL_GREEN);
  gfx->setTextSize(1);
  gfx->setCursor(28, 84);
  gfx->print("AVION SELECTIONNE");
  gfx->setTextSize(a.callsign.length() > 6 ? 2 : 3);
  gfx->setCursor(28, 102);
  gfx->print(a.callsign.substring(0, 8));
  gfx->setTextSize(1);
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(28, 130);
  gfx->print(a.type.length() ? a.type.substring(0, 18) : "TYPE INCONNU");
  gfx->setCursor(28, 146);
  gfx->printf("%dkm  %dkm/h", (int)roundf(a.distance_km), (int)roundf(a.speed_kmh));
  gfx->setCursor(28, 160);
  gfx->printf("%dm  %ddeg", (int)roundf(a.altitude_m), (int)roundf(a.heading_deg));
  gfx->fillRoundRect(144, 108, 66, 44, 8, rgb565(1, 9, 5));
  gfx->drawRoundRect(144, 108, 66, 44, 8, rgb565(24, 96, 36));
  if (photo_ready && photo_hex == a.hex) {
    for (int py = 0; py < PHOTO_H; ++py) {
      for (int px = 0; px < PHOTO_W; ++px) {
        gfx->drawPixel(144 + px, 108 + py, photo_pixels[py * PHOTO_W + px]);
      }
    }
    gfx->drawRoundRect(144, 108, 66, 44, 8, COL_GREEN);
  } else {
    gfx->setTextColor(rgb565(98, 190, 100));
    gfx->setCursor(150, 126);
    gfx->print(photo_status.substring(0, 10));
  }
  drawButton(154, 160, 34, 24, "IA", false);
  drawButton(192, 160, 24, 24, "X", false);
}

void drawBottomControls() {
  drawButton(50, 198, 58, 26, "IA", false);
  drawButton(132, 198, 58, 26, "MENU", false);
}

void drawMenu() {
  gfx->fillRoundRect(22, 50, 196, 142, 18, rgb565(1, 9, 5));
  gfx->drawRoundRect(22, 50, 196, 142, 18, COL_DIM);
  drawCentered("MENU", 67, COL_GREEN, 1);
  drawButton(72, 84, 96, 28, "RADAR", false);
  drawButton(28, 122, 84, 28, "RAYON", false);
  drawButton(128, 122, 84, 28, "NPA", false);
  drawButton(28, 160, 84, 28, "CENTRER", false);
  drawButton(128, 160, 84, 28, "IA", false);
}

void drawPostal() {
  static const char *codes[] = {"1188", "1000", "1200", "1400", "2000", "3000", "8000"};
  gfx->fillRoundRect(18, 42, 204, 156, 18, rgb565(1, 9, 5));
  gfx->drawRoundRect(18, 42, 204, 156, 18, COL_DIM);
  drawCentered("NPA", 58, COL_GREEN, 1);
  gfx->setTextColor(COL_TEXT);
  gfx->setTextSize(1);
  drawCentered(settings.postal + " " + settings.place, 76, COL_TEXT, 1);
  int start = postal_page * 4;
  for (int i = 0; i < 4; ++i) {
    int idx = start + i;
    if (idx >= 7) break;
    int x = (i % 2 == 0) ? 34 : 124;
    int y = (i < 2) ? 98 : 132;
    drawButton(x, y, 76, 26, codes[idx], settings.postal == codes[idx]);
  }
  drawButton(34, 168, 54, 24, postal_page == 0 ? "..." : "PREV", false);
  drawButton(94, 168, 54, 24, postal_page == 0 ? "NEXT" : "...", false);
  drawButton(154, 168, 54, 24, "OK", false);
}

void drawSettings() {
  gfx->fillRoundRect(20, 50, 200, 140, 18, rgb565(1, 9, 5));
  gfx->drawRoundRect(20, 50, 200, 140, 18, COL_DIM);
  drawCentered("RAYON", 66, COL_GREEN, 1);
  drawButton(34, 96, 38, 28, "20", settings.range_km == 20);
  drawButton(78, 96, 38, 28, "50", settings.range_km == 50);
  drawButton(122, 96, 38, 28, "100", settings.range_km == 100);
  drawButton(166, 96, 38, 28, "250", settings.range_km == 250);
  drawButton(70, 150, 100, 30, "RETOUR", false);
}

void drawAI() {
  gfx->fillRoundRect(20, 48, 200, 145, 18, rgb565(1, 9, 5));
  gfx->drawRoundRect(20, 48, 200, 145, 18, COL_GREEN);
  drawCentered("ASSISTANT IA", 64, COL_GREEN, 1);
  drawCentered(ai_status, 82, COL_TEXT, 1);
  gfx->setTextColor(COL_TEXT);
  gfx->setTextSize(1);
  int y = 106;
  String remaining = ai_answer;
  while (remaining.length() && y < 176) {
    int cut = min(28, (int)remaining.length());
    if ((int)remaining.length() > cut) {
      int space = remaining.lastIndexOf(' ', cut);
      if (space > 8) cut = space;
    }
    gfx->setCursor(38, y);
    gfx->print(remaining.substring(0, cut));
    remaining = remaining.substring(cut);
    remaining.trim();
    y += 14;
  }
  drawButton(88, 198, 64, 26, "MENU", false);
}

void render() {
  drawRadarBase();
  for (int i = 0; i < aircraft_count; ++i) {
    if (aircraft[i].distance_km <= settings.range_km) drawAircraftSymbol(aircraft[i], i == selected_index);
  }
  drawSweep();
  gfx->fillCircle(CX, CY, 5, COL_BG);
  gfx->drawCircle(CX, CY, 6, COL_GREEN);
  if (screen_mode == "menu") drawMenu();
  else if (screen_mode == "settings") drawSettings();
  else if (screen_mode == "postal") drawPostal();
  else if (screen_mode == "ai") drawAI();
  else if (selected_index >= 0) drawPopup();
  else drawBottomControls();
  gfx->flush();
}

bool fetchTraffic() {
  if (WiFi.status() != WL_CONNECTED) return false;
  float radius_nm = settings.range_km / 1.852f;
  String url = "https://api.airplanes.live/v2/point/" + String(settings.lat, 5) + "/" + String(settings.lon, 5) + "/" + String(radius_nm, 1);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setUserAgent("FlightDesk/0.1 (+https://github.com/jeanne0r/FlightDesk)");
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  DynamicJsonDocument doc(131072);
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) return false;

  aircraft_count = 0;
  JsonArray ac = doc["ac"].as<JsonArray>();
  for (JsonObject item : ac) {
    if (aircraft_count >= 96) break;
    if (String(jsonString(item["alt_baro"], "")) == "ground") continue;
    float lat = jsonFloat(item["lat"], NAN);
    float lon = jsonFloat(item["lon"], NAN);
    if (isnan(lat) || isnan(lon)) continue;
    float distance = haversine(settings.lat, settings.lon, lat, lon);
    if (distance > settings.range_km) continue;
    Aircraft &a = aircraft[aircraft_count++];
    a.hex = String(item["hex"] | "");
    a.callsign = firstText(item["flight"], item["r"], item["hex"], "LIVE");
    a.callsign.trim();
    a.type = String(jsonString(item["desc"], jsonString(item["t"], "")));
    a.registration = String(jsonString(item["r"], ""));
    a.operator_name = String(jsonString(item["ownOp"], "Airplanes.live"));
    a.lat = lat;
    a.lon = lon;
    a.distance_km = distance;
    a.bearing_deg = bearing(settings.lat, settings.lon, lat, lon);
    float alt_ft = jsonFloat(item["alt_geom"], NAN);
    if (isnan(alt_ft)) alt_ft = jsonFloat(item["alt_baro"], 0.0f);
    float gs_kt = jsonFloat(item["gs"], 0.0f);
    a.altitude_m = alt_ft * 0.3048f;
    a.speed_kmh = gs_kt * 1.852f;
    a.heading_deg = jsonFloat(item["track"], NAN);
    if (isnan(a.heading_deg)) a.heading_deg = jsonFloat(item["true_heading"], NAN);
    if (isnan(a.heading_deg)) a.heading_deg = jsonFloat(item["mag_heading"], a.bearing_deg);
  }
  selected_index = -1;
  clearPhotoCache();
  return true;
}

String localAI(bool selected_only) {
  if (selected_only && selected_index >= 0) {
    const Aircraft &a = aircraft[selected_index];
    return a.callsign + " est a " + String((int)a.distance_km) + " km de " + settings.place +
      ", altitude " + String((int)a.altitude_m) + " m, vitesse " + String((int)a.speed_kmh) +
      " km/h, cap " + String((int)a.heading_deg) + " deg.";
  }
  if (!aircraft_count) return "Aucun avion visible autour de " + settings.place + ".";
  int nearest = 0;
  for (int i = 1; i < aircraft_count; ++i) if (aircraft[i].distance_km < aircraft[nearest].distance_km) nearest = i;
  return String(aircraft_count) + " avion(s) autour de " + settings.place + ". Plus proche: " +
    aircraft[nearest].callsign + " a " + String((int)aircraft[nearest].distance_km) + " km.";
}

String askGemini(bool selected_only) {
  String key = FLIGHTDESK_GEMINI_API_KEY;
  if (!key.length()) {
    ai_status = "IA KO: CLE";
    return "Cle Gemini absente dans cette build.";
  }
  if (WiFi.status() != WL_CONNECTED) {
    ai_status = "IA KO: WIFI";
    return "Wi-Fi non connecte.";
  }
  String context = localAI(selected_only);
  ai_status = "GEMINI...";
  String prompt = "Reponds en francais en maximum 2 phrases pour un petit ecran radar. Donnees: " + context;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(6000);
  String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash-lite:generateContent?key=" + key;
  if (!http.begin(client, url)) {
    ai_status = "IA KO: HTTP";
    return "API Gemini inaccessible.";
  }
  http.addHeader("Content-Type", "application/json");
  DynamicJsonDocument body(4096);
  JsonArray contents = body["contents"].to<JsonArray>();
  JsonObject content = contents.add<JsonObject>();
  JsonArray parts = content["parts"].to<JsonArray>();
  parts.add<JsonObject>()["text"] = prompt;
  body["generationConfig"]["temperature"] = 0.2;
  body["generationConfig"]["maxOutputTokens"] = 80;
  String payload;
  serializeJson(body, payload);
  int code = http.POST(payload);
  if (code != 200) {
    http.end();
    ai_status = code == 400 ? "IA KO: CLE/API" : "IA KO: " + String(code);
    return "Gemini refuse la requete.";
  }
  DynamicJsonDocument answer_doc(8192);
  DeserializationError err = deserializeJson(answer_doc, http.getStream());
  http.end();
  if (err) {
    ai_status = "IA KO: JSON";
    return "Reponse Gemini illisible.";
  }
  const char *text = answer_doc["candidates"][0]["content"]["parts"][0]["text"] | "";
  ai_status = "GEMINI";
  return String(text);
}

bool readTouch(int &x, int &y) {
  if (digitalRead(PIN_TOUCH_INT) == HIGH) return false;
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(CST816_ADDR, 5) != 5) return false;
  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if ((fingers & 0x0F) == 0) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return x >= 0 && x < SCREEN && y >= 0 && y < SCREEN;
}

void handleTouch(int x, int y) {
  if (selected_index >= 0) {
    if (x >= 190 && x <= 224 && y >= 154 && y <= 194) {
      selected_index = -1;
      return;
    }
    if (x >= 148 && x <= 190 && y >= 154 && y <= 194) {
      ai_answer = askGemini(true);
      screen_mode = "ai";
      return;
    }
  }
  if (screen_mode == "radar" && selected_index < 0 && x >= 45 && x <= 113 && y >= 192 && y <= 230) {
    ai_answer = askGemini(false);
    screen_mode = "ai";
    return;
  }
  if (screen_mode == "radar" && selected_index < 0 && x >= 127 && x <= 195 && y >= 192 && y <= 230) {
    screen_mode = "menu";
    selected_index = -1;
    return;
  }
  if (screen_mode == "menu") {
    if (x >= 28 && x <= 112 && y >= 122 && y <= 150) screen_mode = "settings";
    else if (x >= 128 && x <= 212 && y >= 122 && y <= 150) screen_mode = "postal";
    else if (x >= 28 && x <= 112 && y >= 160 && y <= 188) {
      applyPostal(settings.postal);
      last_traffic_ms = 0;
      fetchTraffic();
      screen_mode = "radar";
    }
    else if (x >= 128 && x <= 212 && y >= 160 && y <= 188) {
      ai_answer = askGemini(false);
      screen_mode = "ai";
    } else if (x >= 72 && x <= 168 && y >= 84 && y <= 112) screen_mode = "radar";
    return;
  }
  if (screen_mode == "postal") {
    static const char *codes[] = {"1188", "1000", "1200", "1400", "2000", "3000", "8000"};
    int picked = -1;
    int start = postal_page * 4;
    if (y >= 98 && y <= 124) picked = start + (x < 120 ? 0 : 1);
    else if (y >= 132 && y <= 158) picked = start + (x < 120 ? 2 : 3);
    if (picked >= 0 && picked < 7) {
      applyPostal(codes[picked]);
      last_traffic_ms = 0;
      fetchTraffic();
      screen_mode = "menu";
      return;
    }
    if (x >= 34 && x <= 88 && y >= 168 && y <= 192) postal_page = 0;
    else if (x >= 94 && x <= 148 && y >= 168 && y <= 192) postal_page = 1;
    else if (x >= 154 && x <= 208 && y >= 168 && y <= 192) screen_mode = "menu";
    return;
  }
  if (screen_mode == "settings") {
    if (y >= 96 && y <= 124) {
      if (x >= 34 && x <= 72) settings.range_km = 20;
      else if (x >= 78 && x <= 116) settings.range_km = 50;
      else if (x >= 122 && x <= 160) settings.range_km = 100;
      else if (x >= 166 && x <= 204) settings.range_km = 250;
      last_traffic_ms = 0;
      fetchTraffic();
    } else if (x >= 70 && x <= 170 && y >= 150 && y <= 180) screen_mode = "menu";
    return;
  }
  if (screen_mode == "ai") {
    screen_mode = "menu";
    return;
  }
  int best = -1;
  float best_px = 18;
  for (int i = 0; i < aircraft_count; ++i) {
    int ax, ay;
    polarToScreen(aircraft[i].bearing_deg, aircraft[i].distance_km, settings.range_km, ax, ay);
    float d = hypotf(x - ax, y - ay);
    if (d < best_px) {
      best_px = d;
      best = i;
    }
  }
  if (best >= 0) {
    selected_index = best;
    queuePhotoForAircraft(selected_index);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  pinMode(PIN_TOUCH_INT, INPUT);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(20);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  Wire.begin(PIN_I2C_SDA_TOUCH, PIN_I2C_SCL_TOUCH, 50000);

  if (!canvas->begin(40000000)) {
    display->begin(40000000);
    gfx = display;
  }
  gfx->fillScreen(COL_BG);
  drawCentered("FLIGHTDESK", 94, COL_GREEN, 2);
  drawCentered("BOOT", 124, COL_TEXT, 1);
  gfx->flush();

  WiFi.mode(WIFI_STA);
  WiFi.begin(FLIGHTDESK_WIFI_SSID, FLIGHTDESK_WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  ArduinoOTA.setHostname("flightdesk-ball-v2");
  ArduinoOTA.begin();
  applyPostal(settings.postal);
  fetchTraffic();
}

void loop() {
  ArduinoOTA.handle();
  uint32_t now = millis();
  if (now - last_traffic_ms > TRAFFIC_INTERVAL_MS) {
    fetchTraffic();
    last_traffic_ms = now;
  }
  int tx, ty;
  if (now - last_touch_ms > 180 && readTouch(tx, ty)) {
    last_touch_ms = now;
    handleTouch(tx, ty);
  }
  if (screen_mode == "radar" && selected_index >= 0) {
    servicePendingPhoto();
  }
  if (now - last_frame_ms > FRAME_INTERVAL_MS) {
    sweep_deg = fmodf(sweep_deg + 4.0f, 360.0f);
    render();
    last_frame_ms = now;
  }
  delay(1);
}
