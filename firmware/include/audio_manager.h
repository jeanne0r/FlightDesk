#pragma once
#include "config.h"

namespace flightdesk {

class AudioManager {
public:
    bool begin();
    void applySettings(const Settings& settings);
    bool playRadarBeep();
    bool isReady() const { return ready_; }

private:
    bool ready_ = false;
    Settings settings_;
};

}  // namespace flightdesk
