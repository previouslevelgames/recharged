#include "runtime/frontend_pointer_host.h"
#include "Game/FE/FrontendPointerHostSteps.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool value, const char* message)
{ if (!value) throw std::logic_error(message); }
void Validate(const FrontendPointerViewport& v)
{
    constexpr unsigned limit = 65535;
    Require(v.window && v.window_width && v.window_height && v.pixel_width && v.pixel_height,
        "Frontend pointer requires a live window and drawable extent");
    for (auto value : {v.window_width, v.window_height, v.pixel_width, v.pixel_height})
        Require(value <= limit, "Frontend pointer window extent exceeds supported range");
    for (double value : {v.x, v.y, v.width, v.height})
        Require(std::isfinite(value), "Frontend pointer viewport must be finite");
    Require(v.x >= 0 && v.y >= 0 && v.width > 0 && v.height > 0
        && v.x + v.width <= v.pixel_width && v.y + v.height <= v.pixel_height,
        "Frontend pointer content rectangle is outside its drawable");
    Require(v.screen_width >= 40 && v.screen_height >= 10
        && v.screen_width <= limit && v.screen_height <= limit,
        "Frontend pointer logical extent cannot support original margins");
}
struct Busy
{
    bool& value;
    explicit Busy(bool& v) : value(v) { value = true; }
    ~Busy() { value = false; }
};
struct Event
{
    int mIndex = -1;
    nlVector2 mPosition = {-9999.9f, -9999.9f};
    bool mPressed = false, mReleased = false, mUnidentified0E = false;
};
}
struct FrontendPointerHost::Implementation
{
    FrontendInput& input;
    unsigned index;
    std::thread::id thread = std::this_thread::get_id();
    bool live = true, busy = false, failed = false, blocked = true, previous_mouse = false, previous_accept = false;
    std::uint64_t generation = 0, sequence = 0, device = 0, sdl_device = 0;
    std::vector<SDL_MouseID> mice;
    FrontendPointerPresentationHandle current;
    std::vector<std::shared_ptr<FrontendPointerRegion>> listeners;
    Implementation(FrontendInput& in, unsigned i) : input(in), index(i)
    { Require(i < 4, "Frontend pointer index exceeds four original pointers"); (void)input.InputLocked(); }
    void Ready(bool recover = false) const
    {
        Require(thread == std::this_thread::get_id(), "Frontend pointer host requires its owner thread");
        Require(live, "Frontend pointer host has been released");
        Require(recover || !failed, "Frontend pointer host needs Reset after callback failure");
    }
    void Mutable(bool recover = false) const
    { Ready(recover); Require(!busy, "Frontend pointer host cannot mutate during dispatch or destruction"); }
    void CheckListeners() const
    {
        Require(bool(current), "Frontend pointer host has no rendered presentation");
        for (const auto& region : listeners)
            Require(region && region->Current() == current->Frame(), "Frontend pointer listener targets a different rendered frame");
    }
};
FrontendPointerHost::FrontendPointerHost(FrontendInput& input, unsigned index)
    : impl_(std::make_unique<Implementation>(input, index)) {}
