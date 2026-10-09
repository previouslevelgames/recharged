#include "platform/screenshot_path.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
}

int main()
{
    namespace fs = std::filesystem;
    using mscharged::platform::NextScreenshotPath;

    const auto directory = fs::temp_directory_path() / "mscharged_screenshot_path_tests";
    fs::remove_all(directory);
    fs::create_directories(directory);

    // 2026-10-09 15:30:12.123 UTC; CTest runs this with TZ=UTC.
    const std::chrono::system_clock::time_point now{std::chrono::milliseconds{1791559812123LL}};

    const auto first = NextScreenshotPath(directory, now);
    Check(first.parent_path() == directory, "screenshots stay in the requested directory");
    Check(first.filename().string() == "screenshot_20261009-153012-123.png", "local timestamp with milliseconds");

    // The same instant again, e.g. while the first PNG is still being written.
    const auto second = NextScreenshotPath(directory, now);
    Check(second.filename().string() == "screenshot_20261009-153012-123_2.png", "pending name is not reused");

    // Existing files are never overwritten.
    std::ofstream(first).put('x');
    std::ofstream(second).put('x');
    const auto third = NextScreenshotPath(directory, now);
    Check(third.filename().string() == "screenshot_20261009-153012-123_3.png", "existing files are skipped");

    // A later instant starts without a suffix again.
    const auto later = NextScreenshotPath(directory, now + std::chrono::milliseconds{877});
    Check(later.filename().string() == "screenshot_20261009-153013-000.png", "milliseconds roll over into seconds");

    fs::remove_all(directory);
    if (failures == 0) std::printf("screenshot path checks passed\n");
    return failures == 0 ? 0 : 1;
}
