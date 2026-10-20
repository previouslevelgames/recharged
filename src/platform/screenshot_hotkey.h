#pragma once

struct SDL_Window;

namespace mscharged::platform
{
struct Hotkeys;

// The screenshot key (P unless [hotkeys] screenshot says otherwise) saves the
// next presented frame as a PNG in <working directory>/screenshots. A host
// convenience only: the key never reaches the original game input.
void InitializeScreenshotHotkey(SDL_Window* window);
void ShutdownScreenshotHotkey();
// The [hotkeys] keys; the defaults until set.
void SetHotkeys(const Hotkeys& hotkeys);
// Presses of the display hotkeys since the last call (issue #91), for the
// window's thread to carry out between frames: stretch toggles, and the 3D
// resolution steps (up minus down).
struct DisplayHotkeyPresses
{
    int stretch_toggles = 0;
    int resolution_steps = 0;
};
DisplayHotkeyPresses TakeDisplayHotkeyPresses();
// Diagnostic rendering-layer keys (F9 listed layers, F10 cycle one layer),
// provided by the game module; null leaves the keys unused.
void SetDebugLayerKeys(void (*toggle)(), void (*cycle)());
// The HUD key shows or hides the match HUD (No HUD, issue #58) through the
// game module; null leaves the key unused.
void SetHudKey(void (*toggle)());
}
