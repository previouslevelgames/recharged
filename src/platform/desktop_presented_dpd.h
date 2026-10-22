#pragma once

#include "platform/desktop_wpad.h"

namespace mscharged::platform {
// GX host on its SDL owner only: observes a successful Present and actual SDL sizes.
// This callback cannot service VI, render, acquire, resize or wait for output.
bool QueryPresentedDesktopDpd(void*, SDL_Window*, DesktopDpdProjection*);
// Wider view (display.wider_view, issue #91): in matches the picture fills a
// window of another shape than 16:9 while the HUD and the front end keep their
// 16:9 layout in its middle, so the mouse points within that middle part.
// `shape` is the window's shape divided by 16:9 (wider above 1, taller below);
// 1 maps the whole picture.
void SetPresentedPointerShape(float shape);
} // namespace mscharged::platform
