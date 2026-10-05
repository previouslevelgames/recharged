#pragma once
#include "runtime/frontend_session.h"
#include "runtime/frontend_audio.h"
#include "runtime/frontend_input.h"
#include "runtime/frontend_stack_visual.h"
#include "runtime/frontend_movie_playback.h"
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
using FrontendCreditsMovieRequest=FrontendMovieRequest;
class FrontendMovieImageBinding;
enum class FrontendCreditsBoundary { None, MoviePlayback };
struct FrontendCreditsStatus
{
    unsigned phase=0;
    float elapsed=0;
    bool fade_started=false,default_fade=false,failed=false;
    FrontendCreditsBoundary boundary=FrontendCreditsBoundary::None;
    std::optional<FrontendCreditsMovieRequest> movie;
    // Original handler flags, not proof that output is currently playing.
    bool movie_started=false,texture_swapped=false,credits_over=false,final_message_displayed=false;
    unsigned parser_position=0,parser_tokens=0,lines_on_screen=0;
    bool full_scene_created=false;
};
// Selected original Credits phase ownership. Input/RNG outlive this owner;
// callbacks retain real pointer/stadium/music services and run on the owner
// thread. This is not the full MoviePlayer/HOME/manager lifecycle. A decoded
// THP EOF is not completion: only the exact provider's opaque presentation/audio
// receipt permits natural completion. Explicit source abort genuinely cancels it.
struct FrontendCreditsMovieTarget
{
    FrontendMovieRequest request;
    std::uint32_t instance=0;
    FrontendSession::Handle frame;
    std::shared_ptr<FrontendMoviePlayback> playback;
};
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
    using MovieFactory=std::function<std::shared_ptr<FrontendMoviePlayback>(
        const FrontendMovieRequest&,const FrontendMovieOptions&,std::uint64_t)>;
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
    // Idle preparation does not execute a source scene update or advance phase.
    // The factory must return the exact requested concrete owner/output profile.
    // Output advances only after the source MovieStart step has been admitted.
    void SetMovieProvider(MovieFactory,FrontendMovieOptions);
    void ServiceMovie(std::uint64_t actual_retrace);
    std::optional<FrontendCreditsMovieTarget> MovieTarget() const;
    void AttachMovieBinding(std::shared_ptr<const FrontendMovieImageBinding>);
    std::shared_ptr<const FrontendMovieImageBinding> MovieBinding() const;
    // Caller retains expected, drains/retires126 while idle, then permits the
    // next generation. Failed retirement must keep expected and retry; never
    // destroy a registration from a collecting render/update callback.
    void RetireMovieBinding(const std::shared_ptr<const FrontendMovieImageBinding>& expected);
    void DisplayFinalMessage(const Frame& expected);
    // In a retained stack input window, the expected frame is the last truly
    // published frame; the current source candidate remains unpresented.
    // Original Credits has no early input-lock clock gate. FEInput itself
    // suppresses locked queries while original base/timing still advance.
    bool Button(const Frame& expected,FrontendAction,FrontendButtonQuery,int pad=-1);
    void Release();
};
}
