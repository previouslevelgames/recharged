#pragma once
#include "resources/frontend_scene.h"
#include "resources/frontend_instances.h"
#include <memory>
#include <string_view>

namespace mscharged::resources
{
// Bounded original presentation/slide animation over an owned FEN snapshot.
// Constructor/Reset restore exported assets, reset the presentation clock as
// SetActiveSlide(resetTime=true), then execute Update(0). No handlers/transitions.
// Advance commits the entire snapshot only on success. Scene references expire
// on the next successful Advance/Reset; retained layout texture/font handles do not.
class FrontendAnimationPlayback
{
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit FrontendAnimationPlayback(std::unique_ptr<Impl>);
public:
    FrontendAnimationPlayback(const FrontendScene&,FrontendReference selected={});
    ~FrontendAnimationPlayback();
    FrontendAnimationPlayback(const FrontendAnimationPlayback&)=delete;
    FrontendAnimationPlayback& operator=(const FrontendAnimationPlayback&)=delete;
    void Advance(float delta);
    // Explicit native observation of an already active loading notification.
    // Original BaseLoadingScene order: advance, then inclusive completion/hide.
    // Commits neither clock nor visibility on failure; returns still-active.
    bool AdvanceLoadingNotification(float delta, std::uint32_t component_instance);
    // Original TLComponentInstance::Update: selected component slide only.
    void AdvanceComponent(std::uint32_t instance, float delta);
    void Reset();
    std::unique_ptr<FrontendAnimationPlayback> Clone() const;
    // Original lower-hash first-match selection. Missing names clear active.
    // Presentation does not sample immediately; component calls Update(0).
    bool SelectPresentation(std::string_view name, bool reset_time = false);
    bool SelectComponent(std::uint32_t component_library_id, std::string_view name,
                         bool force_reset = false, bool preserve_time = false);
    void Apply(std::span<const FrontendInstanceChange>);
    FrontendLoadingSetup SetupLoadingScene(bool widescreen);
    const FrontendScene& Scene() const;
    float PresentationTime() const;
    std::size_t ChannelsEvaluated() const; // Last committed update, incl shared slide visits.
};
}
