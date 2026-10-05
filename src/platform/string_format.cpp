#include "platform/string_format.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

namespace mscharged
{
int FormatString(char* buffer, std::size_t size, const char* format, va_list args)
{
    // MSL's __StringWrite consumes aliased %s input before terminating the
    // output. The original nlFont::Load depends on that behavior. A separate
    // native output buffer avoids libc clearing the game's input first.
    std::vector<char> output(size);
    const int result = vsnprintf(size ? output.data() : nullptr, size, format, args);
    if (size)
    {
        const auto count = result >= 0
            ? std::min<std::size_t>(size, static_cast<std::size_t>(result) + 1)
            : static_cast<std::size_t>(std::find(output.begin(), output.end(), '\0') - output.begin()) + 1;
        std::memcpy(buffer, output.data(), std::min<std::size_t>(count, size));
    }
    return result;
}
}
