#pragma once
#include "resources/frontend_fonts.h"
#include "NL/gl/glMatrix.h"
#include <array>
#include <memory>
#include <span>

class GLResourcePool;
class GLView;
namespace mscharged
{
// Real plain-colour font page bindings, retained in one original GL pool mark.
// The graphics session and pool outlive this owner. Aliases are the original
// lowercase hashes; page identities remain the authored case-sensitive hashes.
// This does not publish FontManager globals or implement its boot services.
class FrontendFontRegistry
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendFontRegistry(GLResourcePool&,
        std::span<const std::shared_ptr<const resources::FrontendFont>>, void (*drain)());
    ~FrontendFontRegistry();
    FrontendFontRegistry(const FrontendFontRegistry&) = delete;
    FrontendFontRegistry& operator=(const FrontendFontRegistry&) = delete;
    std::shared_ptr<const resources::FrontendFont> Find(std::uint32_t alias) const;
    std::uint16_t PageIndex(std::uint32_t alias, unsigned page) const;
    // Multiple ordered submissions are allowed during one collecting frame.
    // Layouts must retain the exact registered font. Quads preserve the original
    // page/line order, short UVs, colour and unclipped text raster state.
    unsigned Submit(GLView&, const resources::FontLayout&, const nlMatrix4& model,
        std::array<std::uint8_t, 4> colour = {255, 255, 255, 255}, int layer = 0);
    // After original send/cancel, drain GPU references before releasing owners.
    // A failed submission also needs frame cancellation and FinishFrame.
    void FinishFrame();
    void Release();
    bool Active() const;
};
}
