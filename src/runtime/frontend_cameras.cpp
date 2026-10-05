#include "runtime/frontend_cameras.h"
#include "runtime/animated_camera.h"
#include "runtime/frontend_camera_assets.h"
#include "runtime/graphics_memory.h"
#include "NL/MemAlloc.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <stdexcept>

namespace mscharged
{
thread_local FrontendCameras* FrontendCameras::transition_owner_ = nullptr;
struct FrontendCameraSelection
{
    const cBaseCamera* camera = nullptr;
    std::string alias;
};
// Original CameraMan can delete a replaced camera. Its native allocation owns
// this wrapper; the embedded playback camera stays borrowed and is never put in
// the manager's allocation registry or stack as a separate object.
class FrontendCameras::Camera final : public cBaseCamera
{
    FrontendCameras& owner_;
    AnimatedCamera playback_;
    cBaseCamera& body_;
public:
    static thread_local Camera* updating;
    FrontendCameraSelectionHandle selection;
    std::function<void()> on_end;
    static void End()
    {
        if (!updating) throw std::logic_error("Frontend end callback has no active camera owner");
        if (updating->on_end) updating->on_end();
    }
    Camera(FrontendCameras& owner, CameraAsset::Handle asset)
        : owner_(owner), playback_(asset), body_(playback_.Camera())
    {
        selection = std::make_shared<FrontendCameraSelection>(FrontendCameraSelection{this,asset->Name()});
        playback_.SetEndCallback(End);
    }
    ~Camera() override
    {
        const bool was_busy = owner_.busy_;
        owner_.busy_ = true; on_end = {}; owner_.Forget(this); owner_.busy_ = was_busy;
    }
    eCameraType GetType() override { return body_.GetType(); }
    void Update(float delta) override
    {
        if (updating) throw std::logic_error("Frontend camera update is reentrant");
        const bool was_busy = owner_.busy_;
        owner_.busy_ = true; updating = this;
        try { body_.Update(delta); }
        catch (...) { owner_.failed_ = true; updating = nullptr; owner_.busy_ = was_busy; throw; }
        updating = nullptr; owner_.busy_ = was_busy;
    }
    void Select(CameraAsset::Handle asset, FrontendCameraSelectionHandle identity,
        bool cyclic, std::function<void()> callback)
    {
        playback_.Select(std::move(asset)); playback_.SetCyclic(cyclic);
        selection = std::move(identity); on_end = std::move(callback);
    }
    void Seek(float time) { playback_.Seek(time); }
    void SetCyclic(bool value) { playback_.SetCyclic(value); }
    float Time() const { return playback_.Time(); }
    float Duration() const { return playback_.Duration(); }
    void Reactivate() override { body_.Reactivate(); }
    const nlMatrix4& GetViewMatrix() const override { return body_.GetViewMatrix(); }
    const nlVector3& GetCameraPosition() const override { return body_.GetCameraPosition(); }
    const nlVector3& GetTargetPosition() const override { return body_.GetTargetPosition(); }
    float GetFOV() const override { return body_.GetFOV(); }
};

thread_local FrontendCameras::Camera* FrontendCameras::Camera::updating = nullptr;

class FrontendCameras::Operation
{
    FrontendCameras& owner_;
    int exceptions_ = std::uncaught_exceptions();
public:
    explicit Operation(FrontendCameras& owner) : owner_(owner) { owner_.busy_ = true; }
    ~Operation()
    {
        owner_.failed_ |= std::uncaught_exceptions() > exceptions_;
        owner_.busy_ = false;
    }
};

FrontendCameras::FrontendCameras(OriginalCameras& core, const CameraAssetLibrary& library)
    : core_(core), library_(library)
{
    if (!core_.Active()) throw std::logic_error("Frontend cameras require an active camera session");
    { NativeCameraCall check; }
    cameras_.reserve(MaximumFrontendCameras + 1); // Replacement prepares before deleting the old owner.
}
FrontendCameras::~FrontendCameras()
{
    try { Release(); } catch (...) { std::terminate(); }
}
void FrontendCameras::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("Frontend cameras require their owner thread");
}
void FrontendCameras::Ready() const
{
    CheckThread();
    if (!core_.Active()) throw std::logic_error("Frontend camera session is no longer active");
    if (busy_ || failed_ || released_) throw std::logic_error("Frontend cameras are busy, failed or released");
    { NativeCameraCall check; } // Reject a core callback before any allocation.
    for (auto* camera : cameras_)
        if (!cCameraManager::HasCamera(camera)) throw std::logic_error("Frontend camera stack was changed externally");
}
FrontendCameras::Camera* FrontendCameras::Owned(cBaseCamera* camera) const
{
    const auto found = std::find(cameras_.begin(), cameras_.end(), camera);
    return found == cameras_.end() ? nullptr : *found;
}
void FrontendCameras::Forget(Camera* camera) noexcept
{
    const auto found = std::find(cameras_.begin(), cameras_.end(), camera);
    if (found != cameras_.end()) cameras_.erase(found);
}
void FrontendCameras::TransitionCallback(eCameraMessage message)
{
    auto* owner = transition_owner_;
    transition_owner_ = nullptr;
    if (!owner) return;
    auto callback = std::move(owner->callback_);
    owner->transition_selection_.reset();
    if (callback) callback(message);
}
void FrontendCameras::OwnTransition(std::function<void(eCameraMessage)> callback,
    FrontendCameraSelectionHandle selection) noexcept
{
    // The original operation first aborts any previous transition. Publish its
    // new callback owner only after that operation has succeeded.
    if (transition_owner_)
    {
        transition_owner_->callback_ = {};
        transition_owner_->transition_selection_.reset();
    }
    transition_owner_ = this;
    callback_ = std::move(callback);
    transition_selection_ = std::move(selection);
}
void FrontendCameras::CancelTransition(bool live_core) noexcept
{
    if (transition_owner_ != this) return;
    transition_owner_ = nullptr;
    callback_ = {};
    transition_selection_.reset();
    if (live_core && cCameraManager::m_pCallback == TransitionCallback)
    {
        cCameraManager::m_pCallback = nullptr;
        cCameraManager::m_transition = eCT_NONE;
    }
}
void FrontendCameras::Destroy(bool live_core) noexcept
{
    if (busy_ || thread_ != std::this_thread::get_id()) std::terminate();
    CancelTransition(live_core);
    while (!cameras_.empty()) delete cameras_.back();
    released_ = true;
}
const cBaseCamera& FrontendCameras::Push(const std::string& name, void (*callback)(eCameraMessage),
    float duration, bool delete_current)
{
    return PushPrepared(name,callback,duration,delete_current,true,{});
}
FrontendCameraSelectionHandle FrontendCameras::PushAnimated(const std::string& name,
    bool cyclic, std::function<void()> on_end, std::function<void(eCameraMessage)> transition_end,
    float duration, bool delete_current)
{
    return PushPrepared(name,std::move(transition_end),duration,delete_current,cyclic,std::move(on_end)).selection;
}
FrontendCameras::Camera& FrontendCameras::PushPrepared(const std::string& name,
    std::function<void(eCameraMessage)> callback, float duration, bool delete_current,
    bool cyclic, std::function<void()> on_end)
{
    Ready(); CheckCameraDelta(duration);
    const auto alias = CanonicalCameraAlias(name);
    const auto catalog = FrontendCameraCatalog();
    if (std::none_of(catalog.begin(), catalog.end(), [&](const auto& entry) { return alias == entry.animationName; }))
        throw std::invalid_argument("Unsupported frontend camera alias: " + alias);
    auto asset = library_.Find(alias);
    if (!asset) throw std::invalid_argument("Frontend camera asset is not loaded: " + alias);
    if (delete_current && !Owned(cCameraManager::PeekCamera()))
        throw std::logic_error("Frontend replacement requires an owned current camera");
    if (cameras_.size() >= MaximumFrontendCameras && !delete_current)
        throw std::length_error("Frontend camera stack capacity exhausted");
    if (duration > 0) CheckCameraTransition(duration, eCT_EASE_IN);

    Operation operation(*this);
    std::unique_ptr<Camera> camera;
    {
        ScopedGameAllocator arena(VirtualAllocator);
        camera.reset(new (8, false) Camera(*this, std::move(asset)));
    }
    camera->SetCyclic(cyclic);
    camera->on_end = std::move(on_end);
    cameras_.push_back(camera.get());
    // Reserve the manager's tracking entry before an immediate replacement
    // destroys its old camera. Push's existing insertion then finds this entry.
    TrackNativeCamera(camera.get());
    if (duration == 0)
    {
        // FE/feCamera.cpp deletes the old camera before the immediate push.
        if (delete_current) delete cCameraManager::PopCamera();
        cCameraManager::PushCamera(camera.get());
    }
    else
    {
        cCameraManager::PushCameraWithTransition(camera.get(), duration, eCT_EASE_IN, TransitionCallback, delete_current);
        OwnTransition(std::move(callback),camera->selection);
    }
    return *camera.release();
}
void FrontendCameras::Pop(void (*callback)(eCameraMessage), float duration)
{
    PopPrepared(callback,duration);
}
void FrontendCameras::PopAnimated(const FrontendCameraSelectionHandle& expected,
    std::function<void(eCameraMessage)> callback,float duration)
{
    Ready(); Selected(expected);
    PopPrepared(std::move(callback),duration);
}
void FrontendCameras::PopPrepared(std::function<void(eCameraMessage)> callback, float duration)
{
    Ready(); CheckCameraDelta(duration);
    if (!Owned(cCameraManager::PeekCamera())) throw std::logic_error("Frontend pop requires an owned current camera");
    if (duration > 0) { CheckCameraPop(true); CheckCameraTransition(duration, eCT_EASE_IN); }
    const auto selection=Owned(cCameraManager::PeekCamera())->selection;
    Operation operation(*this);
    if (duration == 0) delete cCameraManager::PopCamera();
    else
    {
        delete cCameraManager::PopCameraWithTransition(duration, eCT_EASE_IN, TransitionCallback);
        OwnTransition(std::move(callback),selection);
    }
}
void FrontendCameras::Advance(float delta, float simulation_delta)
{
    Ready(); CheckCameraDelta(delta); CheckCameraDelta(simulation_delta);
    Operation operation(*this); core_.Advance(delta, simulation_delta);
}
const cBaseCamera* FrontendCameras::ActiveCamera() const { Ready(); return Owned(cCameraManager::PeekCamera()); }
std::string FrontendCameras::ActiveAlias() const
{
    Ready(); const auto* camera = Owned(cCameraManager::PeekCamera()); return camera ? camera->selection->alias : std::string();
}
FrontendCameras::Camera& FrontendCameras::Selected(const FrontendCameraSelectionHandle& identity, bool active) const
{
    if (!identity) throw std::invalid_argument("Frontend camera selection is absent");
    auto* camera = Owned(const_cast<cBaseCamera*>(identity->camera));
    if (!camera || camera->selection != identity)
        throw std::logic_error("Frontend camera selection is stale or belongs to another owner");
    if (active && (camera != cCameraManager::PeekCamera() || cCameraManager::m_transition != eCT_NONE))
        throw std::logic_error("Frontend camera selection requires its active unblended camera");
    return *camera;
}
FrontendCameraSelectionHandle FrontendCameras::Selection() const
{
    Ready();
    if (cCameraManager::m_transition != eCT_NONE)
        throw std::logic_error("Frontend selection requires an owned unblended current camera");
    return CurrentSelection();
}
FrontendCameraSelectionHandle FrontendCameras::CurrentSelection() const
{
    Ready(); auto* camera = Owned(cCameraManager::PeekCamera());
    if (!camera) throw std::logic_error("Frontend selection requires its owned current camera");
    return camera->selection;
}
bool FrontendCameras::IsCurrent(const FrontendCameraSelectionHandle& identity) const
{
    Ready();
    if(!identity)return false;
    auto* camera=Owned(const_cast<cBaseCamera*>(identity->camera));
    return camera&&camera->selection==identity&&camera==cCameraManager::PeekCamera();
}
FrontendCameraSelectionHandle FrontendCameras::Select(const FrontendCameraSelectionHandle& expected,
    const std::string& name, bool cyclic, std::function<void()> callback)
{
    Ready(); auto& camera = Selected(expected);
    const auto alias = CanonicalCameraAlias(name);
    const auto catalog = FrontendCameraCatalog();
    if (std::none_of(catalog.begin(),catalog.end(),[&](const auto& entry){return alias == entry.animationName;}))
        throw std::invalid_argument("Unsupported frontend camera alias: " + alias);
    auto asset = library_.Find(alias);
    if (!asset) throw std::invalid_argument("Frontend camera asset is not loaded: " + alias);
    auto identity = std::make_shared<FrontendCameraSelection>(FrontendCameraSelection{&camera,alias});
    Operation operation(*this);
    camera.Select(std::move(asset), identity, cyclic, std::move(callback));
    return identity;
}
void FrontendCameras::Seek(const FrontendCameraSelectionHandle& selection, float time)
{
    Ready(); auto& camera = Selected(selection);
    if (!std::isfinite(time) || time < 0 || time > 1)
        throw std::out_of_range("Frontend camera seek requires normalized time zero through one");
    Operation operation(*this); camera.Seek(time);
}
void FrontendCameras::SetCyclic(const FrontendCameraSelectionHandle& selection, bool cyclic)
{
    Ready(); auto& camera = Selected(selection); Operation operation(*this); camera.SetCyclic(cyclic);
}
float FrontendCameras::Time(const FrontendCameraSelectionHandle& selection) const
{ Ready(); return Selected(selection).Time(); }
float FrontendCameras::Duration(const FrontendCameraSelectionHandle& selection) const
{ Ready(); return Selected(selection).Duration(); }
bool FrontendCameras::DetachEndCallback(const FrontendCameraSelectionHandle& selection)
{
    CheckThread();
    if (busy_) throw std::logic_error("Cannot detach frontend camera callback during dispatch");
    if (!core_.Active())
    {
        if (!cameras_.empty()) throw std::logic_error("Released core retained frontend camera allocations");
        return false;
    }
    CheckNativeCameraTeardown();
    if (!selection) return false;
    auto* camera = Owned(const_cast<cBaseCamera*>(selection->camera));
    if (!camera || camera->selection != selection) return false;
    // Keep the static source dispatch thunk harmless. Removing its retained
    // callback needs no camera rebuild and is safe after a failed core update.
    busy_ = true;
    camera->on_end = {};
    busy_ = false;
    return true;
}
bool FrontendCameras::DetachTransitionCallback(const FrontendCameraSelectionHandle& selection)
{
    CheckThread();
    if(busy_)throw std::logic_error("Cannot detach frontend blend callback during dispatch");
    if(core_.Active())CheckNativeCameraTeardown();
    if(!selection||transition_owner_!=this||transition_selection_!=selection)return false;
    callback_={};transition_selection_.reset();
    return true;
}
std::size_t FrontendCameras::Size() const { Ready(); return cameras_.size(); }
bool FrontendCameras::Failed() const { CheckThread(); return failed_; }
void FrontendCameras::Release()
{
    CheckThread();
    if (busy_) throw std::logic_error("Cannot release frontend cameras during a callback");
    // Release of the original core has already deleted every registered wrapper
    // and reset its callback state. Otherwise even an empty frontend owner must
    // reject cleanup from inside a shared-core callback.
    const bool live_core = core_.Active();
    if (live_core) CheckNativeCameraTeardown();
    else if (!cameras_.empty()) throw std::logic_error("Released core retained frontend camera allocations");
    if (!released_) Destroy(live_core);
}
}
