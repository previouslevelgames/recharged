#pragma once
#include "resources/frontend_fonts.h"
#include <array>

namespace mscharged
{
using TextDrawTransform = std::array<std::array<float, 4>, 3>;
// Draw authored font-page quads in a top-left pixel viewport. Changes GX state;
// call at the end of a render pass, or restore the caller's full pipeline after.
// layout (including texture storage) must remain alive until the frame ends.
// Requires completed graphics startup; owns no game globals or font registry.
void DrawFrontendText(const resources::FontLayout& layout, float x, float y,
                      unsigned viewport_width, unsigned viewport_height,
                      std::array<std::uint8_t, 4> colour = {255, 255, 255, 255});
// Row-major GX transform from local glyph coordinates to the logical viewport.
// The caller supplies authored placement; this function does not choose slides,
// change line layout, or approximate unsupported text effects.
void DrawFrontendText(const resources::FontLayout& layout, const TextDrawTransform& transform,
                      unsigned viewport_width, unsigned viewport_height,
                      std::array<std::uint8_t, 4> colour = {255, 255, 255, 255});
}
