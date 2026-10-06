#pragma once

#include "platform/wpad_sdl.h"

struct SDL_Window;

namespace mscharged::platform {

// After native SDL/SDK host setup, before loading the original source module.
// The original source still initializes KPAD/pads and registers STM callbacks.
void InitializeNativeHardwareOwner(SDL_Window* window, WpadSDLSettings settings);

// Retire the SDK hook and borrowed hardware callbacks before source storage,
// SDK arenas or the window are released. This does not destroy game managers.
void ShutdownNativeHardwareOwner();

} // namespace mscharged::platform
