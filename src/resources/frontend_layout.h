#pragma once
#include "resources/frontend_fonts.h"
#include "resources/frontend_scene.h"
#include <map>

namespace mscharged::resources
{
struct FrontendLayoutText
{
    std::uint32_t instance = 0;
    std::uint16_t priority = 0; // Stored metadata; original FERender does not sort by this field.
    std::string name;
    std::u16string text;
    FontLayout layout;
    // Maps local y-down glyph pixels to logical 640x480 screen pixels:
    // x' = m[0]*x + m[4]*y + m[12], y' = m[1]*x + m[5]*y + m[13].
    // Column storage, not GX's row-major projection storage. Planar affine only.
    std::array<float,16> transform{};
    std::array<std::uint8_t,4> colour{};
};
struct FrontendLayoutFrame
{
    std::vector<FrontendLayoutText> text; // Final original Anark reverse draw order.
    std::map<std::string,unsigned> unavailable;
    unsigned hidden = 0;
};
// Evaluate a bounded stored static frame, without executing animation, handlers,
// slide transitions or menu logic. Omission selects the saved active presentation
// slide; an explicit ID must belong to its presentation ring. Nested components
// use their saved active slide. Animated/nonplanar branches remain unavailable.
// Returned text and font-page handles outlive the input scene/localization.
FrontendLayoutFrame BuildFrontendLayout(const FrontendScene&, const Localization&,
    std::span<const std::shared_ptr<const FrontendFont>> fonts,
    FrontendReference presentation_slide = {});
}
