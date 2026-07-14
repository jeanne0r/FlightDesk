#pragma once
#include <stdint.h>

namespace flightdesk {

struct Settings {
    float home_latitude = 46.5197f;
    float home_longitude = 6.6323f;
    uint16_t radar_range_km = 50;
    uint8_t volume_percent = 55;
    bool muted = false;
    bool radar_beep = true;
    uint8_t brightness_percent = 80;
};

inline bool validRange(uint16_t value) {
    return value == 20 || value == 50 || value == 100 || value == 250;
}

}  // namespace flightdesk
