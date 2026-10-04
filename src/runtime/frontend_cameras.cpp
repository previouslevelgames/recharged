#include "runtime/frontend_cameras.h"
#include "runtime/animated_camera.h"
#include "runtime/frontend_camera_assets.h"
#include "runtime/graphics_memory.h"
#include "NL/MemAlloc.h"
#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>

namespace mscharged
{
thread_local FrontendCameras* FrontendCameras::transition_owner_ = nullptr;
// Original CameraMan can delete a replaced camera. Its native allocation owns
// this wrapper; the embedded playback camera stays borrowed and is never put in
// the manager's allocation registry or stack as a separate object.
class FrontendCameras::Camera final : public cBaseCamera
{
    FrontendCameras& owner_;
    AnimatedCamera playback_;
    cBaseCamera& body_;
public:
    const std::string alias;
    Camera(FrontendCameras& owner, CameraAsset::Handle asset)
        : owner_(owner), playback_(asset), body_(playback_.Camera()), alias(asset->Name()) {}
    ~Camera() override { owner_.Forget(this); }
    eCameraType GetType() override { return body_.GetType(); }
    void Update(float delta) override { body_.Update(delta); }
    void Reactivate() override { body_.Reactivate(); }
    const nlMatrix4& GetViewMatrix() const override { return body_.GetViewMatrix(); }
    const nlVector3& GetCameraPosition() const override { return body_.GetCameraPosition(); }
    const nlVector3& GetTargetPosition() const override { return body_.GetTargetPosition(); }
    float GetFOV() const override { return body_.GetFOV(); }
};

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
    const auto callback = owner->callback_;
    owner->callback_ = nullptr;
    if (callback) callback(message);
}
void FrontendCameras::OwnTransition(void (*callback)(eCameraMessage)) noexcept
{
    // The original operation first aborts any previous transition. Publish its
    // new callback owner only after that operation has succeeded.
    if (transition_owner_) transition_owner_->callback_ = nullptr;
    transition_owner_ = this;
    callback_ = callback;
}
void FrontendCameras::CancelTransition(bool live_core) noexcept
{
    if (transition_owner_ != this) return;
    transition_owner_ = nullptr;
    callback_ = nullptr;
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
        OwnTransition(callback);
    }
    return *camera.release();
}
void FrontendCameras::Pop(void (*callback)(eCameraMessage), float duration)
{
    Ready(); CheckCameraDelta(duration);
    if (!Owned(cCameraManager::PeekCamera())) throw std::logic_error("Frontend pop requires an owned current camera");
    if (duration > 0) { CheckCameraPop(true); CheckCameraTransition(duration, eCT_EASE_IN); }
    Operation operation(*this);
    if (duration == 0) delete cCameraManager::PopCamera();
    else
    {
        delete cCameraManager::PopCameraWithTransition(duration, eCT_EASE_IN, TransitionCallback);
        OwnTransition(callback);
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
    Ready(); const auto* camera = Owned(cCameraManager::PeekCamera()); return camera ? camera->alias : std::string();
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
