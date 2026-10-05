#pragma once
#include "runtime/frontend_transition.h"
#include <string_view>

namespace mscharged
{
enum class FrontendMenuTransitionKind { MainDeparture, OptionsToMain };
enum class FrontendMenuTransitionState
{ Idle, Running, AwaitingService, AwaitingNavigation, SceneQueued, Cancelled, Failed, Released };
enum class FrontendMenuTransitionService { SaveGate, StadiumEffect, Navigation, MusicNavigation };
struct FrontendMenuTransitionServices
{
    // Original SaveEnabled && InOperation, or no authoritative state yet.
    std::function<std::optional<bool>()> save_busy;
    // Return true only after actual admission. False means not admitted yet;
    // callbacks must not repeatedly apply side effects while returning false.
    std::function<bool(unsigned)> trigger_stadium_effect;
    std::function<bool(std::string_view)> start_navigation;
    // Source host42 includes music1, HideButtons and all four waiting cursors.
    std::function<bool()> start_music_navigation;
};
struct FrontendMenuTransitionStatus
{
    FrontendMenuTransitionState state = FrontendMenuTransitionState::Idle;
    std::optional<FrontendMenuTransitionService> pending;
    std::optional<FrontendSceneStack::Token> queued_scene;
    FrontendCameraSelectionHandle camera;
    bool animation_finished = false, blend_finished = false;
};
// Runs original Main departure / Options return bytecode and its camera hosts.
// Unbound save/effect/NAV/music services pause at their exact instruction.
// This neither implements ApplyItem/FEBackButton nor supplies scene readiness.
// Callers perform those genuine source operations before Begin, and advance
// CameraMan once before Update. Input/stack/cameras/NL outlive this owner.
// Camera pops require an owned underlying frontend camera; a borrowed gameplay
// camera is outside this selected Main/Options flow.
// Permanent Main resources, when present, stay retained across the source Pop
// and both destination FEN reads. Ordinary scene requests remain independent.
class FrontendMenuTransition
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendMenuTransition(resources::Bytes,FrontendCameras&,FrontendSceneStack&,InterpreterLimits = {});
    ~FrontendMenuTransition();
    FrontendMenuTransition(const FrontendMenuTransition&) = delete;
    FrontendMenuTransition& operator=(const FrontendMenuTransition&) = delete;
    void Begin(FrontendMenuTransitionKind,FrontendSceneStack::Token,
        const FrontendSession::Handle& published_source,FrontendMenuTransitionServices = {},
        FrontendStackCallbacks destination = {});
    // Original NAV's pending update calls the selected gNextFETransition.
    // Call only from that genuine completion, not a guessed elapsed timer.
    void NavigationTransition(std::string_view function_name);
    void Update(float delta);
    FrontendMenuTransitionStatus Status() const;
    std::span<const unsigned> Calls() const;
    void Cancel();
    void Release();
};
}
