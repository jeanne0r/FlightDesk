#include "radar_math.h"

namespace flightdesk {

float haversineKm(float lat1, float lon1, float lat2, float lon2) {
    constexpr float earth_km = 6371.0f;
    const float p1 = degToRad(lat1);
    const float p2 = degToRad(lat2);
    const float dp = degToRad(lat2 - lat1);
    const float dl = degToRad(lon2 - lon1);

    const float a =
        sinf(dp / 2) * sinf(dp / 2) +
        cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);

    const float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    return earth_km * c;
}

float initialBearingDeg(float lat1, float lon1, float lat2, float lon2) {
    const float p1 = degToRad(lat1);
    const float p2 = degToRad(lat2);
    const float dl = degToRad(lon2 - lon1);

    const float y = sinf(dl) * cosf(p2);
    const float x = cosf(p1) * sinf(p2) -
                    sinf(p1) * cosf(p2) * cosf(dl);

    float bearing = radToDeg(atan2f(y, x));
    if (bearing < 0) bearing += 360.0f;
    return bearing;
}

ScreenPoint polarToScreen(
    float bearing_deg,
    float distance_km,
    float range_km,
    float center_x,
    float center_y,
    float radius_px) {

    if (range_km <= 0 || distance_km > range_km) {
        return {center_x, center_y, false};
    }

    const float angle = degToRad(bearing_deg - 90.0f);
    const float r = radius_px * (distance_km / range_km);

    return {
        center_x + cosf(angle) * r,
        center_y + sinf(angle) * r,
        true
    };
}

}  // namespace flightdesk
