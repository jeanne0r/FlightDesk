#include "gemini_client.h"

namespace flightdesk {

bool GeminiClient::begin(const String& api_key) {
    api_key_ = api_key;
    return !api_key_.isEmpty();
}

String GeminiClient::ask(
    const String& question,
    const std::vector<Aircraft>& context) {

    // Stub volontaire : la clé et l'API ne doivent jamais être codées en dur.
    if (api_key_.isEmpty()) return "Clé Gemini non configurée.";
    if (question.isEmpty()) return "Question vide.";

    return String("Gemini sera connecté dans une prochaine version. Avions visibles : ")
        + context.size();
}

}  // namespace flightdesk
