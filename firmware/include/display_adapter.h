#pragma once
#include <Arduino.h>
#include <vector>
#include "aircraft.h"
#include "config.h"

namespace flightdesk {

/*
 * Adaptateur temporaire.
 *
 * Pour éviter un mauvais brochage, la V0 imprime l'état du radar dans le
 * moniteur série. Le prochain commit remplacera cette classe par l'adaptateur
 * graphique officiel Waveshare/LVGL de la révision exacte reçue.
 */
class DisplayAdapter {
public:
    bool begin();
    void renderRadar(
        const Settings& settings,
        const std::vector<Aircraft>& aircraft,
        float sweep_deg);
    void showBootMessage(const char* message);

private:
    uint32_t last_serial_render_ms_ = 0;
};

}  // namespace flightdesk
