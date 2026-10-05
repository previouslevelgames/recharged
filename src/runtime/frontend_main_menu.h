#pragma once
#include "runtime/frontend_audio.h"
#include "runtime/frontend_pointer_host.h"
#include "runtime/frontend_stack_visual.h"
#include <optional>

namespace mscharged
{
enum class FrontendMainSelectionService { ApplyItem, OnlineSaveMii };
struct FrontendMainSelection
{
    unsigned item = 0, pointer = 0;
    FrontendMainSelectionService service = FrontendMainSelectionService::ApplyItem;
    FrontendSession::Handle source;
};
struct FrontendMainMenuStatus
{
    std::array<int,4> highlighted{};
    std::array<std::array<int,4>,7> pointer_states{};
    std::array<bool,7> default_arrows{}; // Original empty TLComponentDefault; required nodes never default.
    std::optional<FrontendMainSelection> selection;
    bool interactive = false, failed = false;
};
// Original Main component lookup, pointer bounds, Open/Close and Select dispatch
// over retained native resources. This is the menu's visual/input owner, not its
// complete SceneCreated/Update: music, save/HallOfFame, navigation, global pointer
// feedback, Mii/network and ApplyItem transitions remain external services.
// A source Select request stops input at its real ApplyItem/online dependency;
// no successful menu transition or game-ready flag is synthesized.
// Audio and caller RNG must outlive this owner. Destroy before input/SDL/NL.
class FrontendMainMenu : public FrontendStackVisual
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
    FrontendMainMenu(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,bool media_build=false);
    FrontendMainMenu(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,std::shared_ptr<FrontendHandler>,
        bool media_build);
    ~FrontendMainMenu();
    FrontendMainMenu(const FrontendMainMenu&)=delete;
    FrontendMainMenu& operator=(const FrontendMainMenu&)=delete;
    FrontendSession::Handle Current() const;
    FrontendMainMenuStatus Status() const;
    std::array<FrontendPointerBounds,7> Bounds() const;
    // Only caller-acknowledged successful rendering may publish input. Matching
    // resources retain listener history; a new viewport requires neutral input.
    void Acknowledge(const FrontendSession::Handle&,FrontendPointerViewport);
    FrontendPointerDispatch Route(const FrontendPointerDesktopSample&);
    FrontendPointerDispatch Poll(SDL_Window*,bool capture=false);
    void DeliverPointer(const FrontendSession::Handle&,const FrontendPointerEvent&);
    // Focused original base update advances authored visuals. It does not claim
    // the complete SHMainMenu save/HOF/intro gate passed. Call after input once.
    void AdvanceVisual(const FrontendSession::Handle&,float delta);
    void Release();
};
}
