#pragma once

struct SDL_Window;

namespace mscharged::platform
{
// The hotkeys' actions (hotkey_filter.h finds their presses): the screenshot
// hotkey saves the next presented frame as a PNG in <working directory>/
// screenshots, the HUD, stretch and 3D resolution hotkeys are below. A host
// convenience only: the keys never reach the original game input.
void InitializeScreenshotHotkey(SDL_Window* window);
void ShutdownScreenshotHotkey();
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
