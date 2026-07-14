#pragma once
#include <Arduino.h>
#include <vector>
#include "aircraft.h"

namespace flightdesk {

class GeminiClient {
public:
    bool begin(const String& api_key);
    String ask(const String& question, const std::vector<Aircraft>& context);

private:
    String api_key_;
};

}  // namespace flightdesk
