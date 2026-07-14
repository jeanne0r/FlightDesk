#include "audio_manager.h"
#include "board_pins.h"
#include <Arduino.h>

namespace flightdesk {

bool AudioManager::begin() {
    if (PIN_I2S_BCLK < 0 || PIN_I2S_LRCLK < 0 ||
        PIN_I2S_MIC_DIN < 0 || PIN_I2S_SPK_DOUT < 0) {
        Serial.println("[AUDIO] Désactivé : GPIO I2S non configurés");
        ready_ = false;
        return false;
    }

    // Initialisation I2S à ajouter après validation du brochage Waveshare.
    ready_ = true;
    return true;
}

void AudioManager::applySettings(const Settings& settings) {
    settings_ = settings;
}

bool AudioManager::playRadarBeep() {
    if (!ready_ || settings_.muted || !settings_.radar_beep) return false;
    // Génération du bip à implémenter avec le périphérique I2S.
    return true;
}

}  // namespace flightdesk
