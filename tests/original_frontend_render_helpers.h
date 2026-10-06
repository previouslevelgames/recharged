#pragma once
#include "NL/gl/glView.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glDraw3.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glState.h"
#include "NL/gl/glStruct.h"
#include "NL/glx/glxMemory.h"
#include "NL/glx/glxDisplayList.h"
#include "NL/glx/glxSkinMatrix.h"
#include "NL/glx/GXVertexColourTextureMaterialProgram.h"
#include "NL/glx/GXScissoredVertexColourTextureMaterialProgram.h"
#include "Game/FE/feRender.h"
#include "Game/FE/feScene.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feText.h"
#include "Game/FE/tlSlide.h"
#include "Game/FE/tlImageInstance.h"
#include "Game/FE/tlInstance.inl"
#include "Game/FE/feFontResource.h"
#include "Game/FE/feTextureResource.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <fstream>
#include <array>
#include <string>

GLPacketSorter* CreateUnsortedPacketSorter();
namespace {
void Check(bool, const char*);
std::vector<const glModelPacket*> render167_packets;
void Capture167(GLView*, unsigned long flags, const glModelPacket* packet)
{
    if (flags & 0x80) render167_packets.push_back(packet);
}
struct GlyphOracle167
{
    unsigned page, advance, width, height, ascent; int offset;
    std::array<std::uint32_t,4> uv;
};
struct RenderOracle167
{
    int kern_ab; std::array<GlyphOracle167,3> glyphs;
    explicit RenderOracle167(const std::string& path)
    {
        std::ifstream input(path+".render"); input >> kern_ab;
        for(auto& g:glyphs)
            input >> g.page >> g.advance >> g.width >> g.height >> g.ascent >> g.offset
                  >> g.uv[0] >> g.uv[1] >> g.uv[2] >> g.uv[3];
        Check(bool(input),"Independent raw glyph oracle missing");
    }
};
void DirectGlyphPackets167(GLView& view,nlFont& font,const RenderOracle167& oracle,bool clipped,bool flip)
{
    const unsigned short chars[]={'A','B','?',0}; unsigned short buffer[4];
    FontCharString text(chars,&font,buffer);
    for(unsigned i=0;i<3;++i)
    {
        const auto& g=oracle.glyphs[i];const auto& actual=font.GetGlyphInfo(chars[i]);
        Check(actual.Page==g.page && actual.Advance==g.advance && actual.RenderWidth==g.width
              && actual.RenderHeight==g.height && actual.RenderAscent==g.ascent && actual.Offset==g.offset,
              "Actual callback glyph layout differs from raw descriptor grammar");
        Check(std::bit_cast<std::uint32_t>(actual.uv.x)==g.uv[0] && std::bit_cast<std::uint32_t>(actual.uv.y)==g.uv[1]
              && std::bit_cast<std::uint32_t>(actual.uvEnd.x)==g.uv[2] && std::bit_cast<std::uint32_t>(actual.uvEnd.y)==g.uv[3],
              "Actual callback glyph UV bits differ from independent packed/raw coordinates");
    }
    render167_packets.clear();view.Iterate(Capture167);const auto old_count=render167_packets.size();
    if(clipped) font.SetScissorBox({9,13,47,31});
    else font.DisableScissorBox();
    nlColour colour;nlColourSet(colour,31,63,95,127);
    font.DrawString(&view,text,{13,19},colour,colour,-1,nlFont::PASS_Text,flip,nullptr,nullptr);
    render167_packets.clear();view.Iterate(Capture167);
    std::size_t next=old_count;
    for(unsigned page=0;page<font.m_PageCount;++page)
    {
        unsigned glyph_count=0;for(const auto& g:oracle.glyphs)if(g.page==page)++glyph_count;
        if(!glyph_count)continue;
        Check(next<render167_packets.size(),"Original page loop omitted a source glyph packet");
        const auto* packet=render167_packets[next++];
        Check(packet->numUniqueVertices==glyph_count*4 && packet->primType==3,
              "Original page grouping/quad primitive changed");
        Check(static_cast<GLMaterialProgram*>(packet->materialProgram)->programHash==(clipped?0x0027bcf6:0xd3e572da),
              "Original text scissor choice changed");
        Check(static_cast<const glTextureBinding*>(packet->materialParameters)->texture==font.m_TextureHandles[page],
              "Original text page selected an alternate asset");
        const auto* positions=static_cast<const float*>(packet->streams[0].address);
        const auto* colours=static_cast<const unsigned char*>(packet->streams[2].address);
        unsigned vertex=0;float x=13.5f;
        for(unsigned i=0;i<3;++i)
        {
            const auto& g=oracle.glyphs[i];x+=float(g.offset);
            const float y=19.5f+(flip?float(g.ascent):-float(g.ascent));
            const float right=x+float(g.width)-1;
            const float bottom=y+(flip?-float(g.height-1):float(g.height-1));
            const float xy[8]={x,y,x,bottom,right,bottom,right,y};
            const float uv[8]={std::bit_cast<float>(g.uv[0]),std::bit_cast<float>(g.uv[1]),
                              std::bit_cast<float>(g.uv[0]),std::bit_cast<float>(g.uv[3]),
                              std::bit_cast<float>(g.uv[2]),std::bit_cast<float>(g.uv[3]),
                              std::bit_cast<float>(g.uv[2]),std::bit_cast<float>(g.uv[1])};
            if(g.page==page)
                for(unsigned j=0;j<4;++j,++vertex)
                {
                    Check(positions[vertex*3]==xy[j*2] && positions[vertex*3+1]==xy[j*2+1] && positions[vertex*3+2]==0,
                          "Original glyph position/flip/kerning differs from independent raw equation");
                    for(unsigned k=0;k<4;++k)Check(colours[vertex*4+k]==colour.c[k],"Original RGBA byte stream changed");
                    for(unsigned k=0;k<2;++k)
                        if(clipped)
                            Check(static_cast<const float*>(packet->streams[1].address)[vertex*2+k]==uv[j*2+k],
                                  "Original float UV glyph stream differs");
                        else
                            Check(static_cast<const short*>(packet->streams[1].address)[vertex*2+k]==short(uv[j*2+k]*1024),
                                  "Original Wii quantized UV glyph stream differs");
                }
            x+=float(int(g.advance)+(i==0?oracle.kern_ab:0))*font.m_Metrics.Spacing;
        }
    }
    Check(next==render167_packets.size(),"Original font draw inserted unexpected glyph/page packet");
    font.DisableScissorBox();
}
void DisplayListBytes167()
{
    static const unsigned char opcodes[]={0x90,0x98,0xa0,0x80,0xa8,0xb0};
    const unsigned short indices[]={7,1,0x1020};
    std::array<unsigned char,(0x1020+1)*4> stitch{};
    stitch[7*4]=2;stitch[1*4]=9;stitch[0x1020*4]=5;
    for(unsigned prim=0;prim<6;++prim)for(unsigned streams=1;streams<=4;++streams)
    for(bool skin:{false,true})for(bool permanent:{false,true})
    {
        std::array<glModelStream,5> descriptors{};
        for(unsigned i=0;i<streams;++i)descriptors[i].id=i+1;
        if(skin){descriptors[streams].id=7;descriptors[streams].address=stitch.data();}
        glModelPacket packet{};packet.indexBuffer=const_cast<unsigned short*>(indices);packet.numVertices=3;
        packet.primType=prim;packet.numStreams=streams+skin;packet.streams=descriptors.data();
        packet.materialProgram=GXVertexColourTextureMaterialProgram::Instance;
        glplatFinalizePacket(&packet,permanent,glGetCurrentResourcePool());
        DisplayList* list=packet.displayList;
        if(!permanent)list=dlMakeDisplayList(&packet,nullptr,false);
        Check(list && list->magic==0xba7ef00d && list->numStreams==streams && list->hasColorStream==skin,
              "Whole original display-list selection/header changed");
        std::vector<unsigned char> expected{opcodes[prim],0,3};
        const unsigned char slot[]={9,0,18};
        for(unsigned i=0;i<3;++i)
        {
            if(skin)expected.push_back(slot[i]);
            for(unsigned j=0;j<streams;++j){expected.push_back(indices[i]>>8);expected.push_back(indices[i]&255);}
        }
        expected.resize((expected.size()+31)&~std::size_t(31),0);
        Check(list->size==expected.size(),"Original display-list padding geometry changed");
        Check(std::memcmp(list->list,expected.data(),expected.size())==0,
              "Original GX opcode/index/stitch/padding bytes differ from independent big-endian oracle");
        Check(packet.skinnedVertices==0 && packet.skinnedNormals==0,
              "Whole original finalizer source default fields changed");
    }
    glModelPacket empty{};
    Check(dlMakeDisplayList(&empty,nullptr,false)==nullptr,"Original null index display list branch changed");
    empty.indexBuffer=const_cast<unsigned short*>(indices);
    Check(dlMakeDisplayList(&empty,nullptr,false)==nullptr,"Original empty stream display list branch changed");
}
void RenderLoadedFont167(nlFont& font, unsigned long hash, FEResourceManager& resources,const std::string& path)
{
    GLView view;
    glSetDefaultState(false);
    view.m_CreateSorter = CreateUnsortedPacketSorter;
    const auto token = static_cast<eGLView>(reinterpret_cast<glViewHandle>(&view));
    Check(reinterpret_cast<GLView*>(token) == &view, "Native eGLView loses actual view address");
    if (sizeof(void*) > 4) Check(reinterpret_cast<std::uintptr_t>(&view) > UINT32_MAX, "Fixture does not exercise high view address");
    glQuad3 quad{};
    nlMatrix4 identity; identity.SetIdentity();
    quad.SetupRotatedRectangle(30.0f, 20.0f, identity, false, true);
    Check(quad.Attach(token, 0), "Original glQuad3 attachment failed");
    render167_packets.clear(); view.Iterate(Capture167);
    Check(render167_packets.size() == 1, "Original view iteration omitted original quad");
    const auto* qp = render167_packets.front();
    Check(qp->numUniqueVertices == 4 && qp->primType == 3 && qp->numStreams == 3,
          "Original quad primitive/stream selection changed");
    Check(qp->displayList == nullptr && qp->skinnedVertices == 0 && qp->skinnedNormals == 0,
          "Whole original finalizer did not retain source nonpermanent defaults");
    const auto* binding = static_cast<const glTextureBinding*>(qp->materialParameters);
    Check(binding->texture == gWhiteTextureID && binding->textureIndex == 0xffff && binding->flags == 3,
          "Original untextured quad white binding/wrap branch was omitted");
    const auto* xyz = static_cast<const float*>(qp->streams[0].address);
    const float expected_xyz[] = {-15,-10,0, 15,-10,0, 15,10,0, -15,10,0};
    for (unsigned i=0;i<12;++i) Check(xyz[i] == expected_xyz[i], "Original rotated rectangle geometry differs");

    RenderOracle167 oracle(path);
    DirectGlyphPackets167(view,font,oracle,false,false);
    DirectGlyphPackets167(view,font,oracle,false,true);
    DirectGlyphPackets167(view,font,oracle,true,false);
    DisplayListBytes167();
    FEFontResource resource{}; resource.m_type=FERT_FONT; resource.m_hashID=hash;
    resources.QueueResourceLoad(&resource, &VirtualAllocator);
    Check(!resource.IsValid(), "FE queue fabricated loaded font validity");
    resources.Run(0.0f);
    Check(resource.IsValid() && resource.GetFontReference()==&font, "Actual FE font lookup/reference did not complete");
    FEText text; text.m_pFeFontResource=&resource;
    nlColourSet(text.m_attributes.colour,255,255,255,255);
    TLTextInstance instance(&text);
    instance.m_OverloadedAttributes.BoxSize.x=500; instance.m_OverloadedAttributes.BoxSize.y=100;
    const unsigned short chars[]={ 'A','B','?',0 };
    instance.SetString(chars); instance.m_fStartTime=0; instance.m_fDuration=10;
    FELibObject image_object; nlColourSet(image_object.m_attributes.colour,255,255,255,255);
    TLImageInstance image(&image_object); FETextureResource texture_resource;
    image.m_pTextureResource=&texture_resource; image.m_fStartTime=0; image.m_fDuration=10;
    image.m_next=&instance; image.m_prev=&instance;
    instance.m_next=&image; instance.m_prev=&image;
    TLSlide slide; slide.pChildren=&image; slide.m_time=0;
    FEPresentation presentation{}; presentation.m_slides=&slide; presentation.m_currentSlide=&slide;
    FEPackage package{}; package.m_pFEPresentation=&presentation;
    FEScene scene; scene.m_pFEPackage=&package; scene.m_uRenderView=reinterpret_cast<glViewHandle>(&view);
    scene.m_matView.SetIdentity();
    render167_packets.clear(); view.Iterate(Capture167);
    const auto text_begin=render167_packets.size();
    FERender::RenderScene(&scene); render167_packets.clear(); view.Iterate(Capture167);
    Check(render167_packets.size() > text_begin, "Original FERender/TLText/nlTextBox/nlFont path produced no text packets");
    Check(!texture_resource.IsValid(), "Original invalid image readiness was fabricated");
    Check(FERender::m_pRenderScene == nullptr, "Original render scene call lifetime changed");
    for (std::size_t i=text_begin;i<render167_packets.size();++i)
    {
        const auto* packet=render167_packets[i];
        Check(static_cast<GLMaterialProgram*>(packet->materialProgram)->programHash == 0xd3e572da,
              "Original ordinary font chose a replacement material");
        Check(packet->numUniqueVertices % 4 == 0 && packet->streams[1].stride == 4,
              "Original font quad/quantized UV stream changed");
    }
    instance.SetScissorBox(9,13,47,31);
    FERender::RenderScene(&scene);
    render167_packets.clear(); view.Iterate(Capture167);
    bool clipped=false;
    for (const auto* packet:render167_packets)
    {
        if (static_cast<GLMaterialProgram*>(packet->materialProgram)->programHash != 0x0027bcf6) continue;
        clipped=true;
        Check(packet->streams[1].stride == 8, "Original clipped text lost float UV stream");
        const auto* p=static_cast<const GXScissoredTextureParameters*>(packet->materialParameters);
        Check(p->scissorX == 9 && p->scissorY == 13 && p->scissorWidth == 47 && p->scissorHeight == 31,
              "Original whole array setter did not copy the exact four Wii32 scissor words");
    }
    Check(clipped && !font.m_bScissorBox, "Original TLText scissor packet/reset was omitted");
    instance.SetVisible(false);
    const auto before=render167_packets.size(); FERender::RenderScene(&scene);
    render167_packets.clear(); view.Iterate(Capture167);
    Check(render167_packets.size()==before, "Original FE visibility decision was changed");
    // This selected-method fixture authored stack records; actual LoadPackage,
    // scene handlers/on-demand image loading and module allocations are separate gates.
    scene.m_pFEPackage=nullptr;
    resources.UnloadResource(&resource);
}
}
