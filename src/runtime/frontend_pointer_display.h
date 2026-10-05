#pragma once
#include "runtime/frontend_pointer_host.h"
#include <aurora/aurora.h>

namespace mscharged
{
// Bind a checked original region to the exact frame which the caller has sent
// and drained. Aurora's successful presentation supplies the actual rectangle;
// an acquired, submitted, discarded or resized frame alone cannot acknowledge it.
// All operations/destruction use the input's thread, before SDL/Aurora shutdown.
// This supplies pointer events, not a concrete game's menu callback or readiness.
class FrontendPointerDisplay
{
    FrontendInput& input_;
    FrontendPointerHost host_;
    FrontendPointerRegion::Callback callback_;
    std::shared_ptr<FrontendPointerRegion> region_;
    FrontendPointerPresentationHandle current_;
    FrontendPointerBinding binding_{};
    std::uint64_t sequence_ = 0;
    bool failed_ = false, released_ = false, suspended_ = false;
public:
    explicit FrontendPointerDisplay(FrontendInput&, FrontendPointerRegion::Callback = {});
    ~FrontendPointerDisplay();
    FrontendPointerDisplay(const FrontendPointerDisplay&) = delete;
    FrontendPointerDisplay& operator=(const FrontendPointerDisplay&) = delete;
    // Snapshot must be captured after this frame's successful present and drain.
    // New scene resource owners reset listener history and require neutral input.
    // Animated snapshots of the same owners retain original listener history.
    // A failure stops routing until Release; no partially rebound dispatch occurs.
    void Acknowledge(const AuroraPresentation&, FrontendSession::Handle, FrontendPointerBinding);
    FrontendPointerPresentationHandle Current() const;
    FrontendPointerBounds Bounds() const;
    // Explicit aggregate snapshot provider; the same exact presentation and
    // source event rules apply. Caller supplies genuine acquisition/capture state.
    FrontendPointerDispatch Route(const FrontendPointerDesktopSample&);
    // A resize/minimize suspends routing until a matching presented frame exists.
    // Losing matching geometry requires neutral input before the next activation.
    FrontendPointerDispatch Poll(SDL_Window*, bool mouse_capture = false);
    void Release();
};
}
