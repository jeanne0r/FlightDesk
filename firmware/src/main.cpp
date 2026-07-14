#include <Arduino.h>

#include "adsb_client.h"
#include "audio_manager.h"
#include "config.h"
#include "display_adapter.h"
#include "settings_store.h"

using namespace flightdesk;

SettingsStore settings_store;
Settings settings;
AdsbClient adsb;
DisplayAdapter display;
AudioManager audio;

static float sweep_deg = 0;
static uint32_t last_aircraft_update_ms = 0;
static uint32_t last_frame_ms = 0;

void setup() {
    Serial.begin(115200);
    delay(500);

    display.begin();
    display.showBootMessage("FlightDesk v0.1.0");
    display.showBootMessage("Chargement des réglages");

    if (settings_store.begin()) {
        settings = settings_store.load();
    } else {
        Serial.println("[SETTINGS] Preferences indisponibles, valeurs par défaut");
    }

    adsb.begin();
    audio.begin();
    audio.applySettings(settings);

    display.showBootMessage("Radar prêt");
}

void loop() {
    const uint32_t now = millis();

    if (now - last_aircraft_update_ms >= 2000) {
        adsb.update(settings);
        last_aircraft_update_ms = now;
    }

    if (now - last_frame_ms >= 33) {
        sweep_deg += 2.0f;
        if (sweep_deg >= 360.0f) sweep_deg -= 360.0f;

        display.renderRadar(settings, adsb.aircraft(), sweep_deg);
        last_frame_ms = now;
    }

    delay(1);
}
