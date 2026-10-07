#pragma once

#include "platform/desktop_wpad.h"

namespace mscharged::platform {
// GX host on its SDL owner only: observes a successful Present and actual SDL sizes.
// This callback cannot service VI, render, acquire, resize or wait for output.
bool QueryPresentedDesktopDpd(void*, SDL_Window*, DesktopDpdProjection*);
} // namespace mscharged::platform
