#pragma once
#include "resources/frontend_fonts.h"
#include <array>

namespace mscharged
{
// Draw authored font-page quads in a top-left pixel viewport. Changes GX state;
// call at the end of a render pass, or restore the caller's full pipeline after.
// layout (including texture storage) must remain alive until the frame ends.
// Requires completed graphics startup; owns no game globals or font registry.
void DrawFrontendText(const resources::FontLayout& layout, float x, float y,
                      unsigned viewport_width, unsigned viewport_height,
                      std::array<std::uint8_t, 4> colour = {255, 255, 255, 255});
}
