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

HostFramePacing::HostFramePacing(FrameRate rate, bool vsync, SDL_Window* window)
    : rate_(rate), vsync_(vsync), window_(window)
{
    if (rate.kind == FrameRate::Kind::Original)
        throw std::invalid_argument("Host frame pacing is for a frame rate above the original");
    Apply(WindowRefreshRate(window_));
    next_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}

void HostFramePacing::Update(std::chrono::steady_clock::time_point now)
{
    if (now < next_check_) return;
    next_check_ = now + std::chrono::seconds(1);
    const double display = WindowRefreshRate(window_);
    if (std::fabs(display - display_hz_) < 0.01) return;
    Apply(display);
    const auto line = "Frame rate: the window's display changed; " + Description();
    platform::SessionLogLine(line);
    std::fprintf(stderr, "%s\n", line.c_str());
}

std::string HostFramePacing::Description() const
{
    return DescribeFramePacing(rate_, pacing_, display_hz_);
}

void HostFramePacing::Apply(double display_hz)
{
    display_hz_ = display_hz;
    pacing_ = DecideFramePacing(rate_, display_hz, vsync_);
    const AuroraVideoPacing pacing{pacing_.interval_ns};
    aurora_configure_video_pacing(&pacing);
}

} // namespace mscharged::runtime
