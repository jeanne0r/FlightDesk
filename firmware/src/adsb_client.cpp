#include "adsb_client.h"
#include "radar_math.h"

namespace flightdesk {

bool AdsbClient::begin() {
    aircraft_.reserve(32);
    return true;
}

bool AdsbClient::update(const Settings& settings) {
    // V0 : trafic simulé. L'API ADS-B réelle sera ajoutée dans un module séparé.
    createDemoTraffic(settings);
    last_error_ = "";
    return true;
}

void AdsbClient::createDemoTraffic(const Settings& settings) {
    static float phase = 0;
    phase += 2.0f;
    if (phase >= 360) phase -= 360;

    aircraft_.clear();

    const float bearings[] = {
        phase,
        fmodf(phase + 83.0f, 360.0f),
        fmodf(phase + 191.0f, 360.0f),
        fmodf(phase + 277.0f, 360.0f)
    };
    const float distances[] = {8.0f, 19.0f, 31.0f, 44.0f};
    const char* callsigns[] = {"SWR281", "EZY61Q", "AFR123", "RYR8XY"};

    for (int i = 0; i < 4; ++i) {
        Aircraft a;
        a.icao24 = String("demo") + i;
        a.callsign = callsigns[i];
        a.distance_km = distances[i];
        a.bearing_deg = bearings[i];
        a.altitude_m = 2500.0f + i * 2400.0f;
        a.speed_kmh = 520.0f + i * 80.0f;
        a.heading_deg = fmodf(bearings[i] + 35.0f, 360.0f);
        aircraft_.push_back(a);
    }
}

}  // namespace flightdesk
