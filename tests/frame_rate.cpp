#include "runtime/frame_rate.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using mscharged::runtime::FrameRateMeter;
using mscharged::runtime::FrameRateTitle;
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
        const auto title = [](std::optional<double> rate) {
            return rate ? FrameRateTitle("Mario Strikers Charged", *rate) : std::string();
        };
        Check(!meter.Sample(100, t0), "First sample only opens the window");
        Check(!meter.Sample(115, t0 + 500ms), "No rate before one second");
        auto rate = meter.Sample(130, t0 + 1s);
        Check(rate && *rate == 30.0, "Thirty frames in one second");
        Check(title(rate) == "Mario Strikers Charged | 30.0 FPS", "Title with one decimal");
        Check(!meter.Sample(150, t0 + 1500ms), "Window restarts after each rate");
        Check(title(meter.Sample(189, t0 + 2s)) == "Mario Strikers Charged | 59.0 FPS",
              "Rate covers only the new window");
        Check(title(meter.Sample(219, t0 + 4s)) == "Mario Strikers Charged | 15.0 FPS",
              "Long windows average their frames");
        rate = meter.Sample(219, t0 + 5s);
        Check(rate && *rate == 0.0 && title(rate) == "Mario Strikers Charged | 0.0 FPS", "Stalled game reports zero");
        Check(title(meter.Sample(248, t0 + 6s + 50ms)) == "Mario Strikers Charged | 27.6 FPS",
              "One decimal from the actual elapsed time");
        Check(!meter.Sample(10, t0 + 8s), "A restarted counter opens a new window");
        Check(title(meter.Sample(70, t0 + 9s)) == "Mario Strikers Charged | 60.0 FPS",
              "Measurement resumes after a restart");
        std::cout << checks << " frame-rate checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
