#include "runtime/frame_rate.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using mscharged::runtime::FrameRateMeter;
using namespace std::chrono_literals;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try {
        const FrameRateMeter::Clock::time_point t0{};
        FrameRateMeter meter;
        Check(!meter.Sample(100, t0), "First sample only opens the window");
        Check(!meter.Sample(115, t0 + 500ms), "No rate before one second");
        auto rate = meter.Sample(130, t0 + 1s);
        Check(rate && *rate == 30.0, "Thirty frames in one second");
        Check(!meter.Sample(150, t0 + 1500ms), "Window restarts after each rate");
        Check(meter.Sample(189, t0 + 2s) == 59.0,
              "Rate covers only the new window");
        Check(meter.Sample(219, t0 + 4s) == 15.0,
              "Long windows average their frames");
        rate = meter.Sample(219, t0 + 5s);
        Check(rate && *rate == 0.0, "Stalled game reports zero");
        rate = meter.Sample(248, t0 + 6s + 50ms);
        Check(rate && std::abs(*rate - 29.0 / 1.05) < 1e-9, "Rate uses the actual elapsed time");
        Check(!meter.Sample(10, t0 + 8s), "A restarted counter opens a new window");
        Check(meter.Sample(70, t0 + 9s) == 60.0,
              "Measurement resumes after a restart");
        std::cout << checks << " frame-rate checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
