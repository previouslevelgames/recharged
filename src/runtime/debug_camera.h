#pragma once
#include "Game/Camera/DebugCam.h"
#include "runtime/cameras.h"

namespace mscharged
{
enum class DebugCameraControls { Desktop, WiiRemote, WiiFreestyle };

// One diagnostic frame of original pad values, independent of SDL/Wii mappings.
// Pressed and edge values deliberately remain separate, as in the original pad.
struct DebugCameraInputs
{
    DebugCameraControls controls = DebugCameraControls::Desktop;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    float increase_pressure = 0, decrease_pressure = 0;
    float height_up_pressure = 0, height_down_pressure = 0;
    bool increase = false, decrease = false, height_modifier = false;
    bool increase_edge = false, decrease_edge = false;
    bool tweaking = false, profiling = false;
    // The original task-state 0x20000 branch suppresses only pressure height.
    bool suppress_pressure_height = false;
    bool previous_target = false, next_target = false, update_replay_targets = false;
};

struct DebugCameraOrbit
{
    float radius = 10, azimuth = 215, elevation = 25, height = 0;
    float target_x = 0, target_y = 0;
};

// Borrowed by OriginalCameras, with original cDebugCamera control/view behavior.
// Automatic/optional storage avoids conflict with the camera-session allocator.
// Replay targets and Wii DPD acquisition require real game services and reject.
class DebugCamera final : private cDebugCamera
{
    DebugCameraInputs inputs_;
    class Scope;
    void Update(float delta) override;
public:
    DebugCamera();
    ~DebugCamera() override = default;
    DebugCamera(const DebugCamera&) = delete;
    DebugCamera& operator=(const DebugCamera&) = delete;
    static void* operator new(std::size_t) = delete;
    cBaseCamera& Camera() { CheckNativeCameraThread(); return *this; }
    void SetInputs(const DebugCameraInputs& inputs);
    void SetOrbit(const DebugCameraOrbit& orbit);
    DebugCameraOrbit Orbit() const;
    bool ControlsEnabled() const;
    void Advance(float delta);
};

// Explicit service boundary used only by the selected original source.
const DebugCameraInputs& DebugInputs(const cDebugCamera& camera);
void CheckDebugCameraAdvance(const cDebugCamera& camera, float delta);
void CheckDebugCameraTargets(const cDebugCamera& camera);
void CheckDebugCameraLookAt(const nlVector3& position, const nlVector3& target);
void CheckDebugCameraResult(const cDebugCamera& camera);
short DebugCameraAngle(float scaled_angle);
}
