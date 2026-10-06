#pragma once

#include "platform/wpad_sdl.h"
#include "platform/desktop_wpad.h"
#include "platform/stm_device.h"

struct SDL_Window;

namespace mscharged::platform {

// Compose input with an existing SDK hardware owner. The exact STM input is
// borrowed from that owner's initialized device and is never retired here.
// These functions do not register/replace any SDK hardware endpoint.
void InitializeNativeHardwareInput(SDL_Window* window, WpadSDLSettings settings,
    StmInput borrowed_stm, DesktopWpadSettings desktop = {});
void ServiceNativeHardwareInput();
void ShutdownNativeHardwareInput();

// After native SDL/SDK host setup, before loading the original source module.
// The original source still initializes KPAD/pads and registers STM callbacks.
void InitializeNativeHardwareOwner(SDL_Window* window, WpadSDLSettings settings);

// Retire the SDK hook and borrowed hardware callbacks before source storage,
// SDK arenas or the window are released. This does not destroy game managers.
void ShutdownNativeHardwareOwner();

} // namespace mscharged::platform
