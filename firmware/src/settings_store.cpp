#include "settings_store.h"
#include <Preferences.h>

namespace flightdesk {

static Preferences prefs;

bool SettingsStore::begin() {
    return prefs.begin("flightdesk", false);
}

Settings SettingsStore::load() {
    Settings s;
    s.home_latitude = prefs.getFloat("lat", s.home_latitude);
    s.home_longitude = prefs.getFloat("lon", s.home_longitude);
    s.radar_range_km = prefs.getUShort("range", s.radar_range_km);
    s.volume_percent = prefs.getUChar("volume", s.volume_percent);
    s.muted = prefs.getBool("muted", s.muted);
    s.radar_beep = prefs.getBool("beep", s.radar_beep);
    s.brightness_percent = prefs.getUChar("bright", s.brightness_percent);

    if (!validRange(s.radar_range_km)) s.radar_range_km = 50;
    if (s.volume_percent > 100) s.volume_percent = 55;
    if (s.brightness_percent > 100) s.brightness_percent = 80;
    return s;
}

bool SettingsStore::save(const Settings& s) {
    bool ok = true;
    ok &= prefs.putFloat("lat", s.home_latitude) > 0;
    ok &= prefs.putFloat("lon", s.home_longitude) > 0;
    ok &= prefs.putUShort("range", s.radar_range_km) > 0;
    ok &= prefs.putUChar("volume", s.volume_percent) > 0;
    ok &= prefs.putBool("muted", s.muted) > 0;
    ok &= prefs.putBool("beep", s.radar_beep) > 0;
    ok &= prefs.putUChar("bright", s.brightness_percent) > 0;
    return ok;
}

}  // namespace flightdesk
