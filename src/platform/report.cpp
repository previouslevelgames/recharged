#include "NL/nlPrint.h"
#include <cstdarg>
#include <cstdio>

int nlPrintf(const char* format, ...)
{
    va_list args; va_start(args, format);
    const int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}
