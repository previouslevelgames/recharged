#pragma once
#include "resources/frontend_fonts.h"
#include "NL/gl/glDraw2.h"
#include "NL/gl/glState.h"
#include <array>
#include <vector>
#include <optional>
class GLView;
namespace mscharged::detail
{
// Internal checked packet preparation shared by both retained font owners.
struct FontPackets
{
    struct Run { std::size_t begin, end; std::uint32_t texture; };
    std::vector<glPoly2> quads;
    std::vector<Run> runs;
    nlMatrix4 model;
    std::optional<nlVector4> scissor;
};
FontPackets PrepareFontPackets(const resources::FontLayout&, const nlMatrix4&, std::array<std::uint8_t,4>,
    std::optional<std::array<std::uint16_t,4>> scissor = {});
void AttachFontPackets(GLView&, const FontPackets&, int layer);
void CheckFrontendCoordinate(float);
class FrontendPacketState
{
    glStateBundle bundle_;
    unsigned long raster_;
    unsigned long long texture_;
public:
    FrontendPacketState();
    ~FrontendPacketState();
    FrontendPacketState(const FrontendPacketState&) = delete;
    FrontendPacketState& operator=(const FrontendPacketState&) = delete;
};
}
