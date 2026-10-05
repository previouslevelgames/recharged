#pragma once
#include "runtime/frontend_options.h"
#include "runtime/frontend_navigation.h"
#include <thread>

namespace mscharged
{
// Receives the selected original Options commands using the real retained NAV
// owner. Call within the stack's post-base input window, before publication.
// This supplies Back and cursor/music commands, not destination scenes or the
// complete global pointer manager. Input, both owners and music outlive it.
class FrontendOptionsNavigation
{
    FrontendOptions& options_;
    FrontendNavigation& navigation_;
    std::function<void(unsigned)> select_music_;
    FrontendSession::Handle commands_frame_;
    bool back_bound_ = false, busy_ = false, failed_ = false;
    std::thread::id thread_ = std::this_thread::get_id();
    void Check() const;
    FrontendNavigationDispatch Dispatch(const FrontendSession::Handle&,
        const FrontendSession::Handle&,const std::function<FrontendNavigationDispatch()>&);
public:
    FrontendOptionsNavigation(FrontendOptions&,FrontendNavigation&,std::function<void(unsigned)> select_music);
    FrontendOptionsNavigation(const FrontendOptionsNavigation&) = delete;
    FrontendOptionsNavigation& operator=(const FrontendOptionsNavigation&) = delete;
    void ApplyCommands(); // Once per exact source mutation; never replay partial effects.
    FrontendNavigationDispatch Route(const FrontendSession::Handle& menu_shown,
        const FrontendSession::Handle& navigation_shown,const FrontendPointerDesktopSample&);
    FrontendNavigationDispatch Poll(const FrontendSession::Handle& menu_shown,
        const FrontendSession::Handle& navigation_shown,SDL_Window*,bool capture=false);
    bool BackBound() const;
    bool Failed() const;
};
}
