#pragma once
#include "runtime/frontend_session.h"
#include "runtime/frontend_audio.h"
#include "runtime/frontend_input.h"
#include "runtime/frontend_stack_visual.h"
#include <functional>
#include <optional>

namespace mscharged
{
enum class FrontendCreditsCommandKind { PointerEnabled, StadiumRendering, StopMusic, SelectMusic, ReplaceScene };
struct FrontendCreditsCommand
{
    FrontendCreditsCommandKind kind;
    unsigned argument=0;
};
struct FrontendCreditsMovieRequest
{
    std::uint64_t generation=0;
    std::string path;
    bool details_with_sound=true;
    // MoviePlayerScene::Update passes false regardless of mWithSound.
    bool start_with_sound=false,loop=false,synced=true;
};
enum class FrontendCreditsBoundary { None, MoviePlayback };
struct FrontendCreditsStatus
{
    unsigned phase=0;
    float elapsed=0;
    bool fade_started=false,default_fade=false,failed=false;
    FrontendCreditsBoundary boundary=FrontendCreditsBoundary::None;
    std::optional<FrontendCreditsMovieRequest> movie;
    bool full_scene_created=false;
};
// Selected original Credits phase ownership. Input/RNG outlive this owner;
// callbacks retain real pointer/stadium/music services and run on the owner
// thread. This is not the full MoviePlayer/HOME/manager lifecycle. A decoded
// THP EOF is not a playback completion receipt: movie phases stay explicit
// until a real presentation/audio provider can execute them.
class FrontendCredits : public FrontendStackVisual
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
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
    using Frame=FrontendSession::Handle;
    using Services=std::function<void(FrontendCreditsCommand)>;
    FrontendCredits(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,Services,
        bool widescreen=false,unsigned video_mode=0);
    FrontendCredits(std::shared_ptr<FrontendSession>,FrontendInput&,
        std::shared_ptr<FrontendAudio>,unsigned& caller_seed,Services,
        std::shared_ptr<FrontendHandler>,bool widescreen=false,unsigned video_mode=0);
    ~FrontendCredits();
    FrontendCredits(const FrontendCredits&)=delete;
    FrontendCredits& operator=(const FrontendCredits&)=delete;
    Frame Current() const;
    FrontendCreditsStatus Status() const;
    void Update(const Frame& expected,float delta);
    // In a retained stack input window, the expected frame is the last truly
    // published frame; the current source candidate remains unpresented.
    // Original Credits has no early input-lock clock gate. FEInput itself
    // suppresses locked queries while original base/timing still advance.
    bool Button(const Frame& expected,FrontendAction,FrontendButtonQuery,int pad=-1);
    void Release();
};
}
