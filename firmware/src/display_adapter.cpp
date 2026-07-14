#include "display_adapter.h"

namespace flightdesk {

bool DisplayAdapter::begin() {
    Serial.println("[DISPLAY] Adaptateur série actif");
    return true;
}

void DisplayAdapter::showBootMessage(const char* message) {
    Serial.printf("[BOOT] %s\n", message);
}

void DisplayAdapter::renderRadar(
    const Settings& settings,
    const std::vector<Aircraft>& aircraft,
    float sweep_deg) {

    const uint32_t now = millis();
    if (now - last_serial_render_ms_ < 1000) return;
    last_serial_render_ms_ = now;

    Serial.printf(
        "[RADAR] balayage=%3.0f° rayon=%u km avions=%u volume=%u%% muet=%s\n",
        sweep_deg,
        settings.radar_range_km,
        static_cast<unsigned>(aircraft.size()),
        settings.volume_percent,
        settings.muted ? "oui" : "non");

    for (const auto& a : aircraft) {
        if (a.distance_km <= settings.radar_range_km) {
            Serial.printf(
                "  %-8s %5.1f km %6.1f° %5.0f m %4.0f km/h\n",
                a.callsign.c_str(),
                a.distance_km,
                a.bearing_deg,
                a.altitude_m,
                a.speed_kmh);
        }
    }
}

}  // namespace flightdesk
