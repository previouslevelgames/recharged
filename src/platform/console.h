#pragma once

#include "console_verbosity.h"

#include <aurora/aurora.h>

namespace mscharged::platform
{
// A development trace for stdout, printed only on a verbose console.
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void Trace(const char* format, ...);

// Aurora's log in Aurora's own format. Its log level still applies first; a
// normal console then drops info messages a player does not need.
void ConsoleLog(AuroraLogLevel level, const char* module, const char* message, unsigned int length);
}
