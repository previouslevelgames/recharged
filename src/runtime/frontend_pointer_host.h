#pragma once
#include "runtime/frontend_pointer.h"
#include <SDL3/SDL.h>
#include <cstdint>
#include <span>
#include <vector>

namespace mscharged
{
// Actual rendered content in drawable pixels. SDL cursor coordinates use window
// units; both extents are required for HiDPI. This is an acknowledgement from
// the renderer, not an inferred letterbox or a request to change projection.
// The existing FE packet layout uses 640x480. Other dimensions require a caller
// which genuinely renders the corresponding original centered FE projection.
struct FrontendPointerViewport
{
    SDL_WindowID window = 0;
    unsigned window_width = 0, window_height = 0, pixel_width = 0, pixel_height = 0;
    double x = 0, y = 0, width = 0, height = 0;
    unsigned screen_width = 640, screen_height = 480;
    bool widescreen = false; // Original feDPD chooses 854 instead of screen_width.
    bool operator==(const FrontendPointerViewport&) const = default;
};
class FrontendPointerHost;
class FrontendPointerPresentation
{
    friend class FrontendPointerHost;
    FrontendPointerPresentation() = default;
    FrontendSession::Handle frame_;
    FrontendPointerViewport viewport_;
    std::uint64_t generation_ = 0;
public:
    const FrontendSession::Handle& Frame() const { return frame_; }
    const FrontendPointerViewport& Viewport() const { return viewport_; }
    std::uint64_t Generation() const { return generation_; }
};
using FrontendPointerPresentationHandle = std::shared_ptr<const FrontendPointerPresentation>;
struct FrontendPointerDesktopSample
{
    SDL_WindowID window = 0;
    unsigned window_width = 0, window_height = 0, pixel_width = 0, pixel_height = 0;
    std::uint64_t sequence = 0; // Strictly increasing, including suppressed samples.
    std::uint64_t device = 0; // Explicit aggregate desktop mouse identity; 0 means disconnected.
    float x = 0, y = 0; // SDL window units, origin at top left.
    bool connected = false, focused = false, captured = false, primary_down = false;
};
struct FrontendPointerDispatch
{
    FrontendPointerEvent event;
    bool active = false; // Host policy admitted this sample (not Wii DPD validity).
    unsigned listeners = 0;
};
// One absolute desktop mouse routed to one original pointer index. Keeps the
// exact rendered frame and listeners alive. Original action 30 is queried from
// FrontendInput; caller updates that input once before each Route/Poll. Mouse
// down edges are an explicit additional desktop binding. Source Options events
// leave Released/Unidentified false; losing focus never invents a release.
//
// Caller acknowledges only successfully presented frames, after drain when
// replacing resources. Rebind listener frames before Publish; all must match.
// No global FEPointerManager, concrete menu handlers, Wii DPD/speaker/rumble.
class FrontendPointerHost
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit FrontendPointerHost(FrontendInput&, unsigned index = 0);
    ~FrontendPointerHost();
    FrontendPointerHost(const FrontendPointerHost&) = delete;
    FrontendPointerHost& operator=(const FrontendPointerHost&) = delete;
    FrontendPointerPresentationHandle Publish(FrontendSession::Handle, FrontendPointerViewport,
        std::span<const std::shared_ptr<FrontendPointerRegion>>);
    FrontendPointerPresentationHandle Current() const;
    FrontendPointerDispatch Route(const FrontendPointerPresentationHandle&, const FrontendPointerDesktopSample&);
    // Reads SDL's aggregate absolute cursor without pumping/consuming events.
    // Window identity, live extents and mouse focus must match publication.
    FrontendPointerDispatch Poll(const FrontendPointerPresentationHandle&, SDL_Window*, bool mouse_capture = false);
    bool Failed() const;
    // Callback failures can have external side effects and earlier listeners
    // can already be updated. Further routing/publication fails until Reset.
    // Reset clears original listener histories, preserves enabled flags, and
    // requires neutral controls again. Destroy/release before FrontendInput.
    void Reset();
    void Release();
};
}
