#include "NL/nlTicker.h"
#include "NL/nlTime.h"

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/time.hpp>
#include <dolphin/os.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

void AuroraOSShutdown();
// The bounded SDK fixture configures actual memory services without a window.
namespace aurora { extern AuroraConfig g_config; }

namespace {
unsigned checks;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

using HostClock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::int64_t kWiiTicksPerSecond = 60750000;

template<class ReadClock>
void RealTime(ReadClock read, const char* message) {
    // Host observations bracket each actual SDK/source read. Independent
    // hardware units bound its elapsed ticks without a fake clock provider.
    const auto before_start = HostClock::now();
    const auto first = read();
    const auto after_start = HostClock::now();
    std::this_thread::sleep_for(20ms);
    const auto before_end = HostClock::now();
    const auto last = read();
    const auto after_end = HostClock::now();
    const auto minimum = std::chrono::duration_cast<std::chrono::nanoseconds>(before_end - after_start).count();
    const auto maximum = std::chrono::duration_cast<std::chrono::nanoseconds>(after_end - before_start).count();
    const auto low_ticks = minimum * kWiiTicksPerSecond / 1000000000 - 2;
    const auto high_ticks = maximum * kWiiTicksPerSecond / 1000000000 + 2;
    const auto ticks = static_cast<std::int64_t>(last - first);
    Check(ticks >= low_ticks && ticks <= high_ticks, message);
}

struct Cleanup {
    bool initialized{};
    ~Cleanup() {
        aurora::time::set_scale(1.0f);
        if (initialized) AuroraOSShutdown();
        SDL_Quit();
    }
};

void OriginalConversions() {
    // The original ticker rounds to whole microseconds, then multiplies by
    // its original float0.001. Its u32 left shift intentionally wraps.
    struct Case { std::uint32_t ticks; float milliseconds; };
    constexpr std::array cases{
        Case{0, 0.0f}, Case{60, 0.0f}, Case{61, 0.001f},
        Case{60750, 1.0f}, Case{60750000, 1000.0f},
        Case{0x20000000u, 0.0f}, Case{0x2000003du, 0.001f}
    };
    for (const auto& row : cases) {
        const float actual = nlTicksToMilliseconds(row.ticks);
        Check(std::abs(actual - row.milliseconds) <= 0.0002f,
              "original ticker units/rounding/shift wrap changed");
    }
    Check(nlGetTickerDifference(0xffffffc3u, 0u) == 0.001f,
          "original low32 ticker difference wrap changed");
    Check(std::abs(nlGetTimeDifference(123, 60750123) - 1000.0f) <= 0.0002f,
          "original64 time difference conversion changed");
    Check(std::abs(nlGetTimeDifference(0, 607500000) - 10000.0f) <= 0.002f,
          "original64 time conversion was narrowed to low32 ticker rules");
}
} // namespace

int main() {
    Cleanup cleanup;
    try {
        Check(SDL_Init(0), "native SDK clock fixture SDL initialization failed");
        aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size = 0;
        OSInit();
        cleanup.initialized = true;
        Check(__OSBusClock == 243000000 && __OSCoreClock == 729000000,
              "native low-memory clock fields are not the Wii hardware values");
        Check(OSSecondsToTicks(std::int64_t{1}) == kWiiTicksPerSecond &&
              OSTicksToSeconds(kWiiTicksPerSecond) == 1 &&
              OSMillisecondsToTicks(1000LL) == kWiiTicksPerSecond &&
              OSMicrosecondsToTicks(1000000LL) == kWiiTicksPerSecond &&
              OSNanosecondsToTicks(1000000000LL) == kWiiTicksPerSecond,
              "SDK tick conversion callers use different hardware units");
        nlInitTicker();
        nlInitTime();
        OriginalConversions();

        // Presentation pause/scale is separate from the actual hardware time
        // requested by reconstructed source. No source pause logic is changed.
        for (float scale : {1.0f, 0.0f, 0.5f, 2.0f}) {
            aurora::time::set_scale(scale);
            RealTime([] { return OSGetTime(); }, "SDK hardware time rate/continuity mismatch");
            RealTime([] { return nlGetTime(); }, "original source time did not use actual hardware ticks");
            RealTime([] { return OSGetNativeTime(); }, "native SDK clock hardware units mismatch");
            const auto before = aurora::time::game_clock::now();
            const auto ticker_before = nlGetTicker();
            std::this_thread::sleep_for(2ms);
            const auto ticker_after = nlGetTicker();
            Check(std::uint32_t(ticker_after - ticker_before) > 0,
                  "original low32 ticker stopped with presentation pause");
            if (scale == 0.0f) Check(aurora::time::game_clock::now() == before,
                                    "SDK policy changed Aurora presentation pause");
        }
        std::cout << "native Wii clock checks=" << checks
                  << " original ticker/time and hardware pause/scale continuity pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native Wii clock failure after " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
