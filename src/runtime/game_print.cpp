#include "NL/nlPrint.h"
#include <cstdarg>
#include <cstdio>

// Host byte-string formatting. Wii 16-bit wide-string formatting is separate.
int nlSNPrintf(char* buffer, unsigned long size, const char* format, ...)
{
    va_list args; va_start(args, format);
    const int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    if (size) buffer[size-1] = 0;
    return result;
}
int nlPrintf(const char* format, ...)
{
    va_list args; va_start(args, format);
    const int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}
