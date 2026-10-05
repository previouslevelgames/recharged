#include "runtime/frontend_pointer_display.h"
#include <array>
#include <stdexcept>

namespace mscharged
{
namespace
{
void Require(bool value, const char* message)
{ if (!value) throw std::logic_error(message); }
bool SameBinding(const FrontendPointerBinding& a, const FrontendPointerBinding& b)
{
    return a.instance == b.instance && a.use_rotation == b.use_rotation
        && a.offset_x == b.offset_x && a.offset_y == b.offset_y
        && a.scale_x == b.scale_x && a.scale_y == b.scale_y && a.screen_height == b.screen_height;
}
}
FrontendPointerDisplay::FrontendPointerDisplay(FrontendInput& input, FrontendPointerRegion::Callback callback)
    : input_(input), host_(input), callback_(std::move(callback)) {}
FrontendPointerDisplay::~FrontendPointerDisplay()
{ try { Release(); } catch (...) { std::terminate(); } }
void FrontendPointerDisplay::Acknowledge(const AuroraPresentation& present, FrontendSession::Handle frame,
    FrontendPointerBinding binding)
{
    (void)host_.Current(); // Enforce the owner thread, including before first publication.
    Require(!failed_ && !released_, "Frontend pointer display is unavailable after failure/release");
    Require(present.sequence > sequence_ && present.window_id && bool(frame),
        "Frontend pointer display requires a newer successful presentation and exact frame");
    const auto& size = present.size;
    const FrontendPointerViewport viewport{present.window_id, size.width, size.height,
        size.native_fb_width, size.native_fb_height, present.x, present.y, present.width, present.height};
    const bool replace = !region_ || !current_ || !SameBinding(binding_, binding)
        || frame->visuals != current_->Frame()->visuals || frame->images != current_->Frame()->images;
    // Resolve bounds before any retained listener changes. Publication validation
    // remains in the shared host. On any later failure, stop all further routing.
    try
    {
        (void)MeasureFrontendPointerBounds(frame, binding);
        auto candidate = replace ? std::make_shared<FrontendPointerRegion>(input_, frame, binding, callback_) : region_;
        if (!replace) candidate->Rebind(frame, binding);
        const auto next = host_.Publish(frame, viewport, std::array{candidate});
        region_ = std::move(candidate); current_ = next; binding_ = binding; sequence_ = present.sequence;
        if (replace || suspended_) host_.Reset();
        suspended_ = false;
    }
    catch (...) { failed_ = true; throw; }
}
FrontendPointerPresentationHandle FrontendPointerDisplay::Current() const
{
    (void)host_.Current();
    Require(!failed_ && !released_, "Frontend pointer display is unavailable after failure/release");
    return current_;
}
FrontendPointerBounds FrontendPointerDisplay::Bounds() const
{ Require(bool(Current()) && bool(region_), "Frontend pointer display has no acknowledged region"); return region_->Bounds(); }
FrontendPointerDispatch FrontendPointerDisplay::Route(const FrontendPointerDesktopSample& sample)
{
    Require(bool(Current()) && !suspended_, "Frontend pointer display is suspended or has no acknowledged frame");
    try { return host_.Route(current_, sample); }
    catch (...) { failed_ = true; throw; }
}
FrontendPointerDispatch FrontendPointerDisplay::Poll(SDL_Window* window, bool capture)
{
    Require(window && bool(Current()), "Frontend pointer display needs an acknowledged window/frame");
    Require(SDL_GetWindowID(window) == current_->Viewport().window, "Frontend pointer display received a different window");
    int ww = 0, wh = 0, pw = 0, ph = 0;
    Require(SDL_GetWindowSize(window, &ww, &wh) && SDL_GetWindowSizeInPixels(window, &pw, &ph),
        "Cannot read frontend pointer display extent");
    const auto& v = current_->Viewport();
    if (ww <= 0 || wh <= 0 || pw <= 0 || ph <= 0 || unsigned(ww) != v.window_width
        || unsigned(wh) != v.window_height || unsigned(pw) != v.pixel_width || unsigned(ph) != v.pixel_height)
    {
        if (!suspended_) { host_.Reset(); suspended_ = true; }
        FrontendPointerDispatch inactive; inactive.event.position = {-999, -999}; return inactive;
    }
    if (suspended_) { FrontendPointerDispatch inactive; inactive.event.position = {-999, -999}; return inactive; }
    try { return host_.Poll(current_, window, capture); }
    catch (...) { failed_ = true; throw; }
}
void FrontendPointerDisplay::Release()
{
    host_.Release(); // Also checks the thread on an idempotent release.
    if (released_) return;
    current_.reset(); region_.reset(); callback_ = {}; released_ = true;
}
}
