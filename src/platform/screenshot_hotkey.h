#pragma once

struct SDL_Window;

namespace mscharged::platform
{
// P saves the next presented frame as a PNG in <working directory>/screenshots.
// A host convenience only: the key never reaches the original game input.
void InitializeScreenshotHotkey(SDL_Window* window);
void ShutdownScreenshotHotkey();
}
