#pragma once

#include <charconv>
#include <system_error>

#if defined(__APPLE__) || defined(MSCHARGED_STREAM_FLOAT_PARSE)
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#endif

namespace mscharged
{
// std::from_chars for float. Apple's libc++ offers the floating-point overload
// only from macOS 26 on, below the packages' deployment target (macOS 15), so
// Apple parses the same decimal syntax with a classic-locale stream instead.
inline std::from_chars_result FromCharsFloat(const char* first, const char* last, float& value)
{
#if defined(__APPLE__) || defined(MSCHARGED_STREAM_FLOAT_PARSE) // the latter: tests on other hosts
    // from_chars accepts neither leading whitespace nor '+'.
    if (first == last || *first == '+' || *first == ' ' || *first == '\t' || *first == '\n' || *first == '\r')
        return {first, std::errc::invalid_argument};
    std::istringstream stream(std::string(first, last));
    stream.imbue(std::locale::classic());
    float parsed = 0;
    stream >> std::noskipws >> parsed;
    if (stream.fail()) return {first, std::errc::invalid_argument};
    const auto position = stream.eof() ? static_cast<std::streamoff>(last - first) : static_cast<std::streamoff>(stream.tellg());
    if (!std::isfinite(parsed)) return {first + position, std::errc::result_out_of_range};
    value = parsed;
    return {first + position, std::errc{}};
#else
    return std::from_chars(first, last, value);
#endif
}
}
