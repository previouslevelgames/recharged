#pragma once
#include "frontend_layout_fixture.h"
#include "resources/frontend_images.h"

namespace frontend_image_fixture
{
using Colour=std::array<std::uint8_t,4>;
// Independent tile encoder: quadrants in decoded image space, retained in Wii
// RGBA8 plane order or CI8 indices with an RGB5A3 palette.
inline std::shared_ptr<mscharged::resources::Texture> Texture(std::uint32_t id,unsigned format=3,
    std::array<Colour,4> colours={{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255}}})
{
    using namespace mscharged::resources;
    auto texture=std::make_shared<mscharged::resources::Texture>();
    texture->id=id;texture->width=texture->height=16;texture->game_format=format;texture->levels=1;
    texture->bits={8,8,8,8};
    if(format==3)
    {
        texture->gx_format=6;texture->pixels.resize(16*16*4);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        {
            const auto colour=colours[(y/8)*2+x/8];const auto tile=((y/4)*4+x/4)*64,index=((y%4)*4+x%4)*2;
            texture->pixels[tile+index]=colour[3];texture->pixels[tile+index+1]=colour[0];
            texture->pixels[tile+32+index]=colour[1];texture->pixels[tile+32+index+1]=colour[2];
        }
    }
    else if(format==1)
    {
        texture->gx_format=5;texture->pixels.resize(512);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        {
            const auto c=colours[(y/8)*2+x/8];const unsigned value=c[3]==255?0x8000|((c[0]>>3)<<10)|((c[1]>>3)<<5)|(c[2]>>3)
                :((c[3]>>5)<<12)|((c[0]>>4)<<8)|((c[1]>>4)<<4)|(c[2]>>4);
            const auto offset=((y/4)*4+x/4)*32+((y%4)*4+x%4)*2;
            texture->pixels[offset]=value>>8;texture->pixels[offset+1]=value;
        }
    }
    else if(format==2)
    {
        texture->gx_format=14;texture->pixels.resize(128);
        for(unsigned quadrant=0;quadrant<4;++quadrant)for(unsigned block=0;block<4;++block)
        {
            const auto c=colours[quadrant];const unsigned value=((c[0]>>3)<<11)|((c[1]>>2)<<5)|(c[2]>>3);
            const auto offset=quadrant*32+block*8;
            texture->pixels[offset]=texture->pixels[offset+2]=value>>8;
            texture->pixels[offset+1]=texture->pixels[offset+3]=value;
            for(unsigned row=0;row<4;++row)texture->pixels[offset+4+row]=c[3]==0?255:0;
        }
    }
    else if(format==8)
    {
        texture->gx_format=9;texture->palette_entries=4;texture->palette.resize(8);texture->pixels.resize(256);
        for(unsigned i=0;i<4;++i)
        {
            const auto c=colours[i];const unsigned value=c[3]==255?0x8000|((c[0]>>3)<<10)|((c[1]>>3)<<5)|(c[2]>>3)
                :((c[3]>>5)<<12)|((c[0]>>4)<<8)|((c[1]>>4)<<4)|(c[2]>>4);
            texture->palette[i*2]=value>>8;texture->palette[i*2+1]=value;
        }
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
            texture->pixels[((y/4)*2+x/8)*32+(y%4)*8+x%8]=(y/8)*2+x/8;
    }
    else throw std::runtime_error("Unsupported image fixture format");
    return texture;
}
inline void AddImage(mscharged::resources::FrontendScene& scene,std::uint32_t hash,std::uint32_t id=600)
{
    using namespace mscharged::resources;
    scene.resources.push_back({id+1,0,hash,0,false});
    FrontendLibraryObject image{};image.offset=id+2;image.type=1;image.attributes=frontend_layout_fixture::Attributes();
    image.attributes.uv={0,0,1,1};scene.library.push_back(image);
    FrontendInstance instance{};instance.offset=id;instance.type=2;instance.library=id+2;instance.resource=id+1;
    instance.name="Image";instance.duration=100;instance.visible=true;instance.attributes=frontend_layout_fixture::Attributes();
    instance.overload_flags=1|16;instance.image_blend=1;scene.instances.push_back(instance);
    scene.instances[0].children.push_back(id);
}
}
