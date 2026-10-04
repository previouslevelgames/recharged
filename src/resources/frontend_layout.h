#pragma once
#include "resources/frontend_fonts.h"
#include "resources/frontend_scene.h"
#include "resources/texture_bundle.h"
#include <map>
#include <variant>

namespace mscharged::resources
{
struct FrontendImageCatalog;
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
    struct ImageVertex { float x, y, u, v; };
    struct Image
    {
        std::uint32_t instance = 0;
        std::uint16_t priority = 0;
        std::string name;
        std::shared_ptr<const Texture> texture;
        // Original local Y-up quad and half-texel-adjusted UVs. The transform
        // maps these coordinates to the same logical viewport as text.
        std::array<ImageVertex,4> vertices{};
        std::array<float,16> transform{};
        std::array<std::uint8_t,4> colour{};
        std::uint32_t blend = 0;
    };
    using Entry = std::variant<FrontendLayoutText,Image>;
    std::vector<Entry> entries; // One final Anark reverse order across both types.
    std::map<std::string,unsigned> unavailable;
    unsigned hidden = 0;
    std::size_t TextCount() const;
    std::size_t ImageCount() const;
};
using FrontendLayoutImage = FrontendLayoutFrame::Image;
using FrontendImageVertex = FrontendLayoutFrame::ImageVertex;
using FrontendLayoutEntry = FrontendLayoutFrame::Entry;
// Defensive validation for retained public Texture values before layout/drawing.
void ValidateFrontendImageTexture(const Texture&);
// Evaluate a bounded stored static frame, without executing animation, handlers,
// slide transitions or menu logic. Omission selects the saved active presentation
// slide; an explicit ID must belong to its presentation ring. Nested components
// use their saved active slide. Animated/nonplanar branches remain unavailable.
// Returned text and font-page handles outlive the input scene/localization.
FrontendLayoutFrame BuildFrontendLayout(const FrontendScene&, const Localization&,
    std::span<const std::shared_ptr<const FrontendFont>> fonts,
    FrontendReference presentation_slide = {});
FrontendLayoutFrame BuildFrontendLayout(const FrontendScene&, const Localization&,
    std::span<const std::shared_ptr<const FrontendFont>> fonts,
    FrontendReference presentation_slide, const FrontendImageCatalog& images);
}
