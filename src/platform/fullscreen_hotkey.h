#pragma once

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

struct SDL_Window;

namespace mscharged::platform
{
// Alt+Enter (Option+Return on a Mac) switches the game window between
// fullscreen and a window, for this run (issue #77). An Enter pressed with Alt
// never reaches the game's input, where Return is A.
#if defined(__APPLE__)
inline constexpr const char* kFullscreenShortcut = "Option+Return";
#else
inline constexpr const char* kFullscreenShortcut = "Alt+Enter";
#endif

// Whether pressing `key` with `modifiers` held is the shortcut: Enter or the
// keypad's Enter with Alt (Option), without Ctrl or the Windows/Command key.
bool IsFullscreenShortcut(SDL_Scancode key, SDL_Keymod modifiers);

// Takes the shortcut's presses in `window` out of the event stream (an SDL
// event filter, which runs before every event watch).
void InitializeFullscreenHotkey(SDL_Window* window);
void ShutdownFullscreenHotkey();

enum class FullscreenSwitch
{
    None,       // no press since the last call
    Fullscreen, // the window went fullscreen
    Windowed,   // the window left fullscreen
    Failed,     // SDL refused; SDL_GetError() says why
};
// On the window's thread, after its events were pumped: carries out a press.
FullscreenSwitch ServiceFullscreenHotkey();
}
