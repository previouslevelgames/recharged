#include "platform/screenshot_path.h"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <system_error>

namespace mscharged::platform
{
namespace
{
std::tm LocalTime(std::time_t time)
{
    std::tm result{};
#if defined(_WIN32)
    localtime_s(&result, &time);
#else
    localtime_r(&time, &result);
#endif
    return result;
}
}

std::filesystem::path NextScreenshotPath(
    const std::filesystem::path& directory, std::chrono::system_clock::time_point now)
{
    static std::mutex mutex;
    static std::filesystem::path previous;
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const std::tm local = LocalTime(std::chrono::system_clock::to_time_t(seconds));
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d-%03d", local.tm_year + 1900, local.tm_mon + 1,
        local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(milliseconds));
    std::lock_guard lock(mutex);
    for (unsigned suffix = 1;; ++suffix)
    {
        std::string name = std::string("screenshot_") + stamp;
        if (suffix > 1) name += "_" + std::to_string(suffix);
        auto path = directory / (name + ".png");
        std::error_code error;
        if (path != previous && !std::filesystem::exists(path, error))
        {
            previous = path;
            return path;
        }
    }
}
}
