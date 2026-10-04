#pragma once
#include "runtime/frontend_session.h"
#include <memory>
class GLView;
namespace mscharged
{
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
    FrontendSession::Handle Current() const;
    std::size_t Textures() const;
    // Collect original font glPoly2 and image glQuad3 packets in entry order.
    // Same-owner animated snapshots are accepted and published after success.
    // On failure cancel the original frame before FinishFrame or further work.
    unsigned Submit(GLView&, FrontendSession::Handle);
    void FinishFrame();
    void Release();
};
}
