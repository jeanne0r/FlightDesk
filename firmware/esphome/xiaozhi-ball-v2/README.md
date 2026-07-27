# FlightDesk for Xiaozhi Ball V2

This is the current working prototype firmware for a **Xiaozhi Ball V2** round
ESP32-S3 screen.

The Ball stays lightweight: it downloads a 240x240 PNG radar frame from the
FlightDesk simulator/server and sends touch coordinates back to the server.
The server handles live traffic, map tiles, aircraft photos, popups, menus,
postal code settings and screen state.

## What Works

- Round 240x240 radar display.
- Live aircraft through the FlightDesk server.
- Smooth local radar sweep at 100 ms refresh.
- Touch forwarding to `/api/esp32/action`.
- Server-rendered menu, settings, map/recenter controls and aircraft popup.
- Server-rendered AI page: tap an aircraft popup for selected-flight info, or
  use `IA` in the menu for nearby traffic questions.
- PlaneSpotters aircraft thumbnails when available.
- OTA through ESPHome.

## Hardware

Tested on Xiaozhi Ball V2 style hardware:

- ESP32-S3 with 16 MB flash and PSRAM.
- GC9A01A 240x240 round SPI display.
- CST816 touch controller.
- ES8311 audio hardware may be present, but this FlightDesk example does not
  require voice assistant or Home Assistant.

Pin mappings are in `flightdesk-ball-v2.yaml` substitutions and come from the
tested Ball V2 unit.

## Setup

1. Start the FlightDesk server on your LAN:

   ```bash
   cd simulator
   python3 server.py
   ```

2. Copy the ESPHome example secrets:

   ```bash
   cd firmware/esphome/xiaozhi-ball-v2
   cp secrets.example.yaml secrets.yaml
   ```

3. Edit `secrets.yaml` locally with your Wi-Fi credentials.

4. Edit `flightdesk_base_url` in `flightdesk-ball-v2.yaml` so it points to your
   FlightDesk server, for example:

   ```yaml
   flightdesk_base_url: "http://192.168.1.16:4173"
   ```

5. Compile and upload:

   ```bash
   esphome compile flightdesk-ball-v2.yaml
   esphome upload flightdesk-ball-v2.yaml
   ```

## Notes

- Do not commit `secrets.yaml`.
- The PNG refresh is intentionally slower than the display refresh. The server
  frame updates every 30 seconds, while the Ball draws the sweep locally every
  100 ms for a smooth radar feel.
- Gemini runs on the FlightDesk server with `GEMINI_API_KEY`. If no key is set,
  the server still returns a compact local answer from the live aircraft data.
- This preview is designed to evolve toward an autonomous Internet product, not
  a Home Assistant dashboard.
