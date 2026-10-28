#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mscharged
{
inline std::string PathUtf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()}; // Handles char and C++20 char8_t.
}

inline std::filesystem::path PathFromUtf8(std::string_view value)
{
#ifdef __SWITCH__
    // libnx names paths on the SD card sdmc:/..., which std::filesystem takes for
    // relative paths: absolute() puts the working folder in front of them again.
    // The card is the default device, so the path without its name is the same file.
    if (value.substr(0, 6) == "sdmc:/") value.remove_prefix(5);
#endif
#ifdef __cpp_char8_t
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
#else
    return std::filesystem::u8path(value.begin(), value.end());
#endif
}
}
