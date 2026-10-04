#pragma once
#include "resources/frontend_layout.h"

namespace mscharged
{
// Draw the checked mixed static frame in its original submission order. The
// retained frame/font/image pages must outlive GPU consumption. Timelines and scene
// handlers are not executed by this final view pass.
void DrawFrontendLayout(const resources::FrontendLayoutFrame& frame, unsigned width, unsigned height);
}
