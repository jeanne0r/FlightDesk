# Xiaozhi Ball V2 Preview

FlightDesk can run today on a Xiaozhi Ball V2 as a server-rendered round-screen
preview.

The firmware intentionally keeps the ESP32 side small:

- the server renders `/api/esp32/radar.png` as a 240x240 radar image;
- the Ball downloads that PNG every 30 seconds;
- the Ball redraws a local sweep every 100 ms for smooth motion;
- touch coordinates are forwarded to `/api/esp32/action`;
- all UI state stays on the FlightDesk server.

This makes iteration fast and keeps the path open for a later standalone
Internet product. The final product can replace the LAN server with an embedded
or cloud endpoint without redesigning the round-screen UI.

## Flashing

Use the example config in
[`firmware/esphome/xiaozhi-ball-v2`](../firmware/esphome/xiaozhi-ball-v2).

```bash
cd firmware/esphome/xiaozhi-ball-v2
cp secrets.example.yaml secrets.yaml
esphome compile flightdesk-ball-v2.yaml
esphome upload flightdesk-ball-v2.yaml
```

Before compiling, set `flightdesk_base_url` to the machine running
`simulator/server.py`.

## Current UX

- Main radar screen with green OSM map watermark.
- One `MENU` control instead of several tiny bottom buttons.
- Menu pages for settings, map controls, range and recenter.
- Aircraft popup with callsign, type, route city names and photo when available.
- Tap the aircraft popup to ask the AI for a compact selected-flight summary.
- The `IA` menu button asks for a compact traffic summary around the configured
  postal code.
- Server-side fallbacks if the traffic provider or photo provider is temporarily
  unavailable.

## Limitations

- Plane photos depend on public PlaneSpotters data and are not guaranteed for
  every aircraft.
- Gemini requires `GEMINI_API_KEY` on the server. Without it, the AI page uses a
  local deterministic summary from the live aircraft data.
- The current Ball firmware does not implement pinch zoom. Zoom and map movement
  are controlled through server-rendered touch targets.
- This is a preview firmware, not the final embedded C++/LVGL firmware.
