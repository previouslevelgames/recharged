// Drives actual SDL input -> original DebugCam -> native preview rendering.
// Assets remain caller-provided; the registered test creates only synthetic data.
#include "runtime/scene.h"
#include "Game/Camera/CameraMan.h"
#include "NL/gl/gl.h"
#include <SDL3/SDL.h>
#include <array>
#include <charconv>
#include <cmath>
#include <iostream>

namespace
{
struct Controls
{
    SDL_Joystick* joystick = nullptr;
    nlVector3 start{}, orbit{}, reset{}, pan{}, final{};
    std::array<bool, 5> observed{};
    bool raised = false, reported = false, focused = false;
    int phase_start = -1;
    bool lost_focus = false;
    static void Update(void* context)
    {
        auto& self = *static_cast<Controls*>(context);
        if (!self.joystick) return;
        const int frame = glGetCurrentFrame();
        const bool focused = SDL_GetKeyboardFocus() != nullptr;
        self.focused = self.focused || focused;
        // Wayland may grant focus after several rendered frames. Start with
        // neutral input so the production focus latch can rearm before orbit.
        if (self.phase_start < 0 && focused && frame >= 2) self.phase_start = frame + 2;
        const int phase = self.phase_start < 0 ? -1 : frame - self.phase_start;
        if (phase >= 0 && phase <= 59 && !focused) self.lost_focus = true;
        int count = 0; SDL_Window** windows = SDL_GetWindows(&count);
        if (!self.raised && count && !(SDL_GetWindowFlags(windows[0]) & SDL_WINDOW_HIDDEN))
        { SDL_RaiseWindow(windows[0]); self.raised = true; }
        if (!self.reported && frame == 2)
        {
            std::cerr << "Input harness focus=" << (SDL_GetKeyboardFocus() != nullptr)
                << ", window flags=" << (count ? SDL_GetWindowFlags(windows[0]) : 0) << '\n';
            self.reported = true;
        }
        SDL_free(windows);
        SDL_SetJoystickVirtualAxis(self.joystick, SDL_GAMEPAD_AXIS_RIGHTX, phase >= 3 && phase < 20 ? 32767 : 0);
        SDL_SetJoystickVirtualAxis(self.joystick, SDL_GAMEPAD_AXIS_LEFTX, phase >= 35 && phase < 50 ? 16384 : 0);
        SDL_SetJoystickVirtualButton(self.joystick, SDL_GAMEPAD_BUTTON_BACK, phase == 25 || phase == 55);
        const std::array<int, 5> samples{2,23,29,52,59};
        const std::array<nlVector3*, 5> values{&self.start,&self.orbit,&self.reset,&self.pan,&self.final};
        for (unsigned i = 0; i < samples.size(); ++i)
            if (phase == samples[i]) { *values[i] = cCameraManager::m_cameraPosition; self.observed[i] = true; }
    }
};
float Distance(const nlVector3& a, const nlVector3& b)
{ return std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z)); }
}
int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    mscharged::SceneOptions options;
    options.debug_camera = true; options.frames = 180;
    options.world = "/world.tmp.zlib"; options.world_res = "/world.res.zlib";
    options.object_ids = {0x10,0x20};
    if (argc >= 5)
    {
        options.world = argv[2]; options.world_res = argv[3]; options.object_ids.clear();
        for (int i = 4; i < argc; ++i)
        {
            std::string_view value = argv[i]; std::uint32_t id;
            auto result = std::from_chars(value.data(), value.data()+value.size(), id, 16);
            if (result.ec != std::errc{} || result.ptr != value.data()+value.size()) return 2;
            options.object_ids.push_back(id);
        }
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xFFFF/0x4D53");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { std::cerr << SDL_GetError() << '\n'; return 1; }
    Controls controls;
    SDL_VirtualJoystickDesc descriptor; SDL_INIT_INTERFACE(&descriptor);
    descriptor.vendor_id = 0xFFFF; descriptor.product_id = 0x4D53;
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD; descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT)-1; descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT)-1;
    descriptor.name = "Charged preview diagnostic controller";
    descriptor.userdata = &controls; descriptor.Update = Controls::Update;
    auto id = SDL_AttachVirtualJoystick(&descriptor);
    controls.joystick = SDL_OpenJoystick(id);
    if (!id || !controls.joystick) { std::cerr << SDL_GetError() << '\n'; SDL_Quit(); return 1; }
    SDL_SetJoystickVirtualAxis(controls.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(controls.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    int count = 0; auto* pads = SDL_GetGamepads(&count);
    const bool isolated = count == 1 && pads && pads[0] == id; SDL_free(pads);
    if (!isolated) { std::cerr << "Rendered input test requires its isolated virtual gamepad\n"; SDL_Quit(); return 1; }
    const int result = mscharged::RunScenePreview(argc, argv, argv[1], options);
    // RunScenePreview owns Aurora's SDL lifetime, including final SDL_Quit.
    if (SDL_WasInit(SDL_INIT_GAMEPAD))
    { SDL_CloseJoystick(controls.joystick); SDL_DetachVirtualJoystick(id); SDL_Quit(); }
    if (result) return result;
    if (!controls.focused || controls.lost_focus || !controls.observed.back())
    { std::cout << "Rendered input check skipped: compositor did not maintain focus for the complete input sequence\n"; return 77; }
    for (bool observed : controls.observed) if (!observed) { std::cerr << "Missing rendered input sample\n"; return 1; }
    const float orbit = Distance(controls.start,controls.orbit), pan = Distance(controls.reset,controls.pan);
    const float reset = Distance(controls.start,controls.reset), final = Distance(controls.start,controls.final);
    std::cout << "Rendered SDL DebugCam: orbit=" << orbit << ", pan=" << pan << ", reset=" << reset << ", final reset=" << final << '\n';
    if (!(orbit>.01f && pan>.01f && reset<.001f && final<.001f)) return 1;
    return 0;
}
