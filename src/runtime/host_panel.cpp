#include "runtime/host_panel.h"

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
// Above Aurora's ImGui layer, so the panel's events reach ImGui once.
constexpr std::int32_t kPanelLayerPriority = 300;
// The launcher's colours, as the challenge card's.
constexpr ImU32 kDim = IM_COL32(4, 7, 12, 96);
constexpr ImU32 kCard = IM_COL32(18, 25, 41, 238);
constexpr ImU32 kCardBorder = IM_COL32(36, 48, 72, 255);
constexpr ImU32 kCardText = IM_COL32(234, 240, 250, 255);
constexpr ImU32 kCardMuted = IM_COL32(139, 151, 176, 255);
constexpr ImU32 kAccent = IM_COL32(76, 240, 122, 255);
constexpr ImU32 kButton = IM_COL32(24, 33, 53, 255);
constexpr ImU32 kButtonHover = IM_COL32(34, 46, 72, 255);
constexpr ImU32 kButtonActive = IM_COL32(44, 60, 92, 255);
// The game's pause, pressed again this often while the game refuses it, at
// most this many times (well after the pause menu would have appeared).
constexpr auto kPauseRetry = std::chrono::milliseconds(1200);
constexpr int kPausePresses = 10;
// F1 presses (the hotkey filter's action, on the window's thread).
std::atomic<int> sHotkeyPresses{0};

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
    {WPAD_BUTTON_FS_Z, ImGuiKey_GamepadR2},
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

// The panel's input layer: while open it takes the mouse and the keyboard
// from every layer below (the game's hidden cursor too) and gives them to
// ImGui; the players' buttons come from the WPAD hold instead.
struct HostPanelLayer {
    static aurora::input::EventResult OnEvent(const aurora::input::InputEvent& event, void* userdata)
    {
        using aurora::input::InputEvent;
        using Kind = aurora::input::InputSource::Kind;
        auto& panel = *static_cast<HostPanel*>(userdata);
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
            // Escape closes, unless it ends typing in a text field first.
            if (key->pressed && !key->repeat && key->scancode == SDL_SCANCODE_ESCAPE && !panel.typing_)
                panel.closeRequested_ = true;
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
    static aurora::input::PointerMode Pointer(void*) { return aurora::input::PointerMode::Visible; }
};

HostPanel::HostPanel(SDL_Window* window) : window_(window)
{
    layer_ = aurora::input::register_layer({
        .label = "recharged.panel",
        .priority = kPanelLayerPriority,
        .onEvent = HostPanelLayer::OnEvent,
        .capturesSource = HostPanelLayer::Captures,
        .pointerMode = HostPanelLayer::Pointer,
        .userdata = this,
        .enabled = false,
    });
    sHotkeyPresses = 0;
    platform::SetHotkeyAction(HotkeyPanel, [] { ++sHotkeyPresses; });
    platform::SetNativeWpadHomeTaken(true);
    // RECHARGED_CAPTURE_PANEL=SECONDS[,OPEN] opens the panel once, that long
    // after start, and closes it after OPEN seconds.
    if (const char* capture = std::getenv("RECHARGED_CAPTURE_PANEL"); capture && *capture) {
        const auto seconds = [](const char* text) {
            return std::chrono::milliseconds(std::lround(std::atof(text) * 1000.0));
        };
        captureAt_ = Clock::now() + seconds(capture);
        if (const char* open = std::strchr(capture, ',')) captureOpen_ = seconds(open + 1);
        captureDone_ = false;
    }
}

HostPanel::~HostPanel()
{
    Close();
    platform::SetNativeWpadHomeTaken(false);
    platform::SetHotkeyAction(HotkeyPanel, nullptr);
    if (layer_) aurora::input::unregister_layer(layer_);
}

void HostPanel::SetContent(Content content) { content_ = std::move(content); }

void HostPanel::SetOverlay(Overlay overlay) { overlay_ = std::move(overlay); }

void HostPanel::SetPauseRule(std::function<bool()> rule) { pauseRule_ = std::move(rule); }

void HostPanel::Open() { OpenFrom(-1); }

void HostPanel::OpenFrom(int channel)
{
    if (open_) return;
    open_ = true;
    justOpened_ = true;
    closeRequested_ = typing_ = false;
    controllerDown_ = 0;
    keyboardChannel_ = platform::DesktopKeyboardWpadChannel();
    byController_ = channel >= 0 && channel != keyboardChannel_;
    opener_ = channel;
    ImGuiIO& io = ImGui::GetIO();
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
    aurora_set_video_output_blur(true);
    // In an offline match, the game's own pause, pressed by whoever opened it.
    pausePresses_ = 0;
    if (PressPause()) nextPause_ = Clock::now() + kPauseRetry;
}

bool HostPanel::PressPause()
{
    if (!pauseRule_ || !pauseRule_()) return false;
    // The game pauses a match on its 1 button (FrontEnd::UpdateForGame asks
    // for pad button 0x2F, which every Wii Remote layout maps to WPAD 1), not +.
    const int pressed = platform::PressNativeWpadButton(opener_ >= 0 ? opener_ : keyboardChannel_, WPAD_BUTTON_1);
    if (pressed < 0) return false;
    ++pausePresses_;
    platform::SessionLogLine("Online panel: pressed the game's pause (player " + std::to_string(pressed + 1)
                             + ", try " + std::to_string(pausePresses_) + ")");
    return true;
}

void HostPanel::Close()
{
    if (!open_) return;
    open_ = false;
    platform::SetNativeWpadPanelHold(false);
    platform::SetHotkeysTyping(false);
    aurora_set_video_output_blur(false);
    if (layer_) aurora::input::set_layer_enabled(layer_, false);
    aurora_imgui_set_overlay_input(false);
    // A text field's typing (ImGui started SDL's text input for it) ends too.
    if (window_ && SDL_TextInputActive(window_)) SDL_StopTextInput(window_);
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }
}

