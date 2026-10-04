#pragma once
#include "NL/gl/glTarget.h"
#include <thread>

namespace mscharged
{
// Owns original target/pip (256x128 RGB565), copied from a 512x256 viewport.
// Requires OriginalViews. Detach all referring views/packets before Release;
// target destruction drains outstanding GPU work through the target registry.
// A containing OriginalViews shutdown may release it first; stale observations
// reject, and subsequent destruction never frees a newer session's target.
class NisPipTarget
{
    GLRenderPair pair_;
    const std::thread::id thread_ = std::this_thread::get_id();
    void CheckThread() const;
public:
    NisPipTarget();
    ~NisPipTarget();
    NisPipTarget(const NisPipTarget&) = delete;
    NisPipTarget& operator=(const NisPipTarget&) = delete;
    GLRenderPair Pair() const;
    unsigned long Texture() const;
    static unsigned ViewportWidth();
    static unsigned ViewportHeight();
    void Release();
};
}
