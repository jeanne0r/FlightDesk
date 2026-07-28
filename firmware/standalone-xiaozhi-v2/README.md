# Standalone Xiaozhi Ball V2 Firmware

This firmware is the autonomous FlightDesk target for the Xiaozhi Ball V2.

It does not depend on the FlightDesk simulator/backend. The ESP32 fetches live
traffic directly from Internet APIs, renders the round radar locally, handles
touch locally and calls Gemini directly when a key is configured.

## Setup

```bash
cd firmware/standalone-xiaozhi-v2
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h` locally:

```cpp
#define FLIGHTDESK_WIFI_SSID "..."
#define FLIGHTDESK_WIFI_PASSWORD "..."
#define FLIGHTDESK_GEMINI_API_KEY "..."
```

Do not commit `include/secrets.h`.

## Build

```bash
pio run
```

USB upload:

```bash
pio run -t upload
```

First migration from an ESPHome firmware already installed on the Ball:

```bash
esphome upload /path/to/flightdesk-ball-v2.yaml \
  --device 192.168.1.198 \
  --file .pio/build/xiaozhi_ball_v2/firmware.bin
```

This standalone firmware includes ArduinoOTA with hostname
`flightdesk-ball-v2` for later network updates after it has been installed.

## Current Scope

- Direct Wi-Fi connection.
- Airplanes.live traffic around a configurable Swiss postal code.
- Local radar rendering on GC9A01A 240x240.
- CST816 touch handling.
- Range menu: 20, 50, 100, 250 km.
- Selected-aircraft popup.
- Gemini summary for the selected aircraft or nearby traffic.
- Local stylized green map watermark.
- ArduinoOTA for later standalone updates.

Map tiles and aircraft photos are intentionally left for a second embedded pass:
they need more careful memory/caching work than the thin-client ESPHome preview.
