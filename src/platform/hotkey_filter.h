#pragma once

#include "bootstrap/key_actions.h"

struct SDL_Window;

namespace mscharged::platform
{
struct Hotkeys;

// The game window's hotkeys ([hotkeys]; issues #58, #77, #91): each a key,
// pressed with exactly the Ctrl, Shift and Alt it names. An SDL event filter,
// which runs before every event watch, takes a hotkey's press in the window
// out of the event stream, with its key's repeats and release, so the game
// never sees it: the keyboard player's Return stays A while Alt+Return
// switches to fullscreen.
void InitializeHotkeys(SDL_Window* window);
void ShutdownHotkeys();
// The [hotkeys] keys; the defaults until set.
void SetHotkeys(const Hotkeys& hotkeys);
// What `hotkey` does, called where its press is taken out (in SDL's event
// processing, on the window's thread); null leaves it doing nothing. The
// fullscreen hotkey is ServiceFullscreenHotkey's.
void SetHotkeyAction(HotkeyIndex hotkey, void (*action)());
#if defined(MSCHARGED_AUTOPILOT)
// Test builds' autopilot (src/platform/autopilot.cpp): what C, P and U held
// together in the window do, called where the chord's last key comes down;
// null (the default) leaves the three keys as they are. With an action, a
// lone press of one of them reaches the game a moment late and a hotkey on
// one acts on its release, so the chord never presses or does what they do.
void SetAutopilotChord(void (*action)());
#endif

enum class FullscreenSwitch
{
    None,       // no press since the last call
    Fullscreen, // the window went fullscreen
    Windowed,   // the window left fullscreen
    Failed,     // SDL refused; SDL_GetError() says why
};
// On the window's thread, after its events were pumped: carries out a press
// of the fullscreen hotkey (fullscreen or a window, for this run).
FullscreenSwitch ServiceFullscreenHotkey();
}
