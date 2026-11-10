#include "runtime/menu_input.h"

#include "platform/desktop_wpad.h"
#include "platform/hotkey_filter.h"
#include "platform/session_log.h"
#include "platform/wpad_sdl.h"

#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <aurora/input.hpp>
#include <aurora/video.h>
#include <imgui.h>
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

// Dear ImGui's SDL3 backend (built with Aurora): an SDL key's ImGui key.
ImGuiKey ImGui_ImplSDL3_KeyEventToImGuiKey(SDL_Keycode keycode, SDL_Scancode scancode);

namespace mscharged::runtime {
namespace {
// Above Aurora's ImGui layer, so the menus' events reach ImGui once.
constexpr std::int32_t kInputLayerPriority = 300;
// The game's pause, pressed again this often while the game refuses it, at
// most this many times (well after the pause menu would have appeared).
constexpr auto kPauseRetry = std::chrono::milliseconds(1200);
constexpr int kPausePresses = 10;
// F1 and F2 presses (the hotkey filter's actions, on the window's thread).
std::atomic<int> sHotkeyPresses{0}, sOptionsPresses{0};

// The players' buttons as ImGui's gamepad keys.
struct ControllerKey {
    std::uint16_t bit;
    ImGuiKey key;
};
constexpr ControllerKey kControllerKeys[] = {
    {WPAD_BUTTON_UP, ImGuiKey_GamepadDpadUp},     {WPAD_BUTTON_DOWN, ImGuiKey_GamepadDpadDown},
    {WPAD_BUTTON_LEFT, ImGuiKey_GamepadDpadLeft}, {WPAD_BUTTON_RIGHT, ImGuiKey_GamepadDpadRight},
    {WPAD_BUTTON_A, ImGuiKey_GamepadFaceDown},    {WPAD_BUTTON_1, ImGuiKey_GamepadFaceLeft},
    {WPAD_BUTTON_2, ImGuiKey_GamepadFaceUp},      {WPAD_BUTTON_MINUS, ImGuiKey_GamepadL1},
    {WPAD_BUTTON_PLUS, ImGuiKey_GamepadR1},       {WPAD_BUTTON_FS_C, ImGuiKey_GamepadL2},
    {WPAD_BUTTON_FS_Z, ImGuiKey_GamepadR2},       {WPAD_BUTTON_B, ImGuiKey_GamepadFaceRight},
};

// SDL window coordinates in the overlay's surface pixels.
ImVec2 SurfacePixels(float x, float y)
{
    const auto size = aurora_get_window_size();
    const float sx = size.width ? float(size.native_fb_width) / float(size.width) : 1.0f;
    const float sy = size.height ? float(size.native_fb_height) / float(size.height) : 1.0f;
    return {x * sx, y * sy};
}

int MouseButton(std::uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return ImGuiMouseButton_Left;
    case SDL_BUTTON_RIGHT: return ImGuiMouseButton_Right;
    case SDL_BUTTON_MIDDLE: return ImGuiMouseButton_Middle;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
    }
}

int LowestChannel(std::uint32_t channels)
{
    for (int n = 0; n < 4; ++n)
        if (channels & (1u << n)) return n;
    return -1;
}
} // namespace

