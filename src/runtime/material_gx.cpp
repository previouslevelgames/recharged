// Typed native forwarding for the GX state calls selected material TUs use.
// Aurora owns the actual state cache and generates/caches shaders from TEV state.
#include "NL/glx/glxGX.h"
#include "NL/nlColour.h"
#include <dolphin/gx.h>
#include <utility>
#include <stdexcept>

namespace
{
unsigned channels, stages, generators;
nlColour ambient_colours[2]{}, material_colours[2]{};
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
