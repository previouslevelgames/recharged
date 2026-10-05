#pragma once
#include "runtime/frontend_visual_options.h"
#include "runtime/frontend_navigation_done.h"

namespace mscharged
{
// Selected Visual15 and actual NAV ownership, using one sampled pointer event:
// Back, five zoom levels, auto/manual, then Done. Native preference persistence
// is explicit; caller retains/polls it after scene removal. Original game saves,
// global pointer/haptic services and gameplay camera application remain pending.
class FrontendVisualNavigation
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    FrontendNavigationDispatch Dispatch(const FrontendSession::Handle&,
        const FrontendSession::Handle&,const std::function<FrontendNavigationDispatch()>&);
public:
    FrontendVisualNavigation(std::shared_ptr<FrontendVisualOptions>,
        std::shared_ptr<FrontendNavigation>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,
        std::shared_ptr<NativePreferences>);
    ~FrontendVisualNavigation();
    FrontendVisualNavigation(const FrontendVisualNavigation&)=delete;
    FrontendVisualNavigation& operator=(const FrontendVisualNavigation&)=delete;
    void ApplyCommands(); // Once per exact source frame; failure effects never replay.
    // Caller already acknowledged both actually composed frames on their owners.
    void Acknowledge(const FrontendSession::Handle& navigation_presented);
    FrontendNavigationDispatch Route(const FrontendSession::Handle& menu_presented,
        const FrontendSession::Handle& navigation_presented,const FrontendPointerDesktopSample&);
    FrontendNavigationDispatch Poll(const FrontendSession::Handle& menu_presented,
        const FrontendSession::Handle& navigation_presented,SDL_Window*,bool capture=false);
    // The supplied source event has no additional desktop admission/query.
    FrontendNavigationDispatch Deliver(const FrontendSession::Handle& menu_presented,
        const FrontendSession::Handle& navigation_presented,const FrontendPointerEvent&);
    FrontendNavigationDoneStatus DoneStatus() const;
    FrontendPointerBounds DoneBounds() const;
    bool Failed() const;
    void Release();
};
}
