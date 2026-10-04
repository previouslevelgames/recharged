#pragma once
#include "resources/frontend_layout.h"

namespace mscharged
{
// Draw one qualified ordinary static frontend image. Changes GX state; call in
// the final frontend view. Retain the image texture until GPU work has drained.
// Movie/grab callbacks and dynamic texture sources remain unavailable.
void DrawFrontendImage(const resources::FrontendLayoutImage&, unsigned viewport_width, unsigned viewport_height);
}
