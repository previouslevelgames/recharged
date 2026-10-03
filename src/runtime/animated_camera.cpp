#include "runtime/animated_camera.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mscharged
{
namespace
{
struct Call
{
    const cAnimCamera* camera;
    const AnimatedCameraInputs* inputs;
    float* focal;
};
thread_local const Call* active = nullptr;
}
class AnimatedCamera::Scope
{
    Call call_;
public:
    explicit Scope(AnimatedCamera& camera) : call_{&camera, &camera.inputs_, &camera.focal_output_}
    {
        CheckNativeCameraThread();
        if (active) throw std::logic_error("Animated camera call is reentrant");
        if (!camera.asset_ || camera.m_pActiveCameraData != &camera.asset_->Data())
            throw std::logic_error("Animated camera lost its retained asset");
        active = &call_;
    }
    ~Scope() { active = nullptr; }
};
const AnimatedCameraInputs& AnimatedInputs(const cAnimCamera& camera)
{
    CheckNativeCameraThread();
    if (!active || active->camera != &camera)
        throw std::logic_error("Original animated camera requires an explicit native playback owner");
    return *active->inputs;
}
void CheckAnimatedSample(const cAnimCamera& camera, float time)
{
    AnimatedInputs(camera);
    // Original code converts the index to int even in the endpoint branch.
    const double index = double(time) * (camera.m_pActiveCameraData->m_uKeyCount - 1);
    if (!std::isfinite(time) || time < 0 || index > std::numeric_limits<int>::max() - 256.0)
        throw std::out_of_range("Animated camera time exceeds the original sample index range");
}
void CheckAnimatedAdvance(const cAnimCamera& camera, float delta)
{
    const auto& inputs = AnimatedInputs(camera);
    CheckCameraDelta(delta);
    if (camera.m_bUseSimulationTime)
    {
        delta = camera.m_fLastSimulationTime < 0 ? 0 : inputs.simulation_time - camera.m_fLastSimulationTime;
        CheckCameraDelta(delta);
    }
    CheckAnimatedSample(camera, camera.m_fAnimationTime + (delta * camera.m_fAnimationSpeed) / camera.GetDuration());
}
void CheckAnimatedLookAt(const nlVector3& position, const nlVector3& target)
{
    CheckCameraVector(position); CheckCameraVector(target);
    const double x = double(target.x) - position.x, y = double(target.y) - position.y;
    // Original look-at uses Z up: a parallel/zero direction has no valid basis.
    if (x*x + y*y < 1e-12) throw std::invalid_argument("Animated camera look-at direction is parallel to Z up");
}
void RecordAnimatedFocal(const cAnimCamera& camera, float distance)
{
    AnimatedInputs(camera);
    CheckCameraPose(camera);
    if (!std::isfinite(distance)) throw std::overflow_error("Animated camera focal distance overflow");
    *active->focal = distance;
}
AnimatedCamera::AnimatedCamera(CameraAsset::Handle asset) : asset_(std::move(asset))
{
    if (!asset_) throw std::invalid_argument("Animated camera needs a decoded asset");
    NativeCameraCall call;
    m_pActiveCameraData = const_cast<cCameraData*>(&asset_->Data()); // Original playback only reads key data.
    Scope scope(*this);
    SetAnimationTime(0, true);
}
void AnimatedCamera::Configure(const AnimatedCameraOptions& options)
{
    CheckCameraDelta(options.speed); CheckCameraVector(options.offset);
    for (float value : {options.mirror.x, options.mirror.y, options.mirror.z})
        if (value != 1 && value != -1) throw std::invalid_argument("Camera mirror axes must be +1 or -1");
    NativeCameraCall call;
    m_bCyclic = options.cyclic; m_bUseSimulationTime = options.simulation_time;
    m_bUseLookAt = options.look_at; m_fAnimationSpeed = options.speed;
    m_OffsetPos = options.offset; m_Mirror = options.mirror; mFacingAngle = options.facing;
    m_EndOfAnimationCallback = options.on_end; m_fLastSimulationTime = -1;
    Scope scope(*this); BuildAnimViewMatrix(m_matView);
}
void AnimatedCamera::SetInputs(AnimatedCameraInputs inputs)
{
    CheckCameraDelta(inputs.simulation_time);
    NativeCameraCall call;
    if (m_bUseSimulationTime && m_fLastSimulationTime >= 0 && inputs.simulation_time < m_fLastSimulationTime)
        throw std::invalid_argument("Camera simulation clock moved backwards");
    inputs_ = inputs;
}
void AnimatedCamera::Seek(float time)
{
    NativeCameraCall call; Scope scope(*this); SetAnimationTime(time, true);
}
float AnimatedCamera::Advance(float delta)
{
    CheckCameraDelta(delta);
    NativeCameraCall call; Scope scope(*this); return ManualUpdate(delta);
}
void AnimatedCamera::Update(float delta)
{
    Scope scope(*this); cAnimCamera::Update(delta);
}
float AnimatedCamera::Time() const { CheckNativeCameraThread(); return GetAnimationTime(); }
float AnimatedCamera::Duration() const { CheckNativeCameraThread(); return GetDuration(); }
float AnimatedCamera::TimeLeft() const { CheckNativeCameraThread(); return GetTimeLeft(); }
float AnimatedCamera::FocalDistance() const { CheckNativeCameraThread(); return focal_output_; }
}
