#pragma once

#include <chrono>
#include <filesystem>

namespace mscharged::platform
{
// <directory>/screenshot_<YYYYMMDD-HHMMSS-mmm>.png for `now` in local time. A
// numbered suffix keeps the name unique when the file already exists or is the
// previous result, whose PNG may still be waiting to be written.
std::filesystem::path NextScreenshotPath(
    const std::filesystem::path& directory, std::chrono::system_clock::time_point now);
}
