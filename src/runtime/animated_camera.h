#pragma once
#include "runtime/camera_assets.h"
#include "runtime/cameras.h"

namespace mscharged
{
// Explicit inputs for the selected playback code. This does not initialize the
// game's Presentation, FixedUpdateTask, or depth-of-field renderer.
struct AnimatedCameraInputs
{
    bool widescreen = false, letterbox = false;
    float simulation_time = 0;
};
struct AnimatedCameraOptions
{
    // Original quaternion mode retains/publishes the finite target channel.
    // Look-at additionally requires representable original normalization math;
    // validation failure during playback poisons the owning camera session.
    bool cyclic = true, simulation_time = false, look_at = false;
    float speed = 1;
    nlVector3 offset{0,0,0}, mirror{1,1,1};
    unsigned short facing = 0;
    void (*on_end)() = nullptr; // Called under the camera owner's mutation guard.
};

// Borrowed by OriginalCameras; destroy this owner before releasing game memory.
// Keep it in automatic/optional storage so camera-session allocation ownership
// cannot conflict with an external owning pointer.
class AnimatedCamera final : private cAnimCamera
{
    CameraAsset::Handle asset_;
    AnimatedCameraInputs inputs_;
    float focal_output_ = 0;
    class Scope;
    void Update(float delta) override;
public:
    explicit AnimatedCamera(CameraAsset::Handle asset);
    ~AnimatedCamera() override = default;
    AnimatedCamera(const AnimatedCamera&) = delete;
    AnimatedCamera& operator=(const AnimatedCamera&) = delete;
    static void* operator new(std::size_t) = delete;
    cBaseCamera& Camera() { CheckNativeCameraThread(); return *this; }
    void Configure(const AnimatedCameraOptions& options);
    // Original successful SelectCameraAnimation changes the retained track and
    // resets only its simulation clock. Time/view stay unchanged until Seek or
    // Update; absent aliases are rejected by the owning catalog before this call.
    void Select(CameraAsset::Handle);
    void SetCyclic(bool);
    void SetEndCallback(void (*callback)());
    void SetInputs(AnimatedCameraInputs inputs);
    void Seek(float normalized_time);
    float Advance(float delta); // Original ManualUpdate; manager Update also works.
    float Time() const;
    float Duration() const;
    float TimeLeft() const;
    float FocalDistance() const;
};

// Used only by the selected original TU within an owning camera's call scope.
const AnimatedCameraInputs& AnimatedInputs(const cAnimCamera& camera);
void CheckAnimatedSample(const cAnimCamera& camera, float normalized_time);
void CheckAnimatedAdvance(const cAnimCamera& camera, float delta);
void CheckAnimatedLookAt(const nlVector3& position, const nlVector3& target);
void RecordAnimatedFocal(const cAnimCamera& camera, float distance);
}
