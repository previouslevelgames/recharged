#pragma once
#include "runtime/debug_camera.h"
#include <SDL3/SDL.h>
#include <array>

namespace mscharged
{
struct DebugCameraDevices
{
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    SDL_JoystickID gamepad = 0;
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axes{};
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
};
struct DebugCameraCapture
{
    bool focused = true, keyboard = false, gamepad = false;
};
struct DebugCameraCommand
{
    DebugCameraInputs inputs;
    bool reset = false;
};
// Deterministic input mapping. Captured/unfocused devices must return to neutral
// before controlling the camera again; edges cannot leak through a UI session.
class DebugCameraInputMap
{
    bool keyboard_blocked_ = true, gamepad_blocked_ = true;
    SDL_JoystickID gamepad_ = 0;
    bool increase_ = false, decrease_ = false, reset_ = false;
public:
    DebugCameraCommand Sample(const DebugCameraDevices&, DebugCameraCapture);
};
// Owns one additional SDL gamepad reference. Aurora retains its own pad handles.
// Destroy before Aurora/SDL shutdown. Poll after SDL events have been serviced.
class DebugCameraInput
{
    SDL_Gamepad* gamepad_ = nullptr;
    SDL_JoystickID preferred_ = 0;
    DebugCameraInputMap mapping_;
public:
    // Zero selects the first available pad; an explicit ID permits isolated diagnostics.
    explicit DebugCameraInput(SDL_JoystickID preferred = 0) : preferred_(preferred) {}
    ~DebugCameraInput();
    DebugCameraInput(const DebugCameraInput&) = delete;
    DebugCameraInput& operator=(const DebugCameraInput&) = delete;
    DebugCameraDevices ReadDevices();
    DebugCameraCommand Poll(SDL_Window*, bool keyboard_capture, bool gamepad_capture);
    bool GamepadConnected() const { return gamepad_ != nullptr; }
};
}
