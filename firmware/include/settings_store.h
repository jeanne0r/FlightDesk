#pragma once
#include "config.h"

namespace flightdesk {

class SettingsStore {
public:
    bool begin();
    Settings load();
    bool save(const Settings& settings);
};

}  // namespace flightdesk
