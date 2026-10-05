#pragma once
#include "runtime/frontend_audio_options.h"
#include "runtime/frontend_navigation_done.h"

namespace mscharged
{
// Actual Audio14 + NAV ownership. Requires exact shared permanent Main visual
// and image owners; no second input query, global pointer manager or game save.
// NativePreferences is deliberately explicit; caller polls it and gates scene
// departure on its genuine pending/error state after this owner is removed.
class FrontendAudioNavigation
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendAudioNavigation(std::shared_ptr<FrontendAudioOptions>,
        std::shared_ptr<FrontendNavigation>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,
        std::shared_ptr<NativePreferences>);
    ~FrontendAudioNavigation();
    FrontendAudioNavigation(const FrontendAudioNavigation&)=delete;
    FrontendAudioNavigation& operator=(const FrontendAudioNavigation&)=delete;
    void ApplyCommands(); // At most once per exact source frame; no failure replay.
    void Acknowledge(const FrontendSession::Handle& navigation_presented);
    // Explicit already-produced event; active remains false because this does
    // not make a desktop-focus/capture admission decision.
    FrontendNavigationDispatch Deliver(const FrontendSession::Handle& menu_presented,
        const FrontendSession::Handle& navigation_presented,const FrontendPointerEvent&);
    FrontendNavigationDispatch Route(const FrontendSession::Handle& menu_presented,
        const FrontendSession::Handle& navigation_presented,const FrontendPointerDesktopSample&);
    FrontendNavigationDispatch Poll(const FrontendSession::Handle& menu_presented,
        const FrontendSession::Handle& navigation_presented,SDL_Window*,bool capture=false);
    FrontendNavigationDoneStatus DoneStatus() const;
    FrontendPointerBounds DoneBounds() const;
    bool Failed() const;
    void Release();
};
}
