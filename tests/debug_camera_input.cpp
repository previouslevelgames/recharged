#include "runtime/debug_camera_input.h"
#include "runtime/startup.h"
#include "NL/MemAlloc.h"
#include "Game/Camera/CameraMan.h"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void Near(float value, float expected, float tolerance = .0001f)
{ Check(std::isfinite(value) && std::abs(value-expected) <= tolerance, "Unexpected mapped value"); }
void Keyboard()
{
    DebugCameraDevices d; DebugCameraInputMap map; map.Sample(d, {});
    d.keys[SDL_SCANCODE_W] = d.keys[SDL_SCANCODE_RIGHT] = true;
    auto command = map.Sample(d, {}); Near(command.inputs.left_y, 1); Near(command.inputs.right_x, 1);
    d.keys[SDL_SCANCODE_Q] = true; command = map.Sample(d, {});
    Check(command.inputs.decrease_edge, "New Q edge missing");
    Check(!map.Sample(d, {}).inputs.decrease_edge, "Held key repeats an edge");
    d.keys[SDL_SCANCODE_E] = true; command = map.Sample(d, {});
    Check(command.inputs.increase_edge && command.inputs.decrease_pressure == 1, "Original toggle chord missing");
    d.keys[SDL_SCANCODE_R] = true; Check(map.Sample(d, {}).reset, "Reset edge missing");
    Check(!map.Sample(d, {}).reset, "Held reset repeats");
    d.keys[SDL_SCANCODE_LSHIFT] = true; Check(map.Sample(d, {}).inputs.height_modifier, "Height modifier missing");
    for (auto capture : {DebugCameraCapture{false,false,false}, DebugCameraCapture{true,true,false}})
    {
        command = map.Sample(d, capture); Near(command.inputs.right_x, 0); Near(command.inputs.left_y, 0);
        Check(!command.reset && !command.inputs.increase_edge, "Captured edge leaked");
        command = map.Sample(d, {}); Near(command.inputs.right_x, 0);
        d = {}; map.Sample(d, {}); d.keys[SDL_SCANCODE_RIGHT] = true;
        Near(map.Sample(d, {}).inputs.right_x, 1);
    }
}
void PadAndCapture()
{
    DebugCameraDevices d; d.gamepad = 12; DebugCameraInputMap map; map.Sample(d, {});
    d.axes[SDL_GAMEPAD_AXIS_RIGHTX] = 5000; Near(map.Sample(d, {}).inputs.right_x, 0);
    d.axes[SDL_GAMEPAD_AXIS_RIGHTX] = 32767; Near(map.Sample(d, {}).inputs.right_x, 1);
    d.axes[SDL_GAMEPAD_AXIS_RIGHTY] = -32768; Near(map.Sample(d, {}).inputs.right_y, 1);
    d.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767; Near(map.Sample(d, {}).inputs.height_down_pressure, 1);
    d.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = -32768; Near(map.Sample(d, {}).inputs.height_up_pressure, 0);
    d.buttons[SDL_GAMEPAD_BUTTON_BACK] = true; Check(map.Sample(d, {}).reset, "Pad reset missing");
    Near(map.Sample(d, {true,false,true}).inputs.right_x, 0);
    Near(map.Sample(d, {}).inputs.right_x, 0); // Returning from UI requires neutral.
    d = {}; d.gamepad = 12; map.Sample(d, {});
    d.keys[SDL_SCANCODE_D] = true; d.axes[SDL_GAMEPAD_AXIS_LEFTX] = 32767;
    Near(map.Sample(d, {}).inputs.left_x, 1); // Combined values remain bounded.
    Near(map.Sample(d, {true,true,false}).inputs.left_x, 1); // Keyboard capture leaves pad usable.
    d.gamepad = 0; Near(map.Sample(d, {}).inputs.left_x, 0); // Old pad axes cannot survive disconnect.
    d.gamepad = 13; Near(map.Sample(d, {}).inputs.left_x, 0); // New held device must neutralize.
    d = {}; d.gamepad = 13; map.Sample(d, {});
    d.axes[SDL_GAMEPAD_AXIS_LEFTX] = -32768; Near(map.Sample(d, {}).inputs.left_x, -1);
    d.keys[SDL_SCANCODE_D] = true; Near(map.Sample(d, {}).inputs.left_x, 0);
    // Axis range/deadzone is monotonic and bounded across all signed samples.
    d = {}; d.gamepad = 13; map.Sample(d, {}); float previous = -1;
    for (int sample = -32768; sample <= 32767; ++sample)
    {
        d.axes[SDL_GAMEPAD_AXIS_RIGHTX] = Sint16(sample);
        const float axis = map.Sample(d, {}).inputs.right_x;
        Check(axis >= previous && axis >= -1 && axis <= 1, "Stick mapping escaped its monotonic range"); previous = axis;
    }
}
void NativePose()
{
    OriginalCameras core; DebugCamera camera; DebugCameraDevices d; DebugCameraInputMap map;
    cCameraManager::PushCamera(&camera.Camera()); map.Sample(d, {});
    d.keys[SDL_SCANCODE_RIGHT] = true;
    camera.SetInputs(map.Sample(d, {}).inputs); core.Advance(.5f, .5f); Near(camera.Orbit().azimuth, 265);
    d.keys[SDL_SCANCODE_E] = d.keys[SDL_SCANCODE_Q] = true;
    camera.SetInputs(map.Sample(d, {}).inputs); core.Advance(.5f, .5f);
    Check(!camera.ControlsEnabled(), "Desktop chord did not reach original toggle"); Near(camera.Orbit().azimuth, 265);
    camera.SetInputs(map.Sample(d, {}).inputs); core.Advance(.5f, .5f);
    Check(!camera.ControlsEnabled(), "Held desktop chord retriggered original toggle");
    d.keys[SDL_SCANCODE_Q] = false; camera.SetInputs(map.Sample(d, {}).inputs); core.Advance(0,0);
    d.keys[SDL_SCANCODE_Q] = true; camera.SetInputs(map.Sample(d, {}).inputs); core.Advance(.1f,.1f);
    Check(camera.ControlsEnabled(), "Repressed desktop chord did not restore controls"); Near(camera.Orbit().azimuth, 275);
}
void VirtualPad()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_VirtualJoystickDesc descriptor; SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD; descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT)-1; descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT)-1;
    descriptor.name = "Charged debug camera test controller";
    const auto id = SDL_AttachVirtualJoystick(&descriptor); Check(id != 0, SDL_GetError());
    auto* joystick = SDL_OpenJoystick(id); Check(joystick != nullptr, SDL_GetError());
    Check(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768), SDL_GetError());
    Check(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768), SDL_GetError());
    SDL_UpdateJoysticks(); SDL_PumpEvents();
    {
        DebugCameraInput reader(id); DebugCameraInputMap map;
        auto state = reader.ReadDevices();
        Check(state.gamepad == id, "Explicit virtual controller selection failed");
        map.Sample(state, {});
        Check(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 32767), SDL_GetError());
        Check(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true), SDL_GetError());
        SDL_UpdateJoysticks(); SDL_PumpEvents();
        auto command = map.Sample(reader.ReadDevices(), {});
        Near(command.inputs.right_x, 1); Check(command.inputs.increase_edge, "SDL shoulder edge missing");
        Check(!map.Sample(reader.ReadDevices(), {}).inputs.increase_edge, "SDL held shoulder repeated");
        Check(SDL_DetachVirtualJoystick(id), SDL_GetError()); SDL_PumpEvents();
        state = reader.ReadDevices(); Check(state.gamepad == 0, "Disconnected SDL pad retained");
        command = map.Sample(state, {}); Near(command.inputs.right_x, 0);
        Check(!command.inputs.increase && !command.inputs.increase_edge, "Disconnected SDL button retained");
    }
    SDL_CloseJoystick(joystick); SDL_Quit();
}
}
int main()
{
    try
    {
        Keyboard(); PadAndCapture(); VirtualPad();
        std::vector<std::uint64_t> mem1(1024*1024), mem2(1024*1024);
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8); VirtualAllocator.Initialize(mem2.data(),mem2.size()*8); gMemoryInitialized=1;
        for (int i=0;i<3;++i) NativePose();
        Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8, "Input camera tests leaked arenas");
        ResetStartupMemory(); std::cout << checks << " debug camera input checks passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
