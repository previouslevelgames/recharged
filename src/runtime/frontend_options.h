#pragma once
#include "runtime/frontend_audio.h"
#include "runtime/frontend_pointer_host.h"
#include "runtime/frontend_stack_visual.h"
#include <optional>

namespace mscharged
{
enum class FrontendOptionsCommandKind
{
    HideNavigation, BindNavigationBack, ShowNavigationBack,
    PointerWaiting, PointerCursor, SelectMusic,
    PushScene, TransitionOptionsToMainMenu, PopScene
};
struct FrontendOptionsCommand
{
    FrontendOptionsCommandKind kind;
    unsigned argument = 0;
};
struct FrontendOptionsTransition
{
    // PushScene: original scene14/15/23, SCREEN_NOTHING, queued=true.
    // TransitionOptionsToMainMenu: original script request followed by Pop.
    FrontendOptionsCommandKind kind;
    int scene = -1;
    FrontendSession::Handle source;
};
struct FrontendOptionsStatus
{
    int state = 0, next_scene = -1;
    bool initialized = false, failed = false;
    std::array<std::array<int,4>,3> pointer_states{};
    std::optional<FrontendOptionsTransition> transition;
    // Commands emitted by the most recent mutation, in original source order.
    // These are explicit dependencies, NOT reports that NAV/DPD/music ran.
    std::vector<FrontendOptionsCommand> commands;
};
// Selected original Options visual callbacks and in/out state machine over a
// retained scene. This does not link the full Options/NAV/manager lifecycle or
// manufacture state6. NAV/global pointers/music remain typed external commands.
// Input and the caller RNG must outlive this owner; audio is retained. Release
// before input/SDL/NL teardown. An input frame must be genuinely presented.
class FrontendOptions : public FrontendStackVisual
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    void ApplyPending();
    void AfterBaseUpdate(FrontendHandler::UpdateProof&&);
    std::shared_ptr<FrontendSession> StackSession() const override;
    std::shared_ptr<FrontendHandler> StackHandler() const override;
    unsigned StackScene() const override;
    void AttachStack() override;
    void UpdateStack(FrontendHandler::UpdateProof&&, const FrontendSession::Handle&,
                     const std::function<void()>&) override;
    void ReleaseStack() override;
public:
    FrontendOptions(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,unsigned controller=0);
    FrontendOptions(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,std::shared_ptr<FrontendHandler>,
        unsigned controller);
    ~FrontendOptions();
    FrontendOptions(const FrontendOptions&)=delete;
    FrontendOptions& operator=(const FrontendOptions&)=delete;
    FrontendSession::Handle Current() const;
    FrontendOptionsStatus Status() const;
    std::array<FrontendPointerBounds,3> Bounds() const;
    void Acknowledge(const FrontendSession::Handle&,FrontendPointerViewport);
    FrontendPointerDispatch Route(const FrontendPointerDesktopSample&);
    FrontendPointerDispatch Poll(SDL_Window*,bool capture=false);
    // Explicit source-listener fixture/host route; permits all four indices to
    // qualify original callback history. Desktop routing uses controller only.
    void DeliverPointer(const FrontendSession::Handle&,const FrontendPointerEvent&);
    void AdvanceVisual(const FrontendSession::Handle&,float delta);
    // Caller must supply a genuinely completed FEBackButton action. This is not
    // a keyboard mapping or an implementation of NAV's missing back service.
    void NotifyBackButton(const FrontendSession::Handle&);
    void Release();
};
}
