#pragma once
#include "runtime/world_render.h"
#include "NL/gl/glState.h"

namespace world_fixture
{
using namespace mscharged;
inline std::array<nlVector4, 6> Planes()
{
    return {{{1,0,0,1}, {-1,0,0,1}, {0,1,0,1}, {0,-1,0,1}, {0,0,1,1}, {0,0,-1,0}}};
}
inline resources::StaticWorldObject Object(unsigned id, unsigned model, float x = 0, float y = 0, float z = -.5f)
{
    resources::StaticWorldObject o;
    o.id=id; o.type=0x101; o.model=model; o.creation_flags=3;
    o.transform={1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1}; o.radius=.3f;
    o.bounds_min={x-.2f,y-.2f,z-.01f}; o.bounds_max={x+.2f,y+.2f,z+.01f};
    return o;
}
inline resources::Texture Texture(unsigned id, std::array<unsigned char,4> colour)
{
    resources::Texture t; t.id=id; t.width=t.height=4; t.levels=1; t.game_format=3; t.gx_format=6;
    t.bits={8,8,8,static_cast<unsigned char>(colour[3]==255?0:8)}; t.pixels.resize(64);
    for(unsigned i=0;i<16;++i)
    { t.pixels[i*2]=colour[3]; t.pixels[i*2+1]=colour[0]; t.pixels[32+i*2]=colour[1]; t.pixels[33+i*2]=colour[2]; }
    return t;
}
inline resources::StaticModel Quad(unsigned id, unsigned texture, unsigned blend=0)
{
    resources::Packet p; p.primitive=0; p.material.program=0x21db4385; p.material.textures[0]={texture,3};
    p.raster=0xc0007; glSetRasterState(p.raster,GLS_AlphaBlend,blend);
    glSetRasterState(p.raster,GLS_DepthWrite,blend?0:1);
    p.vertices={{{-.2f,-.2f,0},{0,0}},{{.2f,-.2f,0},{1,0}},{{.2f,.2f,0},{1,1}},{{-.2f,.2f,0},{0,1}}};
    p.indices={0,1,2,0,2,3}; return {id,{p}};
}
}
