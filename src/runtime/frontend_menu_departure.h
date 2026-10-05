#pragma once
#include "runtime/frontend_main_menu.h"
#include "runtime/frontend_navigation.h"
#include "runtime/frontend_menu_transition.h"

namespace mscharged
{
// Apply only the genuine pending Main item6 selection. All supplied owners are
// live; the exact displayed source is retained by the script before its queued
// Pop is processed. Call after the stack's controlled input update, while idle.
// Missing downstream save/effect/navigation services remain script wait points.
// Audio is shared beyond the screen and owns the departure cue's auto-release.
void BeginMainOptions(FrontendMainMenu&,FrontendSceneStack&,FrontendSceneStack::Token,
    FrontendNavigation&,FrontendAudio&,unsigned& caller_seed,FrontendMenuTransition&,
    FrontendMenuTransitionServices = {});
}
