#pragma once
#include <Arduino.h>

namespace flightdesk {

struct Aircraft {
    String icao24;
    String callsign;
    float latitude = 0;
    float longitude = 0;
    float altitude_m = 0;
    float speed_kmh = 0;
    float heading_deg = 0;
    float distance_km = 0;
    float bearing_deg = 0;
    bool on_ground = false;
};

}  // namespace flightdesk
