#pragma once
#include <vector>
#include "aircraft.h"
#include "config.h"

namespace flightdesk {

class AdsbClient {
public:
    bool begin();
    bool update(const Settings& settings);
    const std::vector<Aircraft>& aircraft() const { return aircraft_; }
    String lastError() const { return last_error_; }

private:
    std::vector<Aircraft> aircraft_;
    String last_error_;
    void createDemoTraffic(const Settings& settings);
};

}  // namespace flightdesk
