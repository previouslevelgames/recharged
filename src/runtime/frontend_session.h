#pragma once
#include "runtime/frontend_images.h"
#include "runtime/frontend_visuals.h"
#include "resources/frontend_layout.h"
#include "resources/frontend_instances.h"
#include <string>

namespace mscharged
{
struct FrontendSessionRequest
{
    std::string path;
    FrontendLanguage language = FrontendLanguage::English;
    FrontendImageProfile image_profile = FrontendImageProfile::Main;
    std::string initial_slide; // Empty selects the exported active slide; otherwise unique name required.
    bool animate = true;
};
struct FrontendSessionFrame
{
    FrontendSessionRequest request;
    std::shared_ptr<const FrontendVisualAssets> visuals;
    resources::FrontendImageCatalog::Handle images;
    resources::FrontendScene graph;
    resources::FrontendLayoutFrame layout;
    std::size_t channels_evaluated = 0;
    unsigned image_completed_files = 0;
};
enum class FrontendSessionState { Idle, Loading, Ready, Failed, Cancelled };
struct FrontendSessionProgress
{
    bool fen_completed = false;
    unsigned visual_completed_mask = 0, image_completed_files = 0;
};
// One retained native scene, not the original FESceneManager stack or state 6.
// Begin queues real NL FEN/localization/font reads, then the selected image
// profile. Publication includes checked layout and optional original playback.
// Failed/cancelled replacement keeps Current; retained immutable snapshots own
// their host data after Pop/destruction and NL/arena shutdown. Service/mutate/
// destroy on the creating thread before NL shutdown. No handler callbacks run.
class FrontendSession
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using Handle = std::shared_ptr<const FrontendSessionFrame>;
    FrontendSession();
    ~FrontendSession();
    FrontendSession(const FrontendSession&) = delete;
    FrontendSession& operator=(const FrontendSession&) = delete;
    void Begin(FrontendSessionRequest);
    void Poll();
    void Service();
    void Cancel();
    void Pop(); // Cancels pending work and clears this single current scene.
    FrontendSessionState State() const;
    FrontendSessionProgress Progress() const;
    Handle Current() const;
    Handle Result() const; // Reports the latest Begin failure/pending/cancellation.
    // Current animated scene remains usable while replacement is pending or
    // failed. Each mutation publishes only after playback AND layout succeed.
    void Advance(float delta);
    void Reset();
    // Original first matching lower-hash lookup; missing name clears active.
    // Presentation selection does not Update(0); component selection does.
    bool SelectPresentation(std::string_view name, bool reset_time = false);
    bool SelectComponent(std::uint32_t component_library_id, std::string_view name,
                         bool force_reset = false, bool preserve_time = false);
    // IDs belong to this exact retained snapshot. Stale/foreign snapshots fail
    // before mutation; successful changes atomically include layout rebuilding.
    // Supported on static and animated current scenes, including replacement.
    void Apply(const Handle& expected_current, std::span<const resources::FrontendInstanceChange>);
    // Shared original BaseLoadingScene setup prefix; requires animated current
    // scene and stops before the unavailable HBMManager::SetBlocked call.
    resources::FrontendLoadingSetup SetupLoadingScene(const Handle& expected_current, bool widescreen);
};
}
