#include "platform/system.h"

namespace { std::uint8_t system_language = 1; }
extern "C" std::uint8_t SCGetLanguage() { return system_language; }

namespace mscharged
{
void SetStartupSystemLanguage(std::uint8_t language) { system_language = language; }
}
