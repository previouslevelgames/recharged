#pragma once
#include "resources/frontend_scene.h"
#include "runtime/frontend_visuals.h"
#include "runtime/frontend_images.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mscharged
{
using FrontendResourceSceneToken = std::uint64_t;
enum class FrontendResourceCreationStage { SetPresentation, SceneCreated, InitializeSubHandlers, Complete };
enum class FrontendResourcePending
{
    None, Package, SetPresentation, SceneCreated, InitializeSubHandlers,
    Assets, ContextRegistration, TextureRegistration, FontRegistration,
    DynamicTexture, EmbeddedContext, Release
};
struct FrontendResourceSceneView
{
    FrontendResourceSceneToken token = 0;
    std::string path;
    std::shared_ptr<const resources::FrontendScene> package;
};
using FrontendResourceSceneHandle = std::shared_ptr<const FrontendResourceSceneView>;
struct FrontendResourceCreationAdmission
{
    bool admitted = false;
    std::vector<std::string> missing;
};
// Mandatory selected native callbacks. Missing functions stay pending. An
// admitted selected callback does not establish full concrete SceneCreated or
// original save/Mii/HBM/global services. No successful default is installed.
// A Pending callback retains its own exact async identity/cursor; it cannot
// repeat already-applied effects when retried with the same retained scene.
struct FrontendResourceSceneCallbacks
{
    using Function = std::function<FrontendResourceCreationAdmission(FrontendResourceSceneHandle)>;
    Function set_presentation, scene_created, initialize_subhandlers;
    std::vector<std::string> original_handler_dependencies;
};
enum class FrontendResourceLeaseKind { Context, Texture, Font };
// Actual native registration lifetime supplied by a graphics owner. Validation
// must check the real pool/slots and retained data. Release must drain real
// references before disposing the registration; on failure retain it for retry.
// The registration owner cannot be replaced by decoded host assets. There is
// no default provider, and no destructor callback that silently swallows failure.
struct FrontendResourceLease
{
    FrontendResourceLeaseKind kind = FrontendResourceLeaseKind::Context;
    std::shared_ptr<void> registration;
    std::uint64_t marker = 0;
    std::shared_ptr<const resources::Texture> texture;
    std::shared_ptr<const resources::FrontendFont> font;
    std::function<void()> validate, release;
};
struct FrontendResourceRegistrations
{
    using Lease = std::shared_ptr<FrontendResourceLease>;
    std::function<Lease(FrontendResourceSceneHandle)> mark_context;
    std::function<Lease(FrontendResourceSceneHandle, std::shared_ptr<const resources::Texture>)> texture;
    std::function<Lease(FrontendResourceSceneHandle, std::shared_ptr<const resources::FrontendFont>)> font;
};
struct FrontendResourceSceneStatus
{
    FrontendResourceSceneToken token = 0;
    int source_state = 1; // Genuine selected source lifecycle: 1 -> 5 -> 6.
    FrontendResourceCreationStage creation = FrontendResourceCreationStage::SetPresentation;
    FrontendResourcePending pending = FrontendResourcePending::Package;
    bool active = false, failed = false, retiring = false;
    bool full_scene_created = false; // This owner selects callback/resource scope only.
    unsigned valid_resources = 0, resources = 0, completion_callbacks = 0;
    std::vector<std::string> missing;
    std::vector<std::string> original_handler_dependencies;
};
struct FrontendResourceContextStatus
{
    unsigned scenes = 0, push_pop_messages = 0, queued_resources = 0;
    FrontendResourceSceneToken current_context = 0;
    bool main_assets_ready = false, main_assets_failed = false;
    bool all_resource_scenes_valid = false;
    bool discard_frame = true; // Original !AreAllScenesValid, not native Published.
};

// Bounded original Main permanent-resource profile, ordered FEN context/ring
// completion and selected handler callbacks. Sources stay in original state1
// until a real NL FEN decode; state5/callbacks precede resource queue completion.
// State6 requires actual validated registration leases plus the original queue
// or context-switch completion operation. It does not qualify full SceneCreated,
// Wii globals, movie playback, original FE tasks or a rendered/presented frame.
// InGame on-demand sorting, permanent/mini null-scene contexts are unselected.
// Service, mutate, release and destroy on the creating NL thread before NL and
// graphics shutdown. Pop/release failure retains its exact queue/leases for retry.
class FrontendResourceContexts
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit FrontendResourceContexts(FrontendResourceRegistrations);
    ~FrontendResourceContexts();
    FrontendResourceContexts(const FrontendResourceContexts&) = delete;
    FrontendResourceContexts& operator=(const FrontendResourceContexts&) = delete;
    void BeginMainResources(FrontendLanguage = FrontendLanguage::English);
    FrontendResourceSceneToken QueueScene(std::string path, FrontendResourceSceneCallbacks = {});
    void QueuePop(FrontendResourceSceneToken);
    // Each call admits one original resource-manager Update turn. Poll observes
    // an externally serviced NL pump; Service additionally runs that real pump.
    void Poll();
    void Service();
    FrontendResourceSceneStatus SceneStatus(FrontendResourceSceneToken) const;
    FrontendResourceSceneHandle Scene(FrontendResourceSceneToken) const;
    FrontendResourceContextStatus Status() const;
    // Propagates captured async/decode/provider errors. A missing provider is
    // typed pending rather than an exception or fabricated success.
    void Check(FrontendResourceSceneToken) const;
    void Release(); // Cancels reads and pops in reverse ownership order; retryable.
};
}
