#pragma once
#include "runtime/frontend_session.h"
#include "runtime/frontend_movie_binding.h"
#include <memory>
class GLView;
namespace mscharged
{
struct FrontendMovieEntryStatus
{
    unsigned awaiting_binding=0, awaiting_frame=0, cancelled=0, ready=0;
    unsigned Pending() const { return awaiting_binding+awaiting_frame+cancelled; }
};
struct FrontendMoviePacketStatus
{
    FrontendMovieEntryStatus current; // Current layout, evaluated against live provider state.
    FrontendMovieEntryStatus frame; // Aggregate of successful Submit calls in the last frame.
    unsigned submitted_packets=0; // Queued original packets, NOT encoded/presented receipts.
    std::uint64_t frame_generation=0;
};
// One retained graphics generation in its own original resource pool. Replaces
// font/image bindings atomically while idle; failed replacement keeps Current.
// Game graphics, frame services and arenas must outlive this owner.
class FrontendPacketRenderer
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit FrontendPacketRenderer(void (*drain)());
    ~FrontendPacketRenderer();
    FrontendPacketRenderer(const FrontendPacketRenderer&) = delete;
    FrontendPacketRenderer& operator=(const FrontendPacketRenderer&) = delete;
    // Validate the complete layout and publish its real texture registrations.
    // Must run while idle, before frame acquisition. Same-owner snapshots reuse
    // registration; only this exact old generation may have colliding hashes.
    void Prepare(FrontendSession::Handle);
    // Register actual Y/U/V storage before the source swap/first decoded frame.
    // Session/current instance must be the authored Layer/movie selection.
    FrontendMovieImageBinding::Handle BindMovie(std::shared_ptr<FrontendSession>,
        std::uint32_t instance,std::shared_ptr<FrontendMoviePlayback>);
    void RetireMovie(); // Idle, drained; retained metadata becomes explicitly inactive.
    FrontendSession::Handle Current() const;
    std::size_t Textures() const;
    // Pending entries retain their exact layout position but queue no movie
    // packet/receipt. Frame totals survive a subsequent NAV-only submission.
    FrontendMoviePacketStatus MovieStatus() const;
    // Collect original font glPoly2 and image glQuad3 packets in entry order.
    // Same-owner animated snapshots are accepted and published after success.
    // On failure cancel the original frame before FinishFrame or further work.
    unsigned Submit(GLView&, FrontendSession::Handle);
    void FinishFrame();
    void Release();
};
}
