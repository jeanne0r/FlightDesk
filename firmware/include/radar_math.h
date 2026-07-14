#pragma once
#include <math.h>

namespace flightdesk {

constexpr float kPi = 3.14159265358979323846f;

inline float degToRad(float value) { return value * kPi / 180.0f; }
inline float radToDeg(float value) { return value * 180.0f / kPi; }

float haversineKm(float lat1, float lon1, float lat2, float lon2);
float initialBearingDeg(float lat1, float lon1, float lat2, float lon2);

struct ScreenPoint {
    float x;
    float y;
    bool visible;
};

ScreenPoint polarToScreen(
    float bearing_deg,
    float distance_km,
    float range_km,
    float center_x,
    float center_y,
    float radius_px);

}  // namespace flightdesk
