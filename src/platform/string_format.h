#pragma once
#include <cstddef>
#include <cstdarg>
// C linkage gives the Wii16 formatter an ABI-stable name, so an original-game
// module can import it from the native host on every platform.
extern "C" int mscharged_format_wii16(unsigned short* output, std::size_t count,
                                      const unsigned short* format, va_list arguments);
namespace mscharged
{
int FormatString(char* output, std::size_t count, const char* format, va_list arguments);
inline int FormatString(unsigned short* output, std::size_t count, const unsigned short* format,
                        va_list arguments)
{
    return mscharged_format_wii16(output, count, format, arguments);
}
}
