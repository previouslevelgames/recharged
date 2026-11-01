#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

struct ImFont;
struct SDL_Window;

namespace mscharged::platform {
struct NativeWpadPanelButtons;
}

namespace mscharged::runtime {

// The host panel over the game (the online panel): F1 ([hotkeys] panel) or
// HOME on any player's controller (the Wii Remote's HOME, a gamepad's Guide,
// the keyboard's Home: whatever plays HOME) opens and closes it; Escape, B and
// its Close button close it. While open:
// - the game picture is blurred and darkened behind it (in the window only:
//   screenshots stay sharp, the game's frames never contain the panel);
// - the panel takes the mouse (the system cursor shows), the keyboard and the
//   players' buttons; the game sees every player stand still (no buttons, a
//   centred stick, the pointer and motion where they were) and never HOME;
// - online the game goes on; in an offline match opening it presses the
//   game's own pause (SetPauseRule), again a moment later while the game
//   refuses it (a Mega Strike, a shot), until it pauses or plays on.
// Use it on the presentation owner thread, between game frames.
class HostPanel {
public:
    using Clock = std::chrono::steady_clock;

    // The overlay's fonts, in surface pixels: text 17 and bold 26 units of
    // 1/720 of the picture's height (`unit` pixels).
    struct Fonts {
        ImFont* text = nullptr;
        ImFont* bold = nullptr;
        float unit = 1.0f;
    };
    // What the panel shows under its title, each frame while open: ImGui calls
    // inside the panel's window (a child region filling the rest of it), with
    // `fonts.text` pushed. Mouse, keyboard and controllers work as in any ImGui
    // window: controllers navigate with the D-pad and A (ImGuiKey_GamepadDpad*
    // and GamepadFaceDown), 1 and 2 are GamepadFaceLeft and GamepadFaceUp, - and
    // + GamepadL1 and GamepadR1, the Nunchuk's C and Z GamepadL2 and GamepadR2,
    // its stick GamepadLStick*. B always closes the panel.
    using Content = std::function<void(const Fonts& fonts)>;
    // Or the game's whole overlay (src/overlay): drawn over the picture
    // (`width` x `height` surface pixels, `unit` pixels per 1/720 of its
    // height) instead of the panel's own card, each frame while open. B then
    // goes to it as ImGuiKey_GamepadFaceRight (back a screen, or closing it on
    // a first screen: RequestClose) instead of closing the panel.
    using Overlay = std::function<void(float width, float height, float unit)>;

    // window: the game window, for the mouse position when the panel opens.
    explicit HostPanel(SDL_Window* window);
    ~HostPanel();
    HostPanel(const HostPanel&) = delete;
    HostPanel& operator=(const HostPanel&) = delete;

    // None shows a placeholder line.
    void SetContent(Content content);
    void SetOverlay(Overlay overlay);
    // The overlay closes the panel (its Close, Escape, B on a first screen).
    void RequestClose() { closeRequested_ = true; }
    // Whether a controller's HOME opened it (rather than F1, the keyboard).
    bool OpenedByController() const { return byController_; }
    // Asked when the panel opens, then about once a second while open until
    // it says false: true presses the game's pause (its 1 button) for an
    // offline match in play that is not paused yet; never online.
    void SetPauseRule(std::function<bool()> rule);

    bool IsOpen() const { return open_; }
    void Open();
    void Close();

    // Each host frame, before HostOverlay::Update: the hotkey, HOME, Escape and
    // B, and the players' buttons for ImGui while open.
    void Service(Clock::time_point now);
    // HostOverlay calls this inside its ImGui frame while the panel is open.
    void Draw(float width, float height, const Fonts& fonts);

private:
    friend struct HostPanelLayer;
    // Opened by F1 (-1) or HOME on that WPAD channel.
    void OpenFrom(int channel);
    void FeedControllers(const platform::NativeWpadPanelButtons& buttons);
    // Presses the game's pause if the rule asks for it; false when it does not.
    bool PressPause();

    SDL_Window* window_;
    Content content_;
    Overlay overlay_;
    std::function<bool()> pauseRule_;
    std::uint64_t layer_ = 0;
    bool open_ = false;
    // Its first frame (focus) and whether a controller's HOME opened it (the
    // navigation cursor shows at once).
    bool justOpened_ = false, byController_ = false;
    // Set by the input layer's Escape, the Close button or the dev switch.
    bool closeRequested_ = false;
    // Whether ImGui's text field had the keyboard last frame (Escape and the
    // keyboard's own HOME then belong to the text).
    bool typing_ = false;
    int keyboardChannel_ = -1;
    // Who opened it (a WPAD channel, -1 for F1), and the pause presses so far
    // and when the next one is due.
    int opener_ = -1;
    int pausePresses_ = 0;
    Clock::time_point nextPause_{};
    // The controller keys ImGui has down (bit n: kControllerKeys[n]).
    std::uint32_t controllerDown_ = 0;
    // A dev switch (RECHARGED_CAPTURE_PANEL=SECONDS[,OPEN]): opens once that
    // long after start (and closes after OPEN seconds), for screenshots
    // without key presses.
    Clock::time_point captureAt_{}, captureClose_{};
    std::chrono::milliseconds captureOpen_{0};
    bool captureDone_ = true;
};

} // namespace mscharged::runtime
