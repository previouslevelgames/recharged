#include "platform/string_format.h"
#include <cstddef>
#include <cstdarg>

static_assert(sizeof(wchar_t) == sizeof(unsigned short));
extern "C" int ChargedWii_vswprintf(wchar_t*, std::size_t, const wchar_t*, va_list);

namespace mscharged
{
int FormatString(unsigned short* output, std::size_t count,
                 const unsigned short* format, va_list arguments)
{
    // This unit and the isolated original MSL providers use the Wii16 wchar
    // ABI. Host libc wide APIs are never given the game's unsigned-short data.
    return ChargedWii_vswprintf(reinterpret_cast<wchar_t*>(output), count,
        reinterpret_cast<const wchar_t*>(format), arguments);
}
}