void HostPanel::Service(Clock::time_point now)
{
    const bool hotkey = sHotkeyPresses.exchange(0) > 0;
    std::uint32_t homes = platform::TakeNativeWpadHomePresses();
    if (!captureDone_ && now >= captureAt_) {
        captureDone_ = true;
        if (!open_) OpenFrom(-1);
        if (captureOpen_.count() > 0) captureClose_ = now + captureOpen_;
        return;
    }
    if (captureClose_ != Clock::time_point{} && now >= captureClose_) {
        captureClose_ = {};
        closeRequested_ = true;
    }
    if (!open_) {
        // Closed, the overlay's frames read no input: what Aurora's ImGui
        // layer queued meanwhile would only pile up.
        ImGui::GetIO().ClearEventsQueue();
        if (hotkey) OpenFrom(-1);
        else if (homes) OpenFrom(LowestChannel(homes));
        return;
    }
    // While a text field has the keyboard, the keyboard's own HOME key and
    // the other hotkeys' keys type.
    platform::SetHotkeysTyping(typing_);
    if (typing_ && keyboardChannel_ >= 0) homes &= ~(1u << keyboardChannel_);
    const auto buttons = platform::TakeNativeWpadPanelButtons();
    // B closes the panel's own card; the overlay takes it as a key (back).
    bool back = false;
    for (int n = 0; n < 4 && !overlay_; ++n)
        if (n != keyboardChannel_ && (buttons.pressed[n] & WPAD_BUTTON_B)) back = true;
    if (closeRequested_ || hotkey || homes || back) {
        Close();
        return;
    }
    // The game refuses its pause during a shot or a Mega Strike: press it
    // again a moment later, while the match plays on unpaused.
    if (pausePresses_ > 0 && pausePresses_ < kPausePresses && now >= nextPause_) {
        if (PressPause()) nextPause_ = now + kPauseRetry;
        else pausePresses_ = 0;
    }
    FeedControllers(buttons);
}

