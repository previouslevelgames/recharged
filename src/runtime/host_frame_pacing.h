#pragma once

// The host side of a frame rate other than the original (display.frame_rate,
// bootstrap/frame_rate.h): Aurora presents each frame as the game swaps and
// paces the next, with a frame slot for the refresh rate of the window's
// display. With display.background_throttle it also holds the rate down while
// the window is in the background, the original's included: the game swaps
// through the host only while that lasts. Owner (game) thread only.

#include "bootstrap/frame_rate.h"

#include <chrono>
#include <string>

struct SDL_Window;

namespace mscharged::runtime {

// The refresh rate of the display the window is on, 0 when unknown.
double WindowRefreshRate(SDL_Window* window);

// Minimized, hidden or covered, or (with `focus_counts`) without the focus.
bool WindowInBackground(SDL_Window* window, bool focus_counts);

class HostFramePacing {
public:
    // `configure_game` is the game module's charged_configure_frame_pacing:
    // the host's swap, or null for the game's own VI pacing. Turns Aurora's
    // host pacing on at once for a rate other than the original.
    // `background_hz` (kBackgroundFrameRates, 0 for none) holds the rate
    // while WindowInBackground; `focus_counts` is its argument.
    HostFramePacing(FrameRate rate, bool vsync, SDL_Window* window, void (*configure_game)(void (*)()),
                    int background_hz, bool focus_counts);
    // Once a frame: follows the window into and out of the background and,
    // about once a second, to another display (or a changed refresh rate),
    // and logs the new pacing.
    void Update(std::chrono::steady_clock::time_point now);
    std::string Description() const;
    // A network match with rollback holds the rate to kRollbackFrameRate
    // while it lasts; true when that changed the pacing.
    bool SetRollbackCap(bool on);

private:
    FrameRate HeldRate(double display_hz) const;
    void Apply(double display_hz);
    void Log(const char* reason) const;

    FrameRate rate_;
    bool rollback_cap_ = false;
    bool background_ = false;
    bool vsync_;
    SDL_Window* window_;
    void (*configure_game_)(void (*)());
    int background_hz_;
    bool focus_counts_;
    // The game swaps through aurora_paced_video_swap.
    bool game_host_paced_ = false;
    double display_hz_ = -1.0;
    FramePacing pacing_{};
    std::chrono::steady_clock::time_point next_check_{};
};

} // namespace mscharged::runtime
