#include "runtime/host_frame_pacing.h"

#include "platform/session_log.h"

#include <SDL3/SDL_video.h>
#include <aurora/video.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace mscharged::runtime {

double WindowRefreshRate(SDL_Window* window)
{
    const SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : 0;
    const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
    if (!mode) return 0.0;
    // The exact rate where the platform reports it (59.94 rather than 60).
    if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
        return double(mode->refresh_rate_numerator) / double(mode->refresh_rate_denominator);
    return KnownRefreshRate(mode->refresh_rate) ? double(mode->refresh_rate) : 0.0;
}

bool WindowInBackground(SDL_Window* window, bool focus_counts)
{
    const SDL_WindowFlags flags = window ? SDL_GetWindowFlags(window) : 0;
    if (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) return true;
    return focus_counts && window && !(flags & SDL_WINDOW_INPUT_FOCUS);
}

HostFramePacing::HostFramePacing(FrameRate rate, bool vsync, SDL_Window* window, void (*configure_game)(void (*)()),
                                 int background_hz, bool focus_counts)
    : rate_(rate), vsync_(vsync), window_(window), configure_game_(configure_game), background_hz_(background_hz),
      focus_counts_(focus_counts)
{
    if (!configure_game_) throw std::invalid_argument("Host frame pacing needs the game's frame pacing entry");
    if (rate.kind == FrameRate::Kind::Original && background_hz_ == 0)
        throw std::invalid_argument("Host frame pacing at the original frame rate needs a background frame rate");
    background_ = background_hz_ != 0 && WindowInBackground(window_, focus_counts_);
    Apply(WindowRefreshRate(window_));
    next_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}

void HostFramePacing::Update(std::chrono::steady_clock::time_point now)
{
    if (background_hz_ != 0)
    {
        const bool background = WindowInBackground(window_, focus_counts_);
        if (background != background_)
        {
            background_ = background;
            const FramePacing before = pacing_;
            Apply(display_hz_);
            if (before.interval_ns != pacing_.interval_ns || before.host_paced != pacing_.host_paced
                || before.vsync_paces != pacing_.vsync_paces)
                Log(background ? "the window is in the background" : "the window is back in front");
        }
    }
    if (now < next_check_) return;
    next_check_ = now + std::chrono::seconds(1);
    const double display = WindowRefreshRate(window_);
    if (std::fabs(display - display_hz_) < 0.01) return;
    const bool paced = pacing_.host_paced;
    Apply(display);
    if (paced || pacing_.host_paced) Log("the window's display changed");
}

std::string HostFramePacing::Description() const
{
    const FrameRate held = rollback_cap_ ? CapFrameRate(rate_, kRollbackFrameRate, display_hz_) : rate_;
    const FrameRate rate = HeldRate(display_hz_);
    std::string text = DescribeFramePacing(rate, pacing_, display_hz_);
    if (rate != held) text += " while the window is in the background";
    else if (rate != rate_) text += " in this network match with rollback";
    if (rate != rate_) text += " (" + FrameRateText(rate_) + " otherwise)";
    return text;
}

bool HostFramePacing::SetRollbackCap(bool on)
{
    if (on == rollback_cap_) return false;
    rollback_cap_ = on;
    const FramePacing before = pacing_;
    Apply(display_hz_);
    return before.interval_ns != pacing_.interval_ns || before.vsync_paces != pacing_.vsync_paces;
}

FrameRate HostFramePacing::HeldRate(double display_hz) const
{
    FrameRate rate = rollback_cap_ ? CapFrameRate(rate_, kRollbackFrameRate, display_hz) : rate_;
    if (background_) rate = BackgroundFrameRate(rate, background_hz_, display_hz);
    return rate;
}

void HostFramePacing::Apply(double display_hz)
{
    display_hz_ = display_hz;
    pacing_ = DecideFramePacing(HeldRate(display_hz), display_hz, vsync_);
    if (!pacing_.host_paced)
    {
        // Back to the game's own VI pacing: its swap first, as the paced swap
        // requires Aurora's host pacing.
        if (!game_host_paced_) return;
        configure_game_(nullptr);
        aurora_configure_video_pacing(nullptr);
        game_host_paced_ = false;
        return;
    }
    const AuroraVideoPacing pacing{pacing_.interval_ns};
    aurora_configure_video_pacing(&pacing);
    if (game_host_paced_) return;
    configure_game_(aurora_paced_video_swap);
    game_host_paced_ = true;
}

void HostFramePacing::Log(const char* reason) const
{
    const auto line = std::string("Frame rate: ") + reason + "; " + Description();
    platform::SessionLogLine(line);
    std::fprintf(stderr, "%s\n", line.c_str());
}

} // namespace mscharged::runtime