void HostPanel::FeedControllers(const platform::NativeWpadPanelButtons& buttons)
{
    // The keyboard & mouse player's keys reach ImGui as keys and clicks; its
    // WPAD buttons (Return as A, a click as A) would press twice.
    ImGuiIO& io = ImGui::GetIO();
    // With the overlay, B is its key too (back a screen).
    constexpr ControllerKey kBack{WPAD_BUTTON_B, ImGuiKey_GamepadFaceRight};
    const std::size_t keys = std::size(kControllerKeys) + (overlay_ ? 1 : 0);
    for (std::size_t k = 0; k < keys; ++k) {
        const ControllerKey& entry = k < std::size(kControllerKeys) ? kControllerKeys[k] : kBack;
        bool down = false, pressed = false;
        for (int n = 0; n < 4; ++n) {
            if (n == keyboardChannel_) continue;
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

void HostPanel::Draw(float width, float height, const Fonts& fonts)
{
    if (overlay_) {
        overlay_(width, height, fonts.unit);
        if (justOpened_ && byController_) ImGui::SetNavCursorVisible(true);
        justOpened_ = false;
        typing_ = ImGui::GetIO().WantTextInput;
        return;
    }
    const float u = fonts.unit;
    ImDrawList* back = ImGui::GetBackgroundDrawList();
    // Over the blurred picture: a little darker, then the card's soft shadow.
    back->AddRectFilled({0, 0}, {width, height}, kDim);
    const ImVec2 size{std::round(std::min(720.0f * u, width * 0.92f)),
                      std::round(std::min(470.0f * u, height * 0.86f))};
    const ImVec2 min{std::round((width - size.x) * 0.5f), std::round((height - size.y) * 0.5f)};
    const ImVec2 max{min.x + size.x, min.y + size.y};
    const float round = 18.0f * u, pad = 28.0f * u;
    for (int n = 3; n >= 1; --n) {
        const float spread = float(n) * 7.0f * u;
        back->AddRectFilled({min.x - spread, min.y - spread + 10.0f * u}, {max.x + spread, max.y + spread + 10.0f * u},
                            IM_COL32(0, 0, 0, 40), round + spread);
    }

    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(size);
    if (justOpened_) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, round);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, 22.0f * u});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, u));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {20.0f * u, 10.0f * u});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 22.0f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.0f * u, 10.0f * u});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 5.0f * u);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, kCardBorder);
    ImGui::PushStyleColor(ImGuiCol_Text, kCardText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, kCardMuted);
    ImGui::PushStyleColor(ImGuiCol_Button, kButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kButtonActive);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##recharged-host-panel", nullptr, flags);
    if (justOpened_ && byController_) ImGui::SetNavCursorVisible(true);
    justOpened_ = false;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({min.x + round, min.y}, {max.x - round, min.y + std::max(2.0f, 3.0f * u)}, kAccent);

    // The title, then the content, then the way out.
    ImGui::PushFont(fonts.text);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("RECHARGED");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(fonts.bold);
    ImGui::TextUnformatted("Online");
    ImGui::PopFont();
    ImGui::Dummy({0, 4.0f * u});

    ImGui::PushFont(fonts.text);
    const float footer = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 6.0f * u;
    ImGui::BeginChild("##content", {0, -footer}, ImGuiChildFlags_NavFlattened);
    if (content_) content_(fonts);
    else {
        ImGui::PushStyleColor(ImGuiCol_Text, kCardMuted);
        ImGui::TextWrapped("The online panel goes here.");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::Dummy({0, 6.0f * u - ImGui::GetStyle().ItemSpacing.y});
    const char* hint = byController_ ? "B or HOME closes the panel." : "Esc closes the panel.";
    const float closeWidth = ImGui::CalcTextSize("Close").x + 2.0f * ImGui::GetStyle().FramePadding.x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", hint);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - closeWidth);
    if (ImGui::Button("Close")) closeRequested_ = true;
    ImGui::SetItemDefaultFocus();
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleColor(10);
    ImGui::PopStyleVar(9);
    typing_ = ImGui::GetIO().WantTextInput;
}

} // namespace mscharged::runtime