// The menus' input layer: while the pages are open it takes the mouse and the
// keyboard from every layer below (the game's hidden cursor too) and gives
// them to ImGui; the players' buttons come from the WPAD hold instead.
struct MenuInputLayer {
    static aurora::input::EventResult OnEvent(const aurora::input::InputEvent& event, void*)
    {
        using aurora::input::InputEvent;
        using Kind = aurora::input::InputSource::Kind;
        const Kind kind = event.source.kind;
        if ((kind != Kind::Keyboard && kind != Kind::Mouse) || !ImGui::GetCurrentContext())
            return aurora::input::EventResult::Pass;
        ImGuiIO& io = ImGui::GetIO();
        if (event.payload.is<InputEvent::Cancelled>()) {
            if (kind == Kind::Keyboard) io.ClearInputKeys();
            else io.ClearInputMouse();
        } else if (const auto* key = event.payload.get_if<InputEvent::KeyChanged>()) {
            io.AddKeyEvent(ImGuiMod_Ctrl, (key->modifiers & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (key->modifiers & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (key->modifiers & SDL_KMOD_ALT) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (key->modifiers & SDL_KMOD_GUI) != 0);
            const ImGuiKey imguiKey = ImGui_ImplSDL3_KeyEventToImGuiKey(key->keycode, key->scancode);
            io.AddKeyEvent(imguiKey, key->pressed);
            io.SetKeyEventNativeData(imguiKey, int(key->keycode), int(key->scancode), int(key->scancode));
        } else if (const auto* text = event.payload.get_if<InputEvent::TextInput>()) {
            io.AddInputCharactersUTF8(text->text.c_str());
        } else if (const auto* pointer = event.payload.get_if<InputEvent::PointerChanged>()) {
            const ImVec2 at = SurfacePixels(pointer->position.x, pointer->position.y);
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(at.x, at.y);
            using Phase = InputEvent::PointerChanged::Phase;
            if (pointer->phase == Phase::Down || pointer->phase == Phase::Up)
                if (const int button = MouseButton(pointer->button); button >= 0)
                    io.AddMouseButtonEvent(button, pointer->phase == Phase::Down);
        } else if (const auto* scroll = event.payload.get_if<InputEvent::Scroll>()) {
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseWheelEvent(-scroll->delta.x, -scroll->delta.y);
        }
        return aurora::input::EventResult::Consume;
    }
    static bool Captures(const aurora::input::InputSource& source, void*)
    {
        using Kind = aurora::input::InputSource::Kind;
        return source.kind == Kind::Keyboard || source.kind == Kind::Mouse;
    }
    // The menus draw the game's hand instead of the system's arrow.
    static aurora::input::PointerMode Pointer(void*) { return aurora::input::PointerMode::Hidden; }
};

MenuInput::MenuInput(SDL_Window* window) : window_(window)
{
    layer_ = aurora::input::register_layer({
        .label = "recharged.menus",
        .priority = kInputLayerPriority,
        .onEvent = MenuInputLayer::OnEvent,
        .capturesSource = MenuInputLayer::Captures,
        .pointerMode = MenuInputLayer::Pointer,
        .userdata = this,
        .enabled = false,
    });
    sHotkeyPresses = sOptionsPresses = 0;
    platform::SetHotkeyAction(HotkeyPanel, [] { ++sHotkeyPresses; });
    platform::SetHotkeyAction(HotkeyOptions, [] { ++sOptionsPresses; });
    platform::SetNativeWpadHomeTaken(true);
    // RECHARGED_CAPTURE_PANEL=SECONDS[,OPEN] presses F1 once, that long after
    // start, and again after OPEN seconds.
    if (const char* capture = std::getenv("RECHARGED_CAPTURE_PANEL"); capture && *capture) {
        const auto seconds = [](const char* text) {
            return std::chrono::milliseconds(std::lround(std::atof(text) * 1000.0));
        };
        captureAt_ = Clock::now() + seconds(capture);
        if (const char* open = std::strchr(capture, ',')) captureOpen_ = seconds(open + 1);
        captureDone_ = false;
    }
}

MenuInput::~MenuInput()
{
    ClosePages();
    platform::SetNativeWpadHomeTaken(false);
    platform::SetHotkeyAction(HotkeyPanel, nullptr);
    platform::SetHotkeyAction(HotkeyOptions, nullptr);
    if (layer_) aurora::input::unregister_layer(layer_);
}

void MenuInput::SetPauseRule(std::function<bool()> rule) { pauseRule_ = std::move(rule); }

void MenuInput::SetMenuKeys(std::function<void(MenuKey key, int channel)> keys) { menuKeys_ = std::move(keys); }

void MenuInput::Capture(bool on)
{
    if (on) {
        ImGuiIO& io = ImGui::GetIO();
        controllerDown_ = 0;
        keyboardChannel_ = platform::DesktopKeyboardWpadChannel();
        // Drop what reached ImGui while it read no input, then start where the
        // mouse is.
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        float x = 0, y = 0;
        if (window_ && SDL_GetMouseFocus() == window_) {
            SDL_GetMouseState(&x, &y);
            const ImVec2 at = SurfacePixels(x, y);
            io.AddMousePosEvent(at.x, at.y);
        }
        aurora_imgui_set_overlay_input(true);
        if (layer_) aurora::input::set_layer_enabled(layer_, true);
        platform::SetNativeWpadPanelHold(true);
        return;
    }
    platform::SetNativeWpadPanelHold(false);
    platform::SetHotkeysInMenu(false);
    if (layer_) aurora::input::set_layer_enabled(layer_, false);
    aurora_imgui_set_overlay_input(false);
    // (No ImGui any more as the program ends.)
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
}

void MenuInput::OpenPages(int channel, bool pause)
{
    if (pages_) return;
    pages_ = true;
    Capture(true);
    opener_ = channel;
    pausePresses_ = 0;
    if (pause && PressPause()) nextPause_ = Clock::now() + kPauseRetry;
}

void MenuInput::ClosePages()
{
    if (!pages_) return;
    pages_ = false;
    pausePresses_ = 0;
    Capture(false);
}

bool MenuInput::PressPause()
{
    if (!pauseRule_ || !pauseRule_()) return false;
    // The game pauses a match on its 1 button (FrontEnd::UpdateForGame asks
    // for pad button 0x2F, which every Wii Remote layout maps to WPAD 1), not +.
    const int pressed = platform::PressNativeWpadButton(opener_ >= 0 ? opener_ : keyboardChannel_, WPAD_BUTTON_1);
    if (pressed < 0) return false;
    ++pausePresses_;
    platform::SessionLogLine("Menus: pressed the game's pause (player " + std::to_string(pressed + 1)
                             + ", try " + std::to_string(pausePresses_) + ")");
    return true;
}

void MenuInput::RetryPause(Clock::time_point now)
{
    if (pausePresses_ > 0 && pausePresses_ < kPausePresses && now >= nextPause_) {
        if (PressPause()) nextPause_ = now + kPauseRetry;
        else pausePresses_ = 0;
    }
}

void MenuInput::Service(Clock::time_point now)
{
    const bool hotkey = sHotkeyPresses.exchange(0) > 0;
    const bool optionsKey = sOptionsPresses.exchange(0) > 0;
    const std::uint32_t homes = platform::TakeNativeWpadHomePresses();
    const std::uint32_t pauses = platform::TakeNativeWpadPausePresses();
    if (!captureDone_ && now >= captureAt_) {
        captureDone_ = true;
        if (menuKeys_) menuKeys_(MenuKey::Recharged, -1);
        if (captureOpen_.count() > 0) captureClose_ = now + captureOpen_;
        return;
    }
    if (captureClose_ != Clock::time_point{} && now >= captureClose_) {
        captureClose_ = {};
        if (menuKeys_) menuKeys_(MenuKey::Recharged, -1);
    }
    // F1 and HOME (the Recharged menu), F2 (Recharged's gameplay options).
    if ((hotkey || optionsKey || homes) && menuKeys_) {
        const bool options = optionsKey && !hotkey && !homes;
        const int channel = hotkey || optionsKey ? -1 : homes ? LowestChannel(homes) : -1;
        menuKeys_(options ? MenuKey::Options : MenuKey::Recharged, channel);
    } else if (pauses && !pages_ && menuKeys_) menuKeys_(MenuKey::Pause, LowestChannel(pauses));
    if (!pages_) {
        // Without pages the overlay's frames read no input: what Aurora's
        // ImGui layer queued meanwhile would only pile up.
        ImGui::GetIO().ClearEventsQueue();
        return;
    }
    // The pages' input: the players' buttons as keys, the pause pressed
    // again while the game refuses it.
    platform::SetHotkeysInMenu(true);
    FeedControllers(platform::TakeNativeWpadPanelButtons());
    RetryPause(now);
}

void MenuInput::FeedControllers(const platform::NativeWpadPanelButtons& buttons)
{
    // The keyboard & mouse player's keys reach ImGui as keys and clicks; its
    // WPAD buttons (Return as A, a click as A) would press twice.
    ImGuiIO& io = ImGui::GetIO();
    for (std::size_t k = 0; k < std::size(kControllerKeys); ++k) {
        const ControllerKey& entry = kControllerKeys[k];
        // The menus take the keyboard player's 1 too (the game's pause, Z):
        // no ImGui key of its own.
        const bool keyboardToo = entry.bit == WPAD_BUTTON_1;
        bool down = false, pressed = false;
        for (int n = 0; n < 4; ++n) {
            if (n == keyboardChannel_ && !keyboardToo) continue;
            down |= (buttons.down[n] & entry.bit) != 0;
            pressed |= (buttons.pressed[n] & entry.bit) != 0;
        }
        const bool was = controllerDown_ & (1u << k);
        const ImGuiKey key = entry.key;
        if (pressed) {
            // Pressed since the last frame: released first if it was down,
            // and a short tap is released again (ImGui sees one per frame).
            if (was) io.AddKeyEvent(key, false);
            io.AddKeyEvent(key, true);
            if (!down) io.AddKeyEvent(key, false);
        } else if (down != was) io.AddKeyEvent(key, down);
        if (down) controllerDown_ |= 1u << k;
        else controllerDown_ &= ~(1u << k);
    }
    // The Nunchuk stick (about +-100 at full tilt; the game's own dead zone of 15).
    int x = 0, y = 0;
    for (int n = 0; n < 4; ++n) {
        if (n == keyboardChannel_) continue;
        if (std::abs(buttons.stick_x[n]) > std::abs(x)) x = buttons.stick_x[n];
        if (std::abs(buttons.stick_y[n]) > std::abs(y)) y = buttons.stick_y[n];
    }
    const auto axis = [&](ImGuiKey key, int value) {
        const float amount = std::clamp((float(value) - 15.0f) / 56.0f, 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, amount > 0.1f, amount);
    };
    axis(ImGuiKey_GamepadLStickRight, x);
    axis(ImGuiKey_GamepadLStickLeft, -x);
    axis(ImGuiKey_GamepadLStickUp, y);
    axis(ImGuiKey_GamepadLStickDown, -y);
}

} // namespace mscharged::runtime
