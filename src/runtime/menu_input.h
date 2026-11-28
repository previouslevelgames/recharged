#pragma once

#include "platform/wiimote_ir.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

struct SDL_Window;

namespace mscharged::platform {
struct NativeWpadPanelButtons;
}

namespace mscharged::runtime {

// The input of Recharged's menus (HostMenus): the keys that open them (F1 or
// HOME on any player's controller: the Wii Remote's HOME, a gamepad's Guide,
// the keyboard's Home; F2 for the gameplay options; the game's pause in an
// online match), and while a page shows the keyboard, the mouse (its system
// cursor hidden: the menus draw the game's hand), the players' buttons,
// which reach ImGui instead of the game, and where their Wii Remotes point
// (Hands). The game then sees every player stand still (no buttons, a
// centred stick, the pointer and motion where they were) and never HOME. In
// an offline match opening the menus may press the game's own pause
// (SetPauseRule), again a moment later while the game refuses it (a Mega
// Strike, a shot), until it pauses or plays on.
// Use it on the presentation owner thread, between game frames.
class MenuInput {
public:
    using Clock = std::chrono::steady_clock;

    // window: the game window, for the mouse position when the pages open
    // and the picture the players point at. kpadSensorHeight: the game's
    // KPAD has a pointer centre (all but the European build's, see
    // platform::KpadPointerCentre).
    MenuInput(SDL_Window* window, bool kpadSensorHeight);
    ~MenuInput();
    MenuInput(const MenuInput&) = delete;
    MenuInput& operator=(const MenuInput&) = delete;

    // Asked when the pages open with `pause`, then about once a second until
    // it says false: true presses the game's pause (its 1 button) for an
    // offline match in play that is not paused yet; never online.
    void SetPauseRule(std::function<bool()> rule);

    // The pages take the input: the keyboard, the mouse and the players'
    // buttons reach ImGui; B (GamepadFaceRight), Escape and Backspace are the
    // menus' back, the 1 button (GamepadFaceLeft, the keyboard player's too)
    // the game's pause. `channel`: the controller that opened them (-1 for
    // the keys). `pause`: press the game's pause in an offline match.
    void OpenPages(int channel, bool pause);
    void ClosePages();
    bool PagesOpen() const { return pages_; }
    // The keys the menus may take: F1 or HOME, F2 (Recharged's gameplay
    // options), and the game's pause pressed while no page shows (only told:
    // it reaches the game as well).
    enum class MenuKey { Recharged, Options, Pause };
    // With the channel of the controller that pressed it (-1 for the keys).
    void SetMenuKeys(std::function<void(MenuKey key, int channel)> keys);
    // The keyboard & mouse player's channel while the pages are open (-1 for
    // none).
    int KeyboardChannel() const { return keyboardChannel_; }
    // Whether the mouse points at the pages (and clicks and scrolls them): as
    // in the game, only for the keyboard & mouse player or, with
    // controls.mouse_pointer, beside a controller playing alone.
    bool MousePoints() const { return mousePoints_; }
    // A player's hand on the pages: where their Wii Remote (or a gamepad's
    // pointer stick or gyro) points, in the overlay's surface pixels, as the
    // game would place its own. It shows once it moved since the pages opened
    // or the player last steered with the D-pad or the stick (the lit item
    // moves, which A then presses), and while it shows the player's A presses
    // where it is. The keyboard player points with the mouse.
    struct Hand {
        bool shown = false;
        float x = 0.0f, y = 0.0f;
        bool moved = false; // this frame
        bool click = false; // A pressed this frame
        bool held = false;  // and still down
    };
    const std::array<Hand, 4>& Hands() const { return hands_; }

    // Each host frame, before HostOverlay::Update: the keys, and the players'
    // buttons for ImGui while the pages are open.
    void Service(Clock::time_point now);

private:
    friend struct MenuInputLayer;
    // The hands from the players' cameras, before their buttons go to ImGui.
    void PointHands(const platform::NativeWpadPanelButtons& buttons);
    void FeedControllers(const platform::NativeWpadPanelButtons& buttons);
    // Presses the game's pause if the rule asks for it; false when it does not.
    bool PressPause();
    // The game refuses its pause during a shot or a Mega Strike: pressed again
    // a moment later, while the match plays on unpaused.
    void RetryPause(Clock::time_point now);
    // Takes the input or gives it back.
    void Capture(bool on);

    SDL_Window* window_;
    std::function<bool()> pauseRule_;
    std::function<void(MenuKey, int)> menuKeys_;
    std::uint64_t layer_ = 0;
    bool pages_ = false;
    int keyboardChannel_ = -1;
    bool mousePoints_ = false;
    // Who opened the pages (a WPAD channel, -1 for the keys), and the pause
    // presses so far and when the next one is due.
    int opener_ = -1;
    int pausePresses_ = 0;
    Clock::time_point nextPause_{};
    // The controller keys ImGui has down (bit n: kControllerKeys[n]).
    std::uint32_t controllerDown_ = 0;
    // Each player's hand: its camera's pointer, where it rests (it shows once
    // moved away from there), and whether its A is the hand's (pressed where
    // the hand is, until released) rather than the lit item's.
    struct Pointing {
        platform::CameraPointer camera;
        std::optional<std::array<float, 2>> rest;
        bool live = false;
        bool a = false;
    };
    std::array<Pointing, 4> pointing_{};
    std::array<Hand, 4> hands_{};
    bool kpadSensorHeight_ = true;
    // A dev switch (RECHARGED_CAPTURE_PANEL=SECONDS[,OPEN]): presses F1 once
    // that long after start (and again after OPEN seconds), for screenshots
    // without key presses.
    Clock::time_point captureAt_{}, captureClose_{};
    std::chrono::milliseconds captureOpen_{0};
    bool captureDone_ = true;
};

} // namespace mscharged::runtime
