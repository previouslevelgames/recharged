#include "runtime/frame_rate_title.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

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
        const FrameRateTitle::Clock::time_point t0{};
        FrameRateTitle title("Mario Strikers Charged");
        Check(!title.Sample(100, t0), "First sample only opens the window");
        Check(!title.Sample(115, t0 + 500ms), "No title before one second");
        auto text = title.Sample(130, t0 + 1s);
        Check(text && *text == "Mario Strikers Charged | 30.0 FPS", "Thirty frames in one second");
        Check(!title.Sample(150, t0 + 1500ms), "Window restarts after each title");
        text = title.Sample(189, t0 + 2s);
        Check(text && *text == "Mario Strikers Charged | 59.0 FPS", "Rate covers only the new window");
        text = title.Sample(219, t0 + 4s);
        Check(text && *text == "Mario Strikers Charged | 15.0 FPS", "Long windows average their frames");
        text = title.Sample(219, t0 + 5s);
        Check(text && *text == "Mario Strikers Charged | 0.0 FPS", "Stalled game reports zero");
        text = title.Sample(248, t0 + 6s + 50ms);
        Check(text && *text == "Mario Strikers Charged | 27.6 FPS", "One decimal from the actual elapsed time");
        Check(!title.Sample(10, t0 + 8s), "A restarted counter opens a new window");
        text = title.Sample(70, t0 + 9s);
        Check(text && *text == "Mario Strikers Charged | 60.0 FPS", "Measurement resumes after a restart");
        std::cout << checks << " frame-rate title checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
