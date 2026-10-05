#pragma once
#include "runtime/frontend_handler.h"
#include "runtime/frontend_stack_visual.h"
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace mscharged
{
struct FrontendStackRequest
{
    unsigned scene = 0; // Original SceneEntryTable index, not a guessed filename.
    FrontendLanguage language = FrontendLanguage::English;
    FrontendImageProfile image_profile = FrontendImageProfile::Main;
    std::string initial_slide;
    bool animate = true;
    unsigned movement = 0; // Original ScreenMovement: nothing 0, forward 1, back 2.
    FrontendSessionResourcesMode resources_mode = FrontendSessionResourcesMode::Scene;
    FrontendSessionResources::Handle shared_resources; // Opaque verified Main owners; new FEN only.
};
enum class FrontendStackState
{ Queued, Loading, AwaitingHandler, AwaitingPublication, Published, Failed };

struct FrontendStackContext
{
    std::uint64_t token;
    FrontendSession& session;
    FrontendHandler& handler;
    unsigned movement = 0;
    // These references are borrowed for this callback only. Do not retain them,
    // begin another load, acquire exclusive focus, or release either owner.
};
struct FrontendStackCallbacks
{
    std::function<void(FrontendStackContext&)> scene_created;
    std::function<void(FrontendStackContext&)> initialize_subhandlers;
    // Called after the actual original focus/base update. A concrete screen's
    // remaining update work must be provided explicitly; no success default.
    std::function<void(FrontendStackContext&, float)> after_base_update;
};
// Unlike the legacy borrowed callback context, these explicit owners may be
// retained by the returned concrete visual. Input/NL still outlive the stack.
struct FrontendStackVisualContext
{
    std::uint64_t token;
    std::shared_ptr<FrontendSession> session;
    std::shared_ptr<FrontendHandler> handler;
    unsigned movement = 0;
};
using FrontendStackVisualFactory = std::function<std::shared_ptr<FrontendStackVisual>(FrontendStackVisualContext)>;
enum class FrontendStackHandlerScope { Unbound, SuppliedCallbacks, SelectedVisual };
enum class FrontendStackSubhandlers { Unavailable, SuppliedCallbacks, SourceEmpty };
struct FrontendStackEntry
{
    std::uint64_t token = 0;
    unsigned scene = 0;
    FrontendStackState state = FrontendStackState::Queued;
    bool visible = true, queued_pop = false;
    FrontendSession::Handle prepared, published;
    unsigned movement = 0;
    FrontendStackHandlerScope handler_scope = FrontendStackHandlerScope::Unbound;
    FrontendStackSubhandlers subhandlers = FrontendStackSubhandlers::Unavailable;
    bool full_scene_created = false; // Selected visuals never establish this.
};
struct FrontendStackPublication
{
    std::uint64_t token;
    FrontendSession::Handle prepared;
};

// Checked native ownership around original queue, creation and render-order
// contracts. Does not construct concrete Title/Main/Movie handlers or set an
// original FEScene state6. Default entries stop at AwaitingHandler.
// Checked native resources finish before creation callbacks; their original
// SetPresentation/SceneCreated/InitializeSubHandlers order is retained. The
// console starts those callbacks before its resource queue reaches state6.
// Input, NL and game arenas must outlive this owner. Graphics remains external:
// one registration owner must prepare the complete active plan, then acknowledge
// each exact frame while idle. No GL pool marks or borrowed textures are owned
// here. Drain must genuinely finish/cancel all references before publication/pop.
class FrontendSceneStack
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using Token = std::uint64_t;
    explicit FrontendSceneStack(FrontendInput&, std::function<void()> drain);
    ~FrontendSceneStack();
    FrontendSceneStack(const FrontendSceneStack&) = delete;
    FrontendSceneStack& operator=(const FrontendSceneStack&) = delete;
    static std::string_view SourcePath(unsigned scene); // USA table; null/out-of-range rejected.
    Token QueuePush(FrontendStackRequest, FrontendStackCallbacks = {});
    Token QueuePop(); // First processed stack entry not already queued for pop.
    void QueuePop(Token);
    void Cancel(Token); // Remove an unpublished queued/loading/candidate entry.
    void Bind(Token, FrontendStackCallbacks); // Complete callbacks, once, before creation.
    void BindVisual(Token, FrontendStackVisualFactory); // Main1/Options13/Audio14/Visual15/Credits23 selected scope only.
    void Poll(); // Process FIFO commands, observe actual resource completion/creation.
    void Service(); // Poll, one real NL service pass, Poll.
    using PresentedInput = std::function<void(Token, const FrontendSession::Handle&)>;
    // Selected visuals first admit their source-specific pre-base gate. Admitted
    // visuals receive exactly one proven base update, their source
    // gate, then this input window against last acknowledged geometry. Mutations
    // apply to current proven resources and remain unpresented until Publish.
    void Update(float delta, PresentedInput = {});
    void Publish(Token, const FrontendSession::Handle& prepared);
    void Publish(std::span<const FrontendStackPublication>); // One drain, atomic complete batch.
    void SetVisible(Token, bool);
    void SetTopMost(Token); // Zero clears; affects render order, not update order.
    FrontendStackEntry Entry(Token) const;
    FrontendSessionResources::Handle Resources(Token) const; // Null for ordinary Scene loads.
    std::vector<FrontendStackEntry> Entries() const; // Processed newest-first, then queued pushes.
    std::vector<FrontendStackEntry> RenderPlan() const; // Retained published frames only.
    bool AllReady() const; // Native callbacks+publication+empty queue only; not the game's full gate.
    bool Failed() const; // Drain/shared-service failure: Release must retry cleanup.
    void RethrowFailure(Token) const;
    void Release(); // Drain failure retains all owners and allows another Release.
};
}
