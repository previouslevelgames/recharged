#pragma once
#include "runtime/frontend_input.h"
#include <SDL3/SDL.h>
#include <thread>

namespace mscharged
{
struct FrontendInputDevices
{
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    struct Pad
    {
        SDL_JoystickID id = 0;
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
        Sint16 left_x = 0, left_y = 0;
    };
    std::array<Pad, 4> pads;
};
struct FrontendInputCapture { bool focused = true, keyboard = false, gamepad = false; };
// Focus/capture/device changes require a neutral sample before a new press.
class FrontendInputMap
{
    bool keyboard_blocked_ = true;
    std::array<bool, 4> pad_blocked_{true, true, true, true};
    std::array<SDL_JoystickID, 4> ids_{};
    std::uint32_t keyboard_previous_ = 0;
    std::array<std::uint32_t, 4> pad_previous_{};
public:
    std::array<FrontendPadSample, 4> Sample(const FrontendInputDevices&, FrontendInputCapture);
    // Commit focus/neutral/hotplug gates only when the original input update
    // accepts the snapshot. Rejected times/owner calls must not consume gates.
    std::array<FrontendPadSample, 4> Update(FrontendInput&, const FrontendInputDevices&, FrontendInputCapture, float delta);
};
// One successfully applied original FE update. Consumers must not update FE a
// second time or consume its button queries merely to inspect movement.
struct FrontendInputSnapshot
{
    FrontendInputDevices devices;
    std::array<FrontendPadSample, 4> mapped{};
    FrontendInputCapture capture;
    SDL_WindowID window = 0;
    std::uint64_t sequence = 0;
    float delta = 0;
};
// Owns extra SDL references and stable player slots; destroy before SDL shutdown.
class FrontendInputSDL
{
    std::array<SDL_Gamepad*, 4> pads_{};
    FrontendInputMap mapping_;
    FrontendInputSnapshot snapshot_;
    std::thread::id thread_ = std::this_thread::get_id();
public:
    FrontendInputSDL() = default;
    ~FrontendInputSDL();
    FrontendInputSDL(const FrontendInputSDL&) = delete;
    FrontendInputSDL& operator=(const FrontendInputSDL&) = delete;
    FrontendInputDevices ReadDevices();
    const FrontendInputSnapshot& LastSnapshot() const;
    void Poll(FrontendInput&, SDL_Window*, float delta, bool keyboard_capture = false, bool gamepad_capture = false);
};
}
