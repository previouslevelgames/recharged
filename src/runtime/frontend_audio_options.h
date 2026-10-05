#pragma once
#include "runtime/frontend_audio.h"
#include "runtime/audio_volume.h"
#include "runtime/frontend_stack_visual.h"
#include "runtime/native_preferences.h"
#include "runtime/frontend_pointer_host.h"
namespace mscharged
{
enum class FrontendAudioOptionsCommandKind { HideNavigation,BindBack,BindDone,PointerWaiting,PointerCursor,ShowBackAndDone,DoneText,PushOptions,HoverRumble,DoneDown,ResetNavigation };
struct FrontendAudioOptionsCommand {FrontendAudioOptionsCommandKind kind;unsigned argument=0;};
struct FrontendAudioOptionsStatus
{
    int state=0;
    bool initialized=false,failed=false,native_save_admitted=false;
    std::array<int,3> settings{},backup{};
    std::array<std::array<int,4>,6> pointer_states{};
    std::vector<FrontendAudioOptionsCommand> commands;
};
// Original scene14's mode0 visuals/volume listeners over a retained OPTIONS_IN
// session. Requires explicit actual category authority shared with resident
// output; caller also binds music to it and advances its single frame clock.
// This is not full SceneCreated, GameInfo, SaveLoad, NAV or overlay readiness.
// Original Save is unavailable. The explicitly named native-preferences path
// admits actual host I/O before publication and never completes original SaveLoad.
class FrontendAudioOptions : public FrontendStackVisual
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
    FrontendAudioOptions(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,AudioCategoryVolumes::Handle,unsigned& caller_seed,unsigned controller=0);
    FrontendAudioOptions(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,AudioCategoryVolumes::Handle,unsigned& caller_seed,
        std::shared_ptr<FrontendHandler>,unsigned controller=0);
    ~FrontendAudioOptions();
    FrontendAudioOptions(const FrontendAudioOptions&)=delete;
    FrontendAudioOptions& operator=(const FrontendAudioOptions&)=delete;
    FrontendSession::Handle Current()const;
    FrontendAudioOptionsStatus Status()const;
    void AdvanceVisual(const FrontendSession::Handle&,float delta);
    void Acknowledge(const FrontendSession::Handle&,FrontendPointerViewport);
    std::array<FrontendPointerBounds,6> Bounds()const;
    void DeliverPointer(const FrontendSession::Handle&,const FrontendPointerEvent&);
    FrontendPointerDispatch Route(const FrontendPointerDesktopSample&);
    FrontendPointerDispatch Poll(SDL_Window*,bool capture=false);
    void NotifyBackButton(const FrontendSession::Handle&);
    void Save(const FrontendSession::Handle&); // Explicit missing SaveLoad boundary.
    // Explicit host-preferences save, not original GameInfo/SaveLoad readiness.
    // Preserves all non-audio/default fields of the observed provider. Caller
    // retains/polls it through completion and gates departure on its real state.
    void SaveNativePreferences(const FrontendSession::Handle&,std::shared_ptr<NativePreferences>);
    void Release();
};
}
