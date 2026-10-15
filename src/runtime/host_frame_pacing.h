#pragma once

// The host side of a frame rate above the original (display.frame_rate,
// bootstrap/frame_rate.h): Aurora presents each frame as the game swaps and
// paces the next, with a frame slot for the refresh rate of the window's
// display. Owner (game) thread only.

#include "bootstrap/frame_rate.h"

#include <chrono>
#include <string>

struct SDL_Window;

namespace mscharged::runtime {

// The refresh rate of the display the window is on, 0 when unknown.
double WindowRefreshRate(SDL_Window* window);

class HostFramePacing {
public:
    // Turns Aurora's host pacing on; `rate` must be above the original.
    HostFramePacing(FrameRate rate, bool vsync, SDL_Window* window);
    // Once a frame: about once a second, follows the window to another
    // display (or a changed refresh rate) and logs the new pacing.
    void Update(std::chrono::steady_clock::time_point now);
    std::string Description() const;

private:
    void Apply(double display_hz);

    FrameRate rate_;
    bool vsync_;
    SDL_Window* window_;
    double display_hz_ = -1.0;
    FramePacing pacing_{};
    std::chrono::steady_clock::time_point next_check_{};
};

} // namespace mscharged::runtime
