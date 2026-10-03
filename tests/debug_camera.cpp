#include "runtime/debug_camera.h"
#include "runtime/startup.h"
#include "NL/MemAlloc.h"
#include "Game/Camera/CameraMan.h"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks = 0;
constexpr double Pi = 3.14159265358979323846;
void Check(bool value, const char* message)
{ ++checks; if (!value) throw std::runtime_error(message); }
void Near(float actual, float expected, float tolerance = .0002f)
{
    ++checks;
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error("Debug camera value " + std::to_string(actual) + " != " + std::to_string(expected));
}
template<class F> void Reject(F fn, const char* contains)
{
    try { fn(); }
    catch (const std::exception& e)
    { Check(std::string(e.what()).find(contains) != std::string::npos, "Wrong diagnostic error"); return; }
    throw std::runtime_error("Invalid debug camera input accepted");
}
void ViewOrigin(cBaseCamera& camera)
{
    const auto& p = camera.GetCameraPosition(); const auto& m = camera.GetViewMatrix();
    Near(p.x*m.m11+p.y*m.m21+p.z*m.m31+m.m41, 0, .004f);
    Near(p.x*m.m12+p.y*m.m22+p.z*m.m32+m.m42, 0, .004f);
    Near(p.x*m.m13+p.y*m.m23+p.z*m.m33+m.m43, 0, .004f);
    // Unit, perpendicular world-space axes; this also catches a singular basis.
    Near(m.m11*m.m11+m.m21*m.m21+m.m31*m.m31, 1);
    Near(m.m12*m.m12+m.m22*m.m22+m.m32*m.m32, 1);
    Near(m.m13*m.m13+m.m23*m.m23+m.m33*m.m33, 1);
    Near(m.m11*m.m12+m.m21*m.m22+m.m31*m.m32, 0);
}
void Pose()
{
    OriginalCameras core; DebugCamera camera;
    const auto initial = camera.Orbit();
    Near(initial.radius, 10); Near(initial.azimuth, 215); Near(initial.elevation, 25); Near(initial.height, 0);
    Near(camera.Camera().GetFOV(), 60); Check(camera.ControlsEnabled(), "Controls are not initially enabled");
    Check(camera.Camera().GetType() == eCameraType_Debug, "Original camera type changed");
    // Independent double-precision polar oracle, with an error allowance for the
    // original fixed-angle truncation and lookup-table interpolation.
    for (float azimuth : {-1080.f, -360.f, -215.f, 0.f, 90.f, 215.f, 360.f, 1080.f})
        for (float elevation : {-89.f, -25.f, 0.f, 25.f, 89.f})
        {
            camera.SetOrbit({5, azimuth, elevation, 3, 7, -2});
            const auto& p = camera.Camera().GetCameraPosition();
            const double a = azimuth*Pi/180, e = elevation*Pi/180;
            Near(p.x, float(7+5*std::cos(e)*std::cos(a)), .0015f);
            Near(p.y, float(-2+5*std::cos(e)*std::sin(a)), .0015f);
            Near(p.z, float(3+5*std::sin(e)), .0015f);
            Near(camera.Camera().GetTargetPosition().x, 7);
            Near(camera.Camera().GetTargetPosition().y, -2);
            Near(camera.Camera().GetTargetPosition().z, 3); ViewOrigin(camera.Camera());
        }
}
void Controls()
{
    OriginalCameras core; DebugCamera camera; DebugCameraInputs input;
    camera.SetOrbit({10, 0, 0, 0, 0, 0});
    input.right_x = .25f; input.right_y = -.5f;
    camera.SetInputs(input); camera.Advance(.2f);
    Near(camera.Orbit().azimuth, 5); Near(camera.Orbit().elevation, -10);
    input.right_y = 1; camera.SetInputs(input); camera.Advance(2);
    Near(camera.Orbit().elevation, 89);
    input.right_y = -1; camera.SetInputs(input); camera.Advance(3);
    Near(camera.Orbit().elevation, -89);

    // At zero azimuth/elevation camera-right is world +Y; forward pan is -X.
    camera.SetOrbit({10, 0, 0, 0, 0, 0});
    input = {}; input.left_x = .5f; input.left_y = 1;
    camera.SetInputs(input); camera.Advance(.25f);
    Near(camera.Orbit().target_x, -2.75f); Near(camera.Orbit().target_y, 1.375f);
    Near(camera.Orbit().height, 0);

    // The pan step uses the prior view, even while this frame rotates 90 degrees.
    camera.SetOrbit({10, 0, 0, 0, 0, 0});
    input = {}; input.right_x = .9f; input.left_x = 1;
    camera.SetInputs(input); camera.Advance(1);
    Near(camera.Orbit().azimuth, 90); Near(camera.Orbit().target_x, 0);
    Near(camera.Orbit().target_y, 11);
    ViewOrigin(camera.Camera());

    camera.SetOrbit({10, 0, 0, 2, 0, 0});
    input = {}; input.increase = true; input.height_up_pressure = .5f;
    camera.SetInputs(input); camera.Advance(.5f);
    Near(camera.Orbit().radius, 16.5f); Near(camera.Orbit().height, 5.25f);
    // Both controls use speed 13 from the start of the same frame.
    camera.SetOrbit({10, 0, 0, 2, 0, 0});
    input.height_modifier = true; camera.SetInputs(input); camera.Advance(.5f);
    Near(camera.Orbit().radius, 10); Near(camera.Orbit().height, 11.75f);
    camera.SetOrbit({10, 0, 0, 2, 0, 0});
    input.suppress_pressure_height = true; camera.SetInputs(input); camera.Advance(.5f);
    Near(camera.Orbit().height, 8.5f);
    input = {}; input.decrease = true; camera.SetInputs(input); camera.Advance(10);
    Near(camera.Orbit().radius, .001f);
    input.height_modifier = true; input.height_down_pressure = 1;
    camera.SetInputs(input); camera.Advance(10); Near(camera.Orbit().height, 0);
    ViewOrigin(camera.Camera());
}
void ToggleAndPause()
{
    OriginalCameras core; DebugCamera camera; DebugCameraInputs input;
    input.increase_pressure = 1; input.decrease_edge = true; input.right_x = 1;
    camera.SetInputs(input); camera.Advance(.5f);
    Check(!camera.ControlsEnabled(), "Original chord did not disable controls");
    Near(camera.Orbit().azimuth, 215);
    // Rebuilding a pose cannot consume an edge a second time.
    camera.SetOrbit({10, 0, 0, 0, 0, 0});
    Check(!camera.ControlsEnabled(), "SetOrbit consumed an input edge");
    input = {}; input.decrease_pressure = .25f; input.increase_edge = true; input.right_x = 1;
    camera.SetInputs(input); camera.Advance(.5f);
    Check(camera.ControlsEnabled(), "Opposite original chord did not enable controls");
    Near(camera.Orbit().azimuth, 50);
    input.tweaking = true; camera.SetInputs(input); camera.Advance(1);
    Near(camera.Orbit().azimuth, 50); Check(camera.ControlsEnabled(), "Tweaker pause consumed toggle");
    input.tweaking = false; input.profiling = true; camera.SetInputs(input); camera.Advance(1);
    Near(camera.Orbit().azimuth, 50);
    input = {}; input.increase_edge = true; input.right_x = 1;
    camera.SetInputs(input); camera.Advance(.1f);
    Check(camera.ControlsEnabled(), "Edge without opposite pressure toggled controls"); Near(camera.Orbit().azimuth, 60);
}
void Ownership()
{
    OriginalCameras core;
    {
        DebugCamera first; cCameraManager::PushCamera(&first.Camera());
        DebugCameraInputs input; input.right_x = .5f; first.SetInputs(input);
        core.Advance(.1f, .1f); Near(first.Orbit().azimuth, 220);
        Near(cCameraManager::m_cameraPosition.x, first.Camera().GetCameraPosition().x);
        Near(cCameraManager::m_fFOV, 60);
        {
            DebugCamera second; second.SetOrbit({2, 90, 0, 1, 3, 4});
            cCameraManager::PushCamera(&second.Camera()); core.Advance(0, 0);
            Near(cCameraManager::m_cameraPosition.y, 6); Near(first.Orbit().azimuth, 220);
        }
        Check(cCameraManager::PeekCamera() == &first.Camera(), "Borrowed camera destructor did not unlink");
        core.Advance(.1f, .1f); Near(first.Orbit().azimuth, 225);
        bool rejected = false;
        std::thread wrong([&]{try{first.Orbit();}catch(const std::logic_error&){rejected=true;}}); wrong.join();
        Check(rejected, "Wrong-thread camera access accepted");
    }
    Check(cCameraManager::PeekCamera() == nullptr, "Destroyed debug camera survived on stack");
}
void Invalid()
{
    OriginalCameras core; DebugCamera camera;
    for (float bad : {-2.f, 2.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    { DebugCameraInputs i; i.left_x = bad; Reject([&]{camera.SetInputs(i);}, "stick"); }
    for (float bad : {-1.f, 2.f, std::numeric_limits<float>::quiet_NaN()})
    { DebugCameraInputs i; i.height_down_pressure = bad; Reject([&]{camera.SetInputs(i);}, "pressure"); }
    for (auto unsupported : {DebugCameraControls::WiiRemote, DebugCameraControls::WiiFreestyle})
    { DebugCameraInputs i; i.controls = unsupported; Reject([&]{camera.SetInputs(i);}, "DPD services"); }
    { DebugCameraInputs i; i.previous_target = true; Reject([&]{camera.SetInputs(i);}, "replay services"); }
    { DebugCameraInputs i; i.next_target = true; Reject([&]{camera.SetInputs(i);}, "replay services"); }
    { DebugCameraInputs i; i.update_replay_targets = true; Reject([&]{camera.SetInputs(i);}, "replay services"); }
    for (auto orbit : {DebugCameraOrbit{0, 0, 0, 0}, DebugCameraOrbit{1, 0, 90, 0},
                       DebugCameraOrbit{1, 0, 0, -1}})
        Reject([&]{camera.SetOrbit(orbit);}, "original range");
    for (float bad : {std::numeric_limits<float>::max(), std::numeric_limits<float>::infinity()})
    { DebugCameraOrbit o; o.azimuth = bad; Reject([&]{camera.SetOrbit(o);}, "finite diagnostic range"); }
    camera.Advance(0); // Validation failures above do not poison a usable owner.
    cDebugCamera raw(false); Reject([&]{raw.Update(0);}, "input owner");
}
void Angles()
{
    for (int integer = -262144; integer <= 262144; integer += 17)
    {
        const auto low = static_cast<std::uint16_t>(integer);
        const int expected = low < 32768 ? int(low) : int(low)-65536;
        Near(DebugCameraAngle(float(integer)), float(expected), 0);
    }
    Check(DebugCameraAngle(32768) == -32768, "Signed angle half-turn differs");
    Check(DebugCameraAngle(-32769) == 32767, "Negative angle wrapping differs");
    Check(DebugCameraAngle(65536) == 0, "Angle full-turn differs");
    Check(DebugCameraAngle(65535.9f) == -1, "Angle truncation differs");
    Check(DebugCameraAngle(-65535.9f) == 1, "Negative angle truncation differs");
    Check(DebugCameraAngle(std::numeric_limits<float>::max()) == 0, "Extreme finite angle was unsafe");
    Reject([]{DebugCameraAngle(std::numeric_limits<float>::infinity());}, "finite");
    Reject([]{DebugCameraAngle(std::numeric_limits<float>::quiet_NaN());}, "finite");
}
void FailedSession()
{
    OriginalCameras core; DebugCamera camera; cCameraManager::PushCamera(&camera.Camera());
    core.Advance(0, 0);
    const auto published = cCameraManager::m_cameraPosition;
    Reject([&]{camera.SetOrbit({.001f, 0, 0, 0, 1e7f, 1e7f});}, "float precision");
    // An exception after entering original code invalidates this session. It
    // cannot publish a partially rebuilt pose, but normal RAII teardown works.
    Reject([&]{core.Advance(0, 0);}, "busy or failed");
    Near(cCameraManager::m_cameraPosition.x, published.x);
    Near(cCameraManager::m_cameraPosition.y, published.y);
    Near(cCameraManager::m_cameraPosition.z, published.z);
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024), mem2(1024*1024);
        StandardAllocator.Initialize(mem1.data(), mem1.size()*8); VirtualAllocator.Initialize(mem2.data(), mem2.size()*8); gMemoryInitialized=1;
        Angles();
        for (int i=0; i<3; ++i)
        {
            Pose(); Controls(); ToggleAndPause(); Ownership(); Invalid(); FailedSession();
            {OriginalCameras core; DebugCamera c; Reject([&]{c.Advance(1e30f);}, "finite diagnostic range");}
            {OriginalCameras core; DebugCamera c; Reject([&]{c.Advance(-1);}, "nonnegative");}
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,
                  "Debug camera sessions did not recover both arenas");
        }
        ResetStartupMemory(); std::cout << checks << " original debug camera checks passed\n"; return 0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
