# FlightDesk for Waveshare ESP32-S3-Touch-LCD-2.8C

This branch targets the hardware received for the final FlightDesk device:
Waveshare `ESP32-S3-Touch-LCD-2.8C`, SKU `29086`.

The first commit is a bring-up target, not the final radar UI. It validates the
board safely before porting the autonomous Xiaozhi UI to the 480 x 480 RGB
panel.

## Hardware facts

From the official Waveshare documentation:

- ESP32-S3R8 dual-core MCU.
- 16 MB Flash and 8 MB PSRAM.
- 2.8 inch round capacitive touch LCD, 480 x 480.
- I2C touch interface with interrupt support.
- Onboard QMI8658 IMU, PCF85063 RTC, TF card slot, buzzer, battery charger.
- External I2C header uses `SCL=GPIO7` and `SDA=GPIO15`.

Reference: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C>

## Current target

`waveshare_2_8c_bringup` currently checks:

- USB serial boot.
- ESP32-S3 16 MB Flash / 8 MB OPI PSRAM board profile.
- Wi-Fi connection when local secrets are supplied.
- I2C bus scan on the documented Waveshare I2C pins.

It deliberately does not drive the LCD yet. The 2.8C uses a 480 x 480 RGB panel
path and Waveshare's demo stack rather than the Xiaozhi Ball GC9A01 SPI display
path. The next porting step is to import only the required display/touch driver
initialization from the official demo package, then reuse the FlightDesk radar
renderer on that backend.

## Build

```bash
cd firmware/standalone-waveshare-2.8c
cp include/secrets.example.h include/secrets.h
# Fill secrets locally. Do not commit include/secrets.h.
platformio run
```

## Flash by USB

Connect the `USB TO UART` Type-C port, then:

```bash
platformio run -t upload
platformio device monitor
```

The monitor should show detected I2C devices, Wi-Fi status and free heap/PSRAM.

## Next steps

- Import the official Waveshare 2.8C display initialization.
- Add the 480 x 480 FlightDesk radar renderer.
- Add capacitive touch mapping and calibration.
- Re-enable Airplanes.live, Gemini and audio after the screen/touch base is
  stable.
