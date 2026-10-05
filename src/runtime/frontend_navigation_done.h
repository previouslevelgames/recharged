#pragma once
#include "runtime/frontend_navigation.h"

namespace mscharged
{
struct FrontendNavigationDoneStatus
{
    std::array<int,4> pointer_states{};
    unsigned speaker_context=0, hover_feedback_requests=0;
    bool bound=false, pressed=false, failed=false;
};
// The submenu owns the original Done listener; NAV owns its real component.
// Bounds are the original fixed SetDoneButtonBounds rectangle, independent of
// text or image extent. Retains both callback and native NAV/audio owners.
// Input/RNG outlive this object. Speaker/rumble requests remain explicit data.
class FrontendNavigationDone
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using SaveCallback=std::function<void(const FrontendSession::Handle& menu_presented)>;
    FrontendNavigationDone(std::shared_ptr<FrontendNavigation>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,SaveCallback);
    ~FrontendNavigationDone();
    FrontendNavigationDone(const FrontendNavigationDone&)=delete;
    FrontendNavigationDone& operator=(const FrontendNavigationDone&)=delete;
    void Acknowledge(const FrontendSession::Handle& navigation_presented);
    // Invoke once after the submenu's own listeners, inside NAV's presented-input
    // window. This consumes the existing event and never queries action30.
    bool Route(const FrontendSession::Handle& navigation_presented,
        const FrontendSession::Handle& menu_presented,const FrontendPointerEvent&);
    FrontendPointerBounds Bounds() const;
    FrontendNavigationDoneStatus Status() const;
    void Release();
};
}
