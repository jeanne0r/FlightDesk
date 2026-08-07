# Hardware targets

## Xiaozhi Ball V2

Role: simple preview target.

Keep this firmware intentionally limited:

- local radar rendering;
- live aircraft data when Wi-Fi is configured;
- basic touch selection;
- no final audio stack;
- no final Gemini voice stack;
- no enclosure-critical pinout work.

The Xiaozhi Ball V2 remains useful for quick UI checks, but it is not the final
FlightDesk hardware target.

## Waveshare ESP32-S3-Touch-LCD-2.8C

Role: final FlightDesk target.

Board received:

- Waveshare `ESP32-S3-Touch-LCD-2.8C`;
- SKU `29086`;
- ESP32-S3R8;
- 16 MB Flash;
- 8 MB PSRAM;
- round 2.8 inch capacitive touch LCD;
- 480 x 480 display;
- I2C touch interface;
- external I2C header documented as `SCL=GPIO7`, `SDA=GPIO15`.

Reference: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C>

Final modules to integrate:

- INMP441 I2S microphone;
- MAX98357A I2S amplifier;
- 40 mm 4 ohm / 3 W speaker;
- USB-C 5 V / 2 A power;
- optional SD/cache later only if needed.

## AI decision

The Waveshare board can handle FlightDesk AI as an Internet client:

- record a short PCM audio buffer from INMP441;
- send it to Gemini over HTTPS;
- receive short text or audio response;
- display the answer and optionally play synthesized audio.

It should not run a local language model. The ESP32-S3 has enough PSRAM for a
responsive 480 x 480 UI, networking and short audio buffers, but a useful local
LLM would be too large and too slow for this product.

Implementation rule: keep Gemini optional. Radar, aircraft selection and basic
announcements must work without any Gemini key.
