// Typed native forwarding for the GX state calls selected material TUs use.
// Aurora owns the actual state cache and generates/caches shaders from TEV state.
#include "NL/glx/glxGX.h"
#include "NL/nlColour.h"
#include <dolphin/gx.h>
#include <utility>
#include <stdexcept>
#include <optional>

namespace
{
unsigned channels, stages, generators;
nlColour ambient_colours[2]{}, material_colours[2]{};
bool colour_write = true, alpha_write = true;
struct DepthWrites { bool test; int function; bool write; } depth{true, GX_LEQUAL, true};
std::optional<DepthWrites> saved_depth;
struct Blend { bool enabled; int source, destination; bool subtract; } blend{false, GX_BL_ONE, GX_BL_ZERO, false};
std::optional<Blend> saved_blend;
}
bool gxSetColourUpdate(bool enabled)
{
    GXSetColorUpdate(enabled);
    return std::exchange(colour_write, enabled);
}
bool gxSetAlphaUpdate(bool enabled)
{
    GXSetAlphaUpdate(enabled);
    return std::exchange(alpha_write, enabled);
}
void gxSetZMode(bool test, int function, bool write)
{
    GXSetZMode(test, GXCompare(function), write);
    depth = {test, function, write};
}
void gxSetAlphaCompare(int function, unsigned char reference)
{
    GXSetAlphaCompare(GXCompare(function), reference, GX_AOP_AND, GX_ALWAYS, 0);
}
void gxSaveZMode()
{
    if (saved_depth) throw std::logic_error("Nested GX depth-state save");
    saved_depth = depth;
}
void gxRestoreZMode()
{
    if (!saved_depth) throw std::logic_error("GX depth-state restore without save");
    const auto value = *saved_depth;
    saved_depth.reset();
    gxSetZMode(value.test, value.function, value.write);
}
void gxSetBlendMode(bool enabled, int source, int destination, bool subtract)
{
    if (source < 0 || source > GX_BL_INVDSTALPHA || destination < 0 || destination > GX_BL_INVDSTALPHA)
        throw std::invalid_argument("Invalid native GX blend factors");
    GXSetBlendMode(!enabled ? GX_BM_NONE : subtract ? GX_BM_SUBTRACT : GX_BM_BLEND,
        GXBlendFactor(source), GXBlendFactor(destination), GX_LO_CLEAR);
    blend = {enabled, source, destination, subtract};
}
void gxSaveBlendMode()
{
    if (saved_blend) throw std::logic_error("Nested GX blend-state save");
    saved_blend = blend;
}
void gxRestoreBlendMode()
{
    if (!saved_blend) throw std::logic_error("GX blend-state restore without save");
    const auto value = *saved_blend; saved_blend.reset();
    gxSetBlendMode(value.enabled, value.source, value.destination, value.subtract);
}
namespace mscharged
{
void RestoreMaterialSavedStates()
{
    if (saved_blend) gxRestoreBlendMode();
    if (saved_depth) gxRestoreZMode();
}
}
unsigned gxSetNumChans(unsigned n)
{
    GXSetNumChans(n);
    return std::exchange(channels, n);
}
unsigned gxSetNumTevStages(unsigned n)
{
    GXSetNumTevStages(n);
    return std::exchange(stages, n);
}
unsigned gxSetNumTexGens(unsigned n)
{
    GXSetNumTexGens(n);
    return std::exchange(generators, n);
}
unsigned gxGetNumTevStages()
{
    return stages;
}
unsigned gxGetNumTexGens()
{
    return generators;
}
void gxSetTevOrder(int stage, int coord, int map, int colour)
{
    GXSetTevOrder(GXTevStageID(stage), GXTexCoordID(coord), GXTexMapID(map), GXChannelID(colour));
}
void gxSetTevKColourSel(int stage, int selection)
{
    GXSetTevKColorSel(GXTevStageID(stage), GXTevKColorSel(selection));
}
void gxSetTevColourIn(int stage, int a, int b, int c, int d)
{
    GXSetTevColorIn(GXTevStageID(stage), GXTevColorArg(a), GXTevColorArg(b), GXTevColorArg(c), GXTevColorArg(d));
}
void gxSetTevAlphaIn(int stage, int a, int b, int c, int d)
{
    GXSetTevAlphaIn(GXTevStageID(stage), GXTevAlphaArg(a), GXTevAlphaArg(b), GXTevAlphaArg(c), GXTevAlphaArg(d));
}
void gxSetTevColourOp(int stage, int op, int bias, int scale, bool clamp, int output)
{
    GXSetTevColorOp(GXTevStageID(stage), GXTevOp(op), GXTevBias(bias), GXTevScale(scale), clamp, GXTevRegID(output));
}
void gxSetTevAlphaOp(int stage, int op, int bias, int scale, bool clamp, int output)
{
    GXSetTevAlphaOp(GXTevStageID(stage), GXTevOp(op), GXTevBias(bias), GXTevScale(scale), clamp, GXTevRegID(output));
}
void gxSetTexCoordGen(int dst, int func, int src, unsigned mtx, bool normalize, unsigned post)
{
    GXSetTexCoordGen2(GXTexCoordID(dst), GXTexGenType(func), GXTexGenSrc(src), mtx, normalize, post);
}
void gxSetTexCoordGen(int dst, int func, int src, unsigned mtx)
{
    gxSetTexCoordGen(dst, func, src, mtx, false, GX_PTIDENTITY);
}
nlColour gxSetChanAmbColour(int channel, const nlColour& colour)
{
    if (channel < 0 || channel > 1) throw std::out_of_range("Invalid ambient colour channel");
    GXSetChanAmbColor(GXChannelID(GX_COLOR0A0 + channel),
        {colour.c[0], colour.c[1], colour.c[2], colour.c[3]});
    return std::exchange(ambient_colours[channel], colour);
}
nlColour gxSetChanMatColour(int channel, const nlColour& colour)
{
    if (channel < 0 || channel > 1) throw std::out_of_range("Invalid material colour channel");
    GXSetChanMatColor(GXChannelID(GX_COLOR0A0 + channel),
        {colour.c[0], colour.c[1], colour.c[2], colour.c[3]});
    return std::exchange(material_colours[channel], colour);
}