FrontendPointerHost::~FrontendPointerHost()
{
    if (impl_->busy || impl_->thread != std::this_thread::get_id()) std::terminate();
    // Listener callback captures can execute destructors when the last owner
    // disappears. Keep reentrant mutation rejected during that destruction.
    impl_->busy = true;
    impl_->listeners.clear(); impl_->current.reset();
}
FrontendPointerPresentationHandle FrontendPointerHost::Publish(FrontendSession::Handle frame,
    FrontendPointerViewport viewport, std::span<const std::shared_ptr<FrontendPointerRegion>> listeners)
{
    auto& s = *impl_; s.Mutable(); Validate(viewport);
    Require(bool(frame), "Frontend pointer publication needs a retained rendered frame");
    Require(listeners.size() <= 128, "Frontend pointer publication exceeds 128 listeners");
    Require(s.generation != std::numeric_limits<std::uint64_t>::max(), "Frontend pointer generation exhausted");
    for (std::size_t i = 0; i < listeners.size(); ++i)
    {
        Require(listeners[i] && listeners[i]->Current() == frame, "Frontend pointer publication has a stale listener");
        Require(std::find(listeners.begin(), listeners.begin() + i, listeners[i]) == listeners.begin() + i,
            "Frontend pointer publication has a duplicate listener");
    }
    // All allocations precede publication. A failed candidate retains the exact
    // old token, listener ownership and input gates.
    std::vector<std::shared_ptr<FrontendPointerRegion>> next(listeners.begin(), listeners.end());
    auto presentation = std::shared_ptr<FrontendPointerPresentation>(new FrontendPointerPresentation);
    presentation->frame_ = std::move(frame); presentation->viewport_ = viewport;
    presentation->generation_ = s.generation + 1;
    Busy guard(s.busy);
    if (!s.current || s.current->Viewport() != viewport)
    { s.blocked = true; s.previous_mouse = s.previous_accept = false; }
    s.listeners.swap(next); s.current = presentation; ++s.generation;
    // Destruction of removed listeners stays inside the mutation guard.
    next.clear();
    return presentation;
}
FrontendPointerPresentationHandle FrontendPointerHost::Current() const
{ impl_->Ready(true); return impl_->current; }
bool FrontendPointerHost::Failed() const { impl_->Ready(true); return impl_->failed; }
FrontendPointerDispatch FrontendPointerHost::Route(const FrontendPointerPresentationHandle& presentation,
    const FrontendPointerDesktopSample& sample)
{
    auto& s = *impl_; s.Mutable();
    Require(presentation && presentation == s.current, "Frontend pointer sample targets a stale rendered generation");
    const auto& v = presentation->Viewport();
    Require(sample.window == v.window && sample.window_width == v.window_width && sample.window_height == v.window_height
        && sample.pixel_width == v.pixel_width && sample.pixel_height == v.pixel_height,
        "Frontend pointer sample window or extent differs from the rendered viewport");
    Require(sample.sequence && sample.sequence > s.sequence, "Frontend pointer sample sequence is stale");
    Require(std::isfinite(sample.x) && std::isfinite(sample.y)
        && std::abs(double(sample.x)) <= 1e9 && std::abs(double(sample.y)) <= 1e9,
        "Frontend pointer desktop position is outside supported range");
    Require(sample.connected == (sample.device != 0), "Frontend pointer sample has an inconsistent device identity");
    s.CheckListeners();
    Busy guard(s.busy);
    const double px = double(sample.x) * v.pixel_width / v.window_width;
    const double py = double(sample.y) * v.pixel_height / v.window_height;
    // Desktop outside/visibility/capture are host policies. Unlike a temporarily
    // invalid Wii DPD sample, an outside mouse must not reuse a clickable edge.
    bool active = sample.connected && sample.focused && !sample.captured
        && px >= v.x && px < v.x + v.width && py >= v.y && py < v.y + v.height;
    bool blocked = s.blocked || sample.device != s.device || !active;
    bool accept = s.input.Connected(s.index)
        && s.input.Button(FrontendAction::Accept, FrontendButtonQuery::Held, int(s.index));
    if (blocked)
    {
        if (active && !sample.primary_down && !accept) blocked = false;
        active = false; // The neutral observation itself does not activate.
    }
    nlVector2 position = {-999.0f, -999.0f};
    if (active)
    {
        // Convert the explicitly rendered viewport to the source normalized
        // axes (DPD X points left). Only this normalization is desktop policy.
        position.x = float(1.0 - 2.0 * (px - v.x) / v.width);
        position.y = float(1.0 - 2.0 * (py - v.y) / v.height);
        const int width = int(v.widescreen ? 854 : v.screen_width);
        FrontendDPDToAssetPosition(position, width, int(v.screen_height));
        position = FrontendClampPointerPosition(position, width, int(v.screen_height));
    }
    struct Source
    {
        Implementation& state;
        nlVector2 position;
        bool active, mouse_edge;
        nlVector2 Position(int) { return position; }
        bool JustPressed(int index)
        {
            // Preserve the source position-before-action 30 query. Filtering of
            // captured/held input and the extra mouse edge are desktop policy.
            const bool press = state.input.Button(FrontendAction::Accept, FrontendButtonQuery::Pressed, index);
            return active && (mouse_edge || (press && !state.previous_accept));
        }
    } source{s, position, active, sample.primary_down && !s.previous_mouse};
    Event event; FrontendOptionsPointerEvent(event, int(s.index), source);
    FrontendPointerDispatch result;
    result.event = {unsigned(event.mIndex), {event.mPosition.x, event.mPosition.y},
        event.mPressed, event.mReleased, event.mUnidentified0E};
    result.active = active;
    try
    {
        // Full preflight above prevents stale later listeners from causing a
        // partially delivered event. A callback can still mutate external data;
        // detect that before subsequent delivery and poison until explicit Reset.
        for (const auto& region : s.listeners)
        { region->Deliver(presentation->Frame(), result.event); ++result.listeners; }
        s.CheckListeners();
    }
    catch (...) { s.failed = true; throw; }
    s.blocked = blocked; s.previous_mouse = sample.primary_down; s.previous_accept = accept;
    s.sequence = sample.sequence; s.device = sample.device;
    return result;
}
FrontendPointerDispatch FrontendPointerHost::Poll(const FrontendPointerPresentationHandle& presentation,
    SDL_Window* window, bool capture)
{
    auto& s = *impl_; s.Mutable();
    Require(SDL_IsMainThread(), "Frontend pointer SDL polling requires the main thread");
    Require(window && (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO), "Frontend pointer polling requires initialized SDL video and a window");
    Require(presentation && presentation == s.current, "Frontend pointer polling targets a stale rendered generation");
    Require(SDL_GetWindowID(window) == presentation->Viewport().window, "Frontend pointer polling received a different SDL window");
    Require(!SDL_GetWindowRelativeMouseMode(window), "Frontend pointer requires absolute SDL mouse coordinates");
    Require(s.sequence != std::numeric_limits<std::uint64_t>::max(), "Frontend pointer sample sequence exhausted");
    int ww = 0, wh = 0, pw = 0, ph = 0;
    Require(SDL_GetWindowSize(window, &ww, &wh) && SDL_GetWindowSizeInPixels(window, &pw, &ph)
        && ww > 0 && wh > 0 && pw > 0 && ph > 0, "Cannot read frontend pointer SDL window extent");
    int count = 0;
    std::unique_ptr<SDL_MouseID, decltype(&SDL_free)> ids(SDL_GetMice(&count), SDL_free);
    Require(bool(ids) && count >= 0 && count <= 1024, "Cannot enumerate SDL mouse devices");
    std::vector<SDL_MouseID> mice(ids.get(), ids.get() + count);
    std::sort(mice.begin(), mice.end());
    if (mice != s.mice)
    {
        Require(s.sdl_device != std::numeric_limits<std::uint64_t>::max(), "Frontend SDL mouse identity exhausted");
        s.mice.swap(mice); ++s.sdl_device;
    }
    const bool connected = SDL_HasMouse();
    if (connected && !s.sdl_device) ++s.sdl_device; // Platforms can expose only an aggregate cursor.
    FrontendPointerDesktopSample sample;
    sample.window = SDL_GetWindowID(window); sample.window_width = unsigned(ww); sample.window_height = unsigned(wh);
    sample.pixel_width = unsigned(pw); sample.pixel_height = unsigned(ph); sample.sequence = s.sequence + 1;
    sample.device = connected ? s.sdl_device : 0; sample.connected = connected;
    sample.primary_down = (SDL_GetMouseState(&sample.x, &sample.y) & SDL_BUTTON_LMASK) != 0;
    const auto flags = SDL_GetWindowFlags(window);
    sample.focused = SDL_GetMouseFocus() == window && (flags & SDL_WINDOW_INPUT_FOCUS)
        && !(flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
    sample.captured = capture;
    return Route(presentation, sample);
}
void FrontendPointerHost::Reset()
{
    auto& s = *impl_; s.Mutable(true); s.CheckListeners();
    std::vector<bool> enabled; enabled.reserve(s.listeners.size());
    for (const auto& listener : s.listeners) enabled.push_back(listener->Enabled());
    Busy guard(s.busy);
    for (std::size_t i = 0; i < s.listeners.size(); ++i)
    {
        s.listeners[i]->Disable();
        if (enabled[i]) s.listeners[i]->Enable();
    }
    s.blocked = true; s.previous_mouse = s.previous_accept = false; s.device = 0; s.failed = false;
}
void FrontendPointerHost::Release()
{
    auto& s = *impl_;
    Require(s.thread == std::this_thread::get_id(), "Frontend pointer host released from a different thread");
    if (!s.live) return;
    s.Mutable(true); Busy guard(s.busy);
    s.listeners.clear(); s.current.reset(); s.live = false;
}
}
