#include "runtime/debug_camera.h"
#include <cmath>
#include <stdexcept>

namespace mscharged
{
namespace
{
struct Call { const cDebugCamera* camera; const DebugCameraInputs* inputs; };
thread_local const Call* active = nullptr;
constexpr double Limit = 1e7;
void Bounded(float value)
{
    if (!std::isfinite(value) || std::abs(double(value)) > Limit)
        throw std::out_of_range("Debug camera orbit exceeds the finite diagnostic range");
}
void Validate(const DebugCameraOrbit& orbit)
{
    for (float value : {orbit.radius, orbit.azimuth, orbit.elevation,
                        orbit.height, orbit.target_x, orbit.target_y}) Bounded(value);
    if (orbit.radius < .001f || orbit.height < 0 || orbit.elevation < -89 || orbit.elevation > 89)
        throw std::invalid_argument("Debug camera radius, height or elevation is outside its original range");
}
void Validate(const DebugCameraInputs& inputs)
{
    if (inputs.controls != DebugCameraControls::Desktop)
        throw std::invalid_argument("Debug camera Wii controls require the original pad and DPD services");
    if (inputs.previous_target || inputs.next_target || inputs.update_replay_targets)
        throw std::invalid_argument("Debug camera target selection requires player and replay services");
    for (float axis : {inputs.left_x, inputs.left_y, inputs.right_x, inputs.right_y})
        if (!std::isfinite(axis) || axis < -1 || axis > 1)
            throw std::invalid_argument("Debug camera stick values must be in [-1, 1]");
    for (float pressure : {inputs.increase_pressure, inputs.decrease_pressure,
                          inputs.height_up_pressure, inputs.height_down_pressure})
        if (!std::isfinite(pressure) || pressure < 0 || pressure > 1)
            throw std::invalid_argument("Debug camera pressure values must be in [0, 1]");
}
}
class DebugCamera::Scope
{
    Call call_;
public:
    explicit Scope(DebugCamera& camera) : call_{&camera, &camera.inputs_}
    {
        CheckNativeCameraThread();
        if (active) throw std::logic_error("Debug camera call is reentrant");
        active = &call_;
    }
    ~Scope() { active = nullptr; }
};
const DebugCameraInputs& DebugInputs(const cDebugCamera& camera)
{
    CheckNativeCameraThread();
    if (!active || active->camera != &camera)
        throw std::logic_error("Original debug camera requires an explicit native input owner");
    return *active->inputs;
}
void CheckDebugCameraTargets(const cDebugCamera& camera)
{
    DebugInputs(camera);
    if (camera.m_bUpdateTargets || camera.m_pTarget || camera.m_pTargetEntry || camera.m_Targets.m_Head)
        throw std::logic_error("Debug camera targets require player and replay services");
}
void CheckDebugCameraAdvance(const cDebugCamera& camera, float delta)
{
    DebugInputs(camera); CheckCameraDelta(delta); CheckDebugCameraTargets(camera);
    // Bound intermediate control arithmetic before the original mutates state.
    // This covers both pan terms, button/pressure height, and 100 deg/s orbit.
    const double distance = double(delta) * (100 + 4 * (1 + double(camera.m_fRadius) + camera.m_fHeight));
    if (!std::isfinite(distance) || distance > Limit)
        throw std::out_of_range("Debug camera step exceeds the finite diagnostic range");
}
void CheckDebugCameraResult(const cDebugCamera& camera)
{
    DebugInputs(camera);
    Validate({camera.m_fRadius, camera.m_fAzimuth, camera.m_fTheta,
              camera.m_fHeight, camera.m_vecTarget.x, camera.m_vecTarget.y});
    CheckCameraPose(camera);
}
void CheckDebugCameraLookAt(const nlVector3& position, const nlVector3& target)
{
    CheckCameraVector(position); CheckCameraVector(target);
    // Original look-at uses Z up. Small radii at large centers may lose their
    // horizontal separation in float even though every input was finite.
    const double x = double(position.x) - target.x, y = double(position.y) - target.y;
    if (x*x + y*y < 1e-12)
        throw std::invalid_argument("Debug camera look-at direction is parallel to Z up or lost to float precision");
}
short DebugCameraAngle(float scaled_angle)
{
    if (!std::isfinite(scaled_angle)) throw std::out_of_range("Debug camera angle must be finite");
    // Wii truncates to an integer and retains its low 16 bits. Direct float ->
    // s16 is undefined on the host even for the original 215-degree default.
    int value = static_cast<int>(std::fmod(std::trunc(double(scaled_angle)), 65536.0));
    if (value > 32767) value -= 65536;
    if (value < -32768) value += 65536;
    return static_cast<short>(value);
}
DebugCamera::DebugCamera() : cDebugCamera(false)
{
    NativeCameraCall call; Scope scope(*this); cDebugCamera::Update(0);
}
void DebugCamera::SetInputs(const DebugCameraInputs& inputs)
{
    Validate(inputs); NativeCameraCall call; inputs_ = inputs;
}
void DebugCamera::SetOrbit(const DebugCameraOrbit& orbit)
{
    Validate(orbit); NativeCameraCall call;
    m_fRadius = orbit.radius; m_fAzimuth = orbit.azimuth; m_fTheta = orbit.elevation;
    m_fHeight = orbit.height; m_vecTarget = {orbit.target_x, orbit.target_y, orbit.height};
    // Rebuild the pose without consuming held/edge input a second time.
    const auto inputs = inputs_; inputs_ = {};
    try { Scope scope(*this); cDebugCamera::Update(0); }
    catch (...) { inputs_ = inputs; throw; }
    inputs_ = inputs;
}
DebugCameraOrbit DebugCamera::Orbit() const
{
    CheckNativeCameraThread();
    return {m_fRadius, m_fAzimuth, m_fTheta, m_fHeight, m_vecTarget.x, m_vecTarget.y};
}
bool DebugCamera::ControlsEnabled() const { CheckNativeCameraThread(); return m_bEnableControls; }
void DebugCamera::Advance(float delta)
{
    CheckCameraDelta(delta); NativeCameraCall call; Scope scope(*this); cDebugCamera::Update(delta);
}
void DebugCamera::Update(float delta)
{
    Scope scope(*this); cDebugCamera::Update(delta);
}
}
