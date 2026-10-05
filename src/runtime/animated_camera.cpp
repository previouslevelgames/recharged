#include "runtime/animated_camera.h"
#include "runtime/graphics_math.h"
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
float CheckedTargetFloat(double value)
{
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        throw std::overflow_error("Animated camera target arithmetic exceeds the finite float range");
    return static_cast<float>(value);
}
void CheckSampleTime(const cAnimCamera& camera, float time)
{
    // Original code converts the index to int even in the endpoint branch.
    const double index = double(time) * (camera.m_pActiveCameraData->m_uKeyCount - 1);
    if (!std::isfinite(time) || time < 0 || index > std::numeric_limits<int>::max() - 256.0)
        throw std::out_of_range("Animated camera time exceeds the original sample index range");
}
void CheckTargetSample(const cAnimCamera& camera, float time)
{
    const auto& data = *camera.m_pActiveCameraData;
    const float frame = time * float(data.m_uKeyCount - 1);
    unsigned a, b; float wa = 1, wb = 0;
    if (time >= 1) a = b = data.m_uKeyCount - 1;
    else
    {
        a = static_cast<unsigned>(frame); b = a + 1; wb = frame - float(a); wa = 1 - wb;
        nlVector3 delta; nlVec3Sub(delta, data.cameraPos[b], data.cameraPos[a]);
        if (delta.GetLengthSq3D() > 16)
        {
            a = b = wb < .5f ? a : b; wa = 1; wb = 0;
        }
    }
    const auto& first = data.targetPos[a]; const auto& second = data.targetPos[b];
    const float av[]{first.x, first.y, first.z}, bv[]{second.x, second.y, second.z};
    const float mirror[]{camera.m_Mirror.x, camera.m_Mirror.y, camera.m_Mirror.z};
    const float offset[]{camera.m_OffsetPos.x, camera.m_OffsetPos.y, camera.m_OffsetPos.z};
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        // Validate the original separate float products, sum, mirror and offset
        // without first overflowing them. Double exactly represents each product
        // of two float operands; each checked cast models the original rounding.
        const float value = a == b ? av[axis] : CheckedTargetFloat(
            double(CheckedTargetFloat(double(wa) * av[axis])) + CheckedTargetFloat(double(wb) * bv[axis]));
        CheckedTargetFloat(double(CheckedTargetFloat(double(value) * mirror[axis])) + offset[axis]);
    }
}
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
    CheckSampleTime(camera, time); CheckTargetSample(camera, time);
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
    CheckSampleTime(camera, camera.m_fAnimationTime + (delta * camera.m_fAnimationSpeed) / camera.GetDuration());
}
void CheckAnimatedLookAt(const nlVector3& position, const nlVector3& target)
{
    CheckCameraVector(position); CheckCameraVector(target);
    // Preserve original float subtraction/length arithmetic while checking its
    // range in double before normalization. Finite exported targets can exceed
    // that range even though quaternion orientation does not consume them.
    const float x = CheckedTargetFloat(double(position.x) - target.x);
    const float y = CheckedTargetFloat(double(position.y) - target.y);
    const float z = CheckedTargetFloat(double(position.z) - target.z);
    if (double(x)*x + double(y)*y < 1e-12)
        throw std::invalid_argument("Animated camera look-at direction is parallel to Z up");
    const float xy = CheckedTargetFloat(double(CheckedTargetFloat(double(x)*x)) + CheckedTargetFloat(double(y)*y));
    const float squared = CheckedTargetFloat(double(xy) + CheckedTargetFloat(double(z)*z));
    if (squared <= 0) throw std::invalid_argument("Animated camera look-at direction underflows");
    const float inverse_length = 1.f / nlSqrt(squared, true);
    const float nx = x * inverse_length, ny = y * inverse_length;
    const float side_squared = nx*nx + ny*ny;
    if (side_squared == 0)
        throw std::invalid_argument("Animated camera look-at horizontal direction underflows");
    // nlRecipSqrt normalizes the side vector with three float Newton steps.
    // Even a nonzero subnormal side length can overflow its estimate squared.
    float estimate = CheckedTargetFloat(NativeReciprocalSqrtEstimate(side_squared));
    for (unsigned i = 0; i < 3; ++i)
    {
        const float square = CheckedTargetFloat(double(estimate)*estimate);
        const float correction = CheckedTargetFloat(3. - CheckedTargetFloat(double(square)*side_squared));
        estimate = CheckedTargetFloat(double(CheckedTargetFloat(.5*estimate))*correction);
    }
}
void RecordAnimatedFocal(const cAnimCamera& camera, float distance)
{
    AnimatedInputs(camera);
    CheckCameraPose(camera); CheckCameraVector(camera.GetTargetPosition());
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
void AnimatedCamera::Select(CameraAsset::Handle asset)
{
    if (!asset) throw std::invalid_argument("Animated camera selection needs a retained asset");
    NativeCameraCall call;
    const auto* data = &asset->Data();
    asset_ = std::move(asset);
    m_fLastSimulationTime = -1.0f;
    m_pActiveCameraData = const_cast<cCameraData*>(data);
}
void AnimatedCamera::SetCyclic(bool value)
{
    NativeCameraCall call; m_bCyclic = value;
}
void AnimatedCamera::SetEndCallback(void (*callback)())
{
    NativeCameraCall call; m_EndOfAnimationCallback = callback;
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
