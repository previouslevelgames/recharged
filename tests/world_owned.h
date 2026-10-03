#pragma once
// Optional qualification with explicitly selected, locally owned decompressed
// world files. CTest uses generated geometry and never requires game data.
#include "runtime/world_render.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/gpu_readback.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMatrix.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <string>

namespace world_owned
{
using namespace mscharged;
using namespace mscharged::resources;
inline std::vector<unsigned char> Read(const char* path)
{
    const auto size=std::filesystem::file_size(path);
    Require(size>0&&size<=MaximumAssetBytes,"Owned file exceeds bounded resource size");
    std::vector<unsigned char> bytes(size);std::ifstream file(path,std::ios::binary);
    Require(bool(file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())),"Cannot read owned resource");return bytes;
}
inline StaticWorldFrustum Frustum(const ViewMatrices& matrices,float width,float height,float near,float far)
{
    const auto& v=matrices.view;
    return StaticWorldFrustum({{{v.m11,v.m21,v.m31,v.m41+width/2}, {-v.m11,-v.m21,-v.m31,-v.m41+width/2},
        {v.m12,v.m22,v.m32,v.m42+height/2}, {-v.m12,-v.m22,-v.m32,-v.m42+height/2},
        {-v.m13,-v.m23,-v.m33,-v.m43-near}, {v.m13,v.m23,v.m33,v.m43+far}}});
}
// begin() acquires the actual Aurora frame and advances frame scratch memory;
// drain() is the actual GX synchronization hook. No original scheduler is called.
inline void Run(char** paths,ViewMatrices& matrices,GLView& opaque,GLView& alpha,void(*begin)(),void(*drain)())
{
    auto resident=Read(paths[0]),temporary=Read(paths[1]),global=Read(paths[2]);
    std::ifstream selection(paths[3]);Require(bool(selection),"Cannot read explicit object ID list");
    std::vector<std::uint32_t> ids;std::string word;
    while(selection>>word)
    {
        Require(ids.size()<MaximumWorldObjects&&word.size()<=8,"Invalid explicit world object selection");
        std::size_t end=0;auto id=std::stoul(word,&end,16);
        Require(end==word.size()&&id<=UINT32_MAX,"Invalid hex object ID");ids.push_back(id);
    }
    auto objects=ReadStaticWorldObjects(resident,ids);
    std::vector<StaticModel> models;std::set<std::uint32_t> model_ids,required,lookups;Bytes rlt;
    for(const auto& o:objects)
    {
        if(!model_ids.insert(o.model).second)continue;
        auto selected=ReadStaticWorldModel(temporary,o.model,ModelCoordinates::Local);rlt=selected.textures;
        for(const auto& p:selected.model.packets)for(const auto& b:p.material.textures)if(b.texture)required.insert(b.texture);
        for(auto id:MaterialLookupTextures(selected.model))lookups.insert(id);
        models.push_back(std::move(selected.model));
    }
    auto textures=ReadTextureBundle(rlt,{required.begin(),required.end()});
    if(!lookups.empty())
    {
        auto lookup=ReadTextureBundle(global,{lookups.begin(),lookups.end()});
        Require(lookup.animations.empty(),"Animated lookup outside this owned test's scope");
        for(auto& t:lookup.textures)
            if(std::none_of(textures.textures.begin(),textures.textures.end(),[&](const auto& v){return t.id==v.id;}))
                textures.textures.push_back(std::move(t));
    }
    Require(textures.animations.empty(),"This static owned test does not tick texture animations");
    std::cout<<"Owned selection: "<<objects.size()<<" objects, "<<models.size()<<" models, "<<textures.textures.size()<<" textures\n";
    auto* previous=glGetCurrentResourcePool();
    StaticWorldObjects world(objects,models,textures,{4*1024*1024,24*1024*1024},drain);
    objects.clear();models.clear();textures={};resident.clear();temporary.clear();global.clear();
    glSetCurrentResourcePool(&world.Pool());
    try
    {
        const StaticWorldFrustum broad({{{1,0,0,1e7f},{-1,0,0,1e7f},{0,1,0,1e7f},{0,-1,0,1e7f},{0,0,1,1e7f},{0,0,-1,1e7f}}});
        unsigned differences=0;std::size_t culled_total=0;unsigned long draws=0;
        auto lighting=DefaultGameLighting();
        const nlVector3 target{0,0,0},up{0,0,1};
        for(unsigned pose=0;pose<2;++pose)
        {
            const nlVector3 eye{pose?22.f:0.f,-45,60};const nlVector3 at{pose?22.f:0.f,0,0};
            glMatrixLookAt(matrices.view,eye,at,up);
            const float width=pose?32:70,height=width*.75f;
            glMatrixOrthographicCentered(matrices.projection,width,height,1,2000);
            const auto frustum=Frustum(matrices,width,height,1,2000);
            ColourSamples reference{};
            for(unsigned pass=0;pass<2;++pass)
            {
                StaticWorldSubmission result;ColourSamples pixels{};
                for(unsigned frame=0;frame<23;++frame)
                {
                    begin();result=SubmitStaticWorld(world,opaque,alpha,pass?frustum:broad);
                    RenderOriginalViews(0,lighting,&eye);GXDrawDone();
                    if(frame>=20)pixels=EndFrameAndReadColours();else aurora_end_frame();
                    draws+=aurora_get_stats()->drawCallCount;
                }
                if(!pass)reference=pixels;
                else
                {
                    culled_total+=result.objects-result.visible;
                    for(unsigned i=0;i<9;++i)for(unsigned c=0;c<3;++c)
                    {
                        Require(std::abs(int(reference[i][c])-pixels[i][c])<=3,"Owned culling changed visible pixels");
                        if(std::abs(int(pixels[i][c])-16)>5)++differences;
                    }
                }
                std::cout<<"Owned pose "<<pose<<", culling "<<pass<<": "<<result.visible<<'/'<<result.objects<<" visible, "
                    <<result.opaque_packets<<" opaque, "<<result.alpha_packets<<" alpha; centre RGB "
                    <<unsigned(pixels[4][0])<<','<<unsigned(pixels[4][1])<<','<<unsigned(pixels[4][2])<<'\n';
            }
        }
        Require(differences>6&&draws>0&&culled_total>0,"Owned world lacks visible pixels or culling evidence");
        // Fixed overview for desktop inspection/capture; remains bounded.
        const nlVector3 eye{0,-45,60};glMatrixLookAt(matrices.view,eye,target,up);
        glMatrixOrthographicCentered(matrices.projection,70,52.5f,1,2000);
        const auto frustum=Frustum(matrices,70,52.5f,1,2000);
        std::cout<<"Owned visualization: 600 fixed-camera frames\n"<<std::flush;
        for(unsigned frame=0;frame<600;++frame)
        {
            begin();SubmitStaticWorld(world,opaque,alpha,frustum);RenderOriginalViews(0,lighting,&eye);GXDrawDone();
            if(frame==599)EndFrameAndReadColours();else aurora_end_frame();
        }
        std::cout<<"Owned pixels agree with unculled submissions; "<<culled_total<<" objects culled across two views, "<<draws<<" validation draws\n";
    }
    catch(...){opaque.ResetPackets();alpha.ResetPackets();glSetCurrentResourcePool(previous);throw;}
    glSetCurrentResourcePool(previous);world.Release();
}
}
