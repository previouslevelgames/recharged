#pragma once
#include "runtime/frontend_session.h"
#include "runtime/frontend_input.h"

namespace mscharged
{
class FrontendBootAudio;
enum class FrontendBootBoundary { None, PlayLogoSound };
struct FrontendBootStatus
{
    int phase = 1;
    float elapsed = 0, strap_alpha = 255;
    bool strap_dismissed = false;
    FrontendBootBoundary boundary = FrontendBootBoundary::None;
};
// Retained retail USA BootLoadingScene, using shared original setup/update and
// actual FEInput queries. An optional retained audio owner admits the original
// logo cue and bank unload. Without it, stops at FEAudio::PlaySound. No physical
// audio completion, HOME, FEScene state6 or manager readiness is synthesized.
// Input must outlive the owner; operations require the creating NL thread.
class FrontendBootLoading
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using Frame = FrontendSession::Handle;
    explicit FrontendBootLoading(std::shared_ptr<FrontendSession>, FrontendInput&, bool widescreen = false,
        std::shared_ptr<FrontendBootAudio> audio = {});
    ~FrontendBootLoading();
    FrontendBootLoading(const FrontendBootLoading&) = delete;
    FrontendBootLoading& operator=(const FrontendBootLoading&) = delete;
    Frame Current() const;
    FrontendBootStatus Status() const;
    // Expected must be the successfully published visible frame. Atomic state,
    // original playback and layout commit; blocked updates leave them unchanged.
    void Update(const Frame& expected, float delta);
    void Reset(const Frame& expected);
    void Release();
};
}
