#pragma once
#include "runtime/frontend_audio.h"
#include "runtime/frontend_stack_visual.h"
#include "runtime/frontend_pointer_host.h"
#include "runtime/frontend_visual_settings.h"
#include "runtime/native_preferences.h"
namespace mscharged
{
enum class FrontendVisualOptionsCommandKind
{
    HideNavigation,BindBack,BindDone,PointerWaiting,PointerCursor,ShowBackAndDone,
    DoneText,DoneDown,PushOptions,HoverRumble,ResetNavigation
};
struct FrontendVisualOptionsCommand{FrontendVisualOptionsCommandKind kind;unsigned argument=0;};
struct FrontendVisualOptionsStatus
{
    int state=0;
    bool initialized=false,failed=false,native_save_admitted=false;
    // Original constructor's !auto and int(4*zoom); source Back restores these
    // indices, including its original quantization of a non-quarter input.
    std::array<int,2> settings{},backup{};
    // Five level listeners, then auto/manual listeners; each has four pointers.
    std::array<std::array<int,4>,7> pointer_states{};
    std::vector<FrontendVisualOptionsCommand> commands;
};
// Selected original scene15 mode0 visual/listener flow over retained resources.
// Input lock returns BEFORE BaseSceneHandler::Update, unlike Main/Options1/13.
// NAV/DPD/haptics/manager publication/gameplay camera remain explicit requests.
// Source Save() stays unavailable; SaveNativePreferences is an explicit separate
// scope using real113 persistence, never original NormalSaveLoaded/readiness.
class FrontendVisualOptions : public FrontendStackVisual
{
    struct Implementation;std::unique_ptr<Implementation> impl_;
    void ApplyPending();
    void AfterBaseUpdate(FrontendHandler::UpdateProof&&);
    std::shared_ptr<FrontendSession> StackSession() const override;
    std::shared_ptr<FrontendHandler> StackHandler() const override;
    unsigned StackScene() const override;
    bool CanUpdateStack() const override;
    void AttachStack() override;
    void UpdateStack(FrontendHandler::UpdateProof&&,const FrontendSession::Handle&,
        const std::function<void()>&) override;
    void ReleaseStack() override;
public:
    FrontendVisualOptions(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,FrontendVisualSettings::Handle,unsigned& caller_seed,unsigned controller=0);
    FrontendVisualOptions(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,FrontendVisualSettings::Handle,unsigned& caller_seed,
        std::shared_ptr<FrontendHandler>,unsigned controller=0);
    ~FrontendVisualOptions();
    FrontendVisualOptions(const FrontendVisualOptions&)=delete;
    FrontendVisualOptions& operator=(const FrontendVisualOptions&)=delete;
    FrontendSession::Handle Current()const;
    FrontendVisualOptionsStatus Status()const;
    void AdvanceVisual(const FrontendSession::Handle&,float delta);
    void Acknowledge(const FrontendSession::Handle&,FrontendPointerViewport);
    std::array<FrontendPointerBounds,7> Bounds()const;
    void DeliverPointer(const FrontendSession::Handle&,const FrontendPointerEvent&);
    FrontendPointerDispatch Route(const FrontendPointerDesktopSample&);
    FrontendPointerDispatch Poll(SDL_Window*,bool capture=false);
    void NotifyBackButton(const FrontendSession::Handle&); // Actual NAV completion only.
    void Save(const FrontendSession::Handle&); // Full original save service unavailable.
    // Caller retains/polls the service through completion, beyond scene removal.
    // Audio/default fields survive. Requires a completed native preferences load.
    void SaveNativePreferences(const FrontendSession::Handle&,std::shared_ptr<NativePreferences>);
    void Release();
};
}
