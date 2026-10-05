#pragma once
#include "runtime/frontend_cameras.h"
#include "runtime/frontend_stack.h"
#include "runtime/interpreter.h"
#include <optional>

namespace mscharged
{
enum class FrontendTransitionState { Idle, Running, SceneQueued, Cancelled, Failed, Released };
struct FrontendTransitionStatus
{
    FrontendTransitionState state = FrontendTransitionState::Idle;
    float wait = 0;
    bool camera_finished = false;
    std::optional<FrontendSceneStack::Token> queued_scene;
    FrontendCameraSelectionHandle camera;
};
// Executes the actual TransitionTitleScreenToMainMenu bytecode in the selected
// original interpreter. Only its camera, wait, Idle and main-scene queue hosts
// are provided. It neither advances CameraMan nor supplies concrete Title/Main
// handlers, music, movies, save services or a successful boot state.
// Camera/library/stack/input/NL/arenas must outlive this owner.
class FrontendTransition
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendTransition(resources::Bytes script, FrontendCameras&, FrontendSceneStack&,
        InterpreterLimits = {});
    ~FrontendTransition();
    FrontendTransition(const FrontendTransition&) = delete;
    FrontendTransition& operator=(const FrontendTransition&) = delete;
    // Requires the exact acknowledged, published title frame and an owned
    // unblended current camera. Original Title's prior Pop can already be queued;
    // this owner retains its source frame through the transition. Main defaults
    // to AwaitingHandler until the caller provides genuine concrete callbacks.
    void Begin(FrontendSceneStack::Token title, const FrontendSession::Handle& expected_title,
        FrontendStackCallbacks main_callbacks = {});
    // Original priority4 BeginFrame advances camera before priority13 FE update.
    // Caller makes that single camera advance, then invokes Update with its FE
    // delta. Source wait48 subtracts this delta immediately after wait44 passes.
    void Update(float delta);
    FrontendTransitionStatus Status() const;
    std::span<const unsigned> Calls() const; // Bounded diagnostic host trace.
    // Cancel/Release detach only this exact camera selection's callback. They
    // stop unissued work; a successfully issued Main queue entry belongs to the
    // stack and is not implicitly popped/withdrawn. No prior camera is restored.
    void Cancel();
    void Release();
};
}
