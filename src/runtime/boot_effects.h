#pragma once
#include "runtime/boot_loading.h"
#include "runtime/effects_registry.h"

namespace mscharged
{
enum class BootEffectsState { Idle, Loading, Registered, Failed, Released };
struct BootEffectsCounts
{
    std::size_t templates=0, groups=0, textures=0, models=0, animations=0;
    unsigned completed_files=0;
};
// Exact original four-file boot profile. Binding is minted only by this concrete
// owner, and one BootLoading retains it at a time. Registration uses the exact
// persistent pool passed by boot service4, never a substitute inventory. NL,
// graphics memory/materials and OriginalFrames must outlive the binding/owner.
// Begin/finalize are serviced through BootLoading; callers pump real NL reads.
// Completion includes original glDiscardFrame(1), not simulation/actor readiness.
// Release drains real frames before removing only this marked registration.
class BootEffectsResources
{
    struct Implementation;
    std::shared_ptr<Implementation> impl_;
    std::weak_ptr<BootEffectsBinding> binding_;
public:
    // Qualified native effects profile; original console defaults remain in
    // BootLoading. No hidden allocation retry or future NPC capacity claim.
    static constexpr BootLoadingMemory NativeMemory{64*1024,8*1024*1024};
    BootEffectsResources();
    ~BootEffectsResources();
    BootEffectsResources(const BootEffectsResources&)=delete;
    BootEffectsResources& operator=(const BootEffectsResources&)=delete;
    BootEffectsBinding::Handle Binding();
    BootEffectsState State() const;
    BootEffectsCounts Counts() const;
    const GLResourcePool* Pool() const;
    EffectsRegistry::Handle Registry() const;
};
}
