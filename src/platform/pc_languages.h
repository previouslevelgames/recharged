#pragma once

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>

#include <cctype>
#include <string>
#include <vector>

namespace mscharged::platform
{
// The PC's preferred languages as lower-case ISO 639-1 codes ("de", "en"),
// preferred first; empty when the system reports none.
inline std::vector<std::string> PcLanguages()
{
    std::vector<std::string> codes;
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (!locales) return codes;
    for (int i = 0; i < count; ++i)
    {
        if (!locales[i] || !locales[i]->language) continue;
        std::string code = locales[i]->language;
        for (auto& c : code) c = char(std::tolower(static_cast<unsigned char>(c)));
        codes.push_back(code);
    }
    SDL_free(locales);
    return codes;
}
}
