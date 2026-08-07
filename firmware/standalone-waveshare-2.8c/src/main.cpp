#include <Arduino.h>
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

uint32_t lastStatusMs = 0;
uint32_t lastI2cScanMs = 0;

void printHeader() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(kBuildName);
  Serial.println("Target: Waveshare ESP32-S3-Touch-LCD-2.8C, SKU 29086");
  Serial.println("Display: 2.8 inch round capacitive touch LCD, 480x480");
  Serial.println("Bring-up: USB serial + Wi-Fi + official I2C bus scan");
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
