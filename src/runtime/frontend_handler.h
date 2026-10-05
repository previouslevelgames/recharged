#pragma once
#include "runtime/frontend_input.h"
#include "runtime/frontend_session.h"
#include <functional>
#include <vector>

namespace mscharged
{
// Native ownership around shared original base-handler update, ring and
// activation contracts. This does not construct a concrete game screen handler,
// the original manager, or its state 6. Input must outlive this owner.
class FrontendHandler
{
    friend class FrontendSceneStack;
    void AttachStack();
    void ReleaseStack();
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using Frame = FrontendSession::Handle;
    using ScreenId = std::uint64_t;
    // Issued only after one real original base update. Moving invalidates the
    // source; matching handler/session/sequence and both frames are checked on
    // consumption. Keeping a proof cannot extend the lifetime of its handler.
    class UpdateProof
    {
        friend class FrontendHandler;
        const FrontendHandler* owner_ = nullptr;
        std::uint64_t identity_ = 0, sequence_ = 0;
        float delta_ = 0;
        Frame before_, after_;
        UpdateProof(const FrontendHandler*, std::uint64_t, std::uint64_t,
                    float, Frame, Frame);
    public:
        UpdateProof(const UpdateProof&) = delete;
        UpdateProof& operator=(const UpdateProof&) = delete;
        UpdateProof(UpdateProof&&) noexcept;
        UpdateProof& operator=(UpdateProof&&) noexcept;
        const Frame& Before() const { return before_; }
        const Frame& After() const { return after_; }
        float Delta() const { return delta_; }
    };
    // Explicit native activation adapter: queries original input and returns
    // checked instance edits. Publication follows callback success atomically.
    using Activation = std::function<std::vector<resources::FrontendInstanceChange>(const Frame&, FrontendHandler&)>;
    FrontendHandler(std::shared_ptr<FrontendSession>, FrontendInput&);
    ~FrontendHandler();
    FrontendHandler(const FrontendHandler&) = delete;
    FrontendHandler& operator=(const FrontendHandler&) = delete;
    Frame Current() const;
    // Expected is the visible, successfully published frame; mismatches (e.g.
    // a host reload awaiting GPU registration) reject without advancing time.
    void Update(const Frame& expected, float delta);
    UpdateProof UpdateOnce(const Frame& expected, float delta);
private:
    UpdateProof StackUpdateOnce(const Frame&, float);
public:
    Frame ConsumeUpdate(UpdateProof&&, const Frame& before);
    bool Binds(const std::shared_ptr<FrontendSession>&) const;
    bool Button(const Frame& expected, FrontendAction, FrontendButtonQuery,
                int pad = -1, int* found_pad = nullptr);
    ScreenId AddScreen(Activation);
    void Activate(const Frame& expected, ScreenId);
    void OriginalRemove(ScreenId); // Original RemoveScreenHandler is empty.
    void Detach(ScreenId); // Explicit native ring/ownership removal.
    std::vector<ScreenId> Screens() const; // Original ring insertion order.
    ScreenId ActiveScreen() const; // Zero means no active native adapter.
    // At most one lock per owner; release in stack order before destroying an
    // owner. Original input depth remains four across every owner.
    void SetExclusiveInput(bool);
    // Observe an already visible authored notification component. Original
    // BaseLoadingScene::Update then advances first and hides at time>=start+duration.
    // Does not execute HOME activation, SceneCreated's HBM call or state6 gates.
    void WatchLoadingNotification(const Frame& expected, std::uint32_t instance);
    bool LoadingNotificationActive() const;
    void Release(); // Idempotent; clears owned adapters and retained session.
};
}
