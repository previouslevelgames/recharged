#include "NL/nlPrint.h"
#include "console_verbosity.h"
#include <cstdarg>
#include <cstdio>

// The game's debug output: a normal console omits it, the length stays.
int nlPrintf(const char* format, ...)
{
    va_list args; va_start(args, format);
    const int result = mscharged::platform::VerboseConsole() ? vfprintf(stderr, format, args)
                                                             : vsnprintf(nullptr, 0, format, args);
    va_end(args);
    return result;
}
