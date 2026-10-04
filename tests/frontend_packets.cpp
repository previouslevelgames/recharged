#include "frontend_packets_fixture.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include "NL/gl/gl.h"
#include "NL/gl/glState.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0,drains=0;
FrontendPacketRenderer* reentrant=nullptr;
bool fail_drain=false;
void Check(bool yes,const char* what) { ++checks; if (!yes) throw std::runtime_error(what); }
template<class F> void Reject(F f,std::source_location at=std::source_location::current())
{ ++checks; try { f(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid packet operation accepted at line "+std::to_string(at.line())); }
void Drain()
{
    ++drains;
    if (reentrant) { Reject([&]{reentrant->Release();}); Reject([&]{reentrant->Current();}); }
    if (fail_drain) throw std::runtime_error("Injected mixed drain failure");
}
void Invalidate() {}
std::vector<const glModelPacket*> packets;
void Inspect(GLView*,unsigned long,const glModelPacket* p) { if(p) packets.push_back(p); }
struct Backend : FrameBackend
{
    bool Acquire() override { return true; } void Render() override {} void Finish(bool) override {}
    void Drain() override {} void WaitIdle() override {} void Cancel() noexcept override {}
};
void Begin(OriginalFrames& f) { Check(f.Acquire(),"Mixed frame acquisition failed"); glBeginFrame(); }
void End(FrontendPacketRenderer& r) { glEndFrame(); glSendFrame(); r.FinishFrame(); }
void Session()
{
    const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};
    const GLMemoryConfig config{65536,65536,req,3,8};
    glInitResourcePools(); glInitMemory(&config); InitializeOriginalGraphicsState(); SetGraphicsCacheInvalidator(Invalidate);
    MaterialPrograms materials; OriginalViews views(640,480,Invalidate); ViewMatrices matrices;
    auto* view=new(8,false) GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);
    auto* sorted=new(8,false) GLView(&matrices,{},GLViewSort_Texture);gRootView.AddChild(sorted);
    Backend backend; OriginalFrames frames(backend); auto& pool=*glGetCurrentResourcePool();
    const auto free=pool.GetFreeMemory(); const auto slots=glGetTextureManager()->mFreeIndices->mCount;
    const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
    auto first=frontend_packet_fixture::Frame(); auto replacement=frontend_packet_fixture::Frame(1);
    {
        FrontendPacketRenderer renderer(Drain); reentrant=&renderer;
        struct Unwind { OriginalFrames& frames; FrontendPacketRenderer& renderer; ~Unwind() {
            if(std::uncaught_exceptions()) { fail_drain=false;reentrant=nullptr;frames.Cancel();renderer.FinishFrame(); }
        }} unwind{frames,renderer};
        Check(!renderer.Current() && !renderer.Textures(),"Empty renderer claimed a visible scene");
        Reject([&]{renderer.Prepare({});});
        renderer.Prepare(first); Check(renderer.Current()==first && renderer.Textures()==3,"Initial mixed graphics publication failed");
        const auto old_index=glGetTextureIndex(frontend_packet_fixture::ImageHash);
        Check(old_index!=0xffff && glGetTextureManager()->mFreeIndices->mCount==slots-3,"Mixed textures lack real slots");
        Check(glGetCurrentResourcePool()==&pool && pool.GetFreeMemory()==free,"Mixed graphics changed the world pool");
        std::thread other([&]{Reject([&]{renderer.Current();});Reject([&]{renderer.Prepare(replacement);});});other.join();
        Reject([&]{renderer.Submit(*view,first);});
        // Validate complete batches before any original frame allocation.
        for(unsigned mode=0;mode<5;++mode)
        {
            auto bad=std::make_shared<FrontendSessionFrame>(*first);
            auto& image=std::get<resources::FrontendLayoutImage>(bad->layout.entries[1]);
            if(mode==0) image.vertices[0].u=32;
            if(mode==1) image.transform[0]=NAN;
            if(mode==2) image.transform[3]=1;
            if(mode==3) image.blend=8;
            if(mode==4) image.texture=replacement->images->textures.at(frontend_packet_fixture::ImageHash);
            Reject([&]{renderer.Prepare(bad);}); Check(renderer.Current()==first,"Invalid mixed layout changed publication");
        }
        Begin(frames);Reject([&]{renderer.Prepare(replacement);});Reject([&]{renderer.Submit(*sorted,first);});
        glSetRasterState(GLS_AlphaBlend,2);glSetCurrentRasterState(glHandleizeRasterState());
        glSetTextureState(GLTS_DiffuseWrap,3);glSetCurrentTextureState(glHandleizeTextureState());
        glStateBundle before,after;glStateSave(before);const auto raster=glHandleizeRasterState();const auto texture=glHandleizeTextureState();
        auto three=std::make_shared<FrontendSessionFrame>(*first);
        auto text=std::get<resources::FrontendLayoutText>(three->layout.entries[0]);
        text.layout=resources::LayoutFrontendText(text.layout.font,u" "); three->layout.entries.push_back(text);
        Check(renderer.Submit(*view,three)==3,"Mixed quad count changed");
        glStateSave(after);Check(!std::memcmp(&before,&after,sizeof(before)) && raster==glHandleizeRasterState() && texture==glHandleizeTextureState(),"Mixed submission lost caller state");
        packets.clear();view->Iterate(Inspect);Check(packets.size()==3,"Mixed original packet count differs");
        const auto& font=*first->visuals->text;
        const std::array<std::uint32_t,3> hashes{font.pages[0].id,frontend_packet_fixture::ImageHash,font.pages[1].id};
        for(unsigned n=0;n<3;++n)
        {
            const auto& p=*packets[n];const auto* binding=static_cast<const glTextureBinding*>(p.materialParameters);
            Check(binding->texture==hashes[n] && binding->flags==0,"Mixed text/image order or repeat wrapping changed");
            Check(p.primType==GLP_QuadList && p.numUniqueVertices==4 && p.numStreams==3 && p.streams[1].stride==4,"Mixed packets no longer use original short-UV quads");
            Check(glGetRasterState(p.rasterState,GLS_DepthTest)==0 && glGetRasterState(p.rasterState,GLS_DepthWrite)==0
                && glGetRasterState(p.rasterState,GLS_AlphaTest)==1 && glGetRasterState(p.rasterState,GLS_Culling)==0,"Mixed raster defaults changed");
        }
        const auto* pos=static_cast<const float*>(packets[1]->streams[0].address);
        const auto* uv=static_cast<const short*>(packets[1]->streams[1].address);
        Check(pos[0]==-20 && pos[1]==20 && pos[3]==-20 && pos[4]==-20 && pos[6]==20 && pos[7]==-20
            && uv[0]==0 && uv[1]==0 && uv[4]==1024 && uv[5]==1024,"Image glQuad3 vertex/UV oracle differs");
        Reject([&]{renderer.Release();});Reject([&]{renderer.FinishFrame();});End(renderer);
        // All eight authored image blend modes pass through original raster bits.
        for(unsigned blend=0;blend<8;++blend)
        {
            auto next=std::make_shared<FrontendSessionFrame>(*first);std::get<resources::FrontendLayoutImage>(next->layout.entries[1]).blend=blend;
            Begin(frames);renderer.Submit(*view,next);packets.clear();view->Iterate(Inspect);
            Check(glGetRasterState(packets[1]->rasterState,GLS_AlphaBlend)==blend,"Original image blend mode changed");End(renderer);
        }
        renderer.Prepare(first);fail_drain=true;Reject([&]{renderer.Prepare(replacement);});fail_drain=false;
        Check(renderer.Current()==first && glGetTextureIndex(frontend_packet_fixture::ImageHash)==old_index,"Failed drain retired visible graphics");
        // A texture animation alias has precedence over ordinary textures.
        const auto marker=pool.MarkResource();GLTextureAnim animation{};
        GLAnimTex anim_frame{old_index,1};animation.m_textureIndex=0xffff;animation.m_nNumTextures=animation.m_NativeFrameCount=1;
        animation.m_pAnimTex=&anim_frame;animation.m_ePlayMode=GLAnimMode_Loop;animation.m_nPlayDir=1;
        constexpr auto alias=frontend_packet_fixture::ImageHash+1;
        auto alias_frame=frontend_packet_fixture::Frame();
        auto alias_texture=std::make_shared<resources::Texture>(*alias_frame->images->textures.begin()->second);alias_texture->id=alias;
        auto alias_catalog=std::make_shared<resources::FrontendImageCatalog>();alias_catalog->textures.emplace(alias,alias_texture);
        alias_frame->images=alias_catalog;std::get<resources::FrontendLayoutImage>(alias_frame->layout.entries[1]).texture=alias_texture;
        pool.m_inventory->AddTextureAnim(alias,&animation);glGetTextureManager()->RegisterTextureAnim(&animation);
        Reject([&]{renderer.Prepare(alias_frame);});pool.ReleaseResource(marker);
        Check(renderer.Current()==first && glGetTextureIndex(frontend_packet_fixture::ImageHash)==old_index,"Animation alias collision retired visible graphics");
        // Only two slots remain, so candidate page registration must partially
        // complete then roll back without retiring the old generation.
        const auto mark=pool.MarkResource();PlatTexture held[3];
        for(unsigned i=0;i<3;++i)glRegisterTexture(100+i,held+i,&pool);
        Reject([&]{renderer.Prepare(replacement);});
        Check(renderer.Current()==first && glGetTextureIndex(frontend_packet_fixture::ImageHash)==old_index
            && glGetTextureManager()->mFreeIndices->mCount==2,"Partial candidate registration lost old slots/lookup");pool.ReleaseResource(mark);
        renderer.Prepare(replacement);
        const auto replacement_index=glGetTextureIndex(frontend_packet_fixture::ImageHash);
        Check(renderer.Current()==replacement && renderer.Textures()==3 && replacement_index!=old_index
            && glGetTextureManager()->mFreeIndices->mCount==slots-3,"Same-hash replacement did not recover old slots");
        Check(glGetTextureManager()->mTextures[replacement_index]->m_PaletteData==reinterpret_cast<const u16*>(replacement->images->textures.at(frontend_packet_fixture::ImageHash)->palette.data()),"Replacement hash resolved the retired atlas");
        Begin(frames);Reject([&]{renderer.Submit(*view,first);});renderer.Submit(*view,replacement);frames.Cancel();
        fail_drain=true;Reject([&]{renderer.FinishFrame();});fail_drain=false;Reject([&]{renderer.Prepare(first);});renderer.FinishFrame();
        Begin(frames);glFrameAlloc(65536-128,GLM_VertexData);Reject([&]{renderer.Submit(*view,replacement);});
        Reject([&]{renderer.Submit(*view,replacement);});frames.Cancel();renderer.FinishFrame();
        Check(renderer.Current()==replacement,"Failed packet frame changed graphics ownership");
        for(unsigned n=0;n<6;++n)
        {
            auto next=frontend_packet_fixture::Frame(n&1);renderer.Prepare(next);Begin(frames);renderer.Submit(*view,next);End(renderer);
            Check(glGetTextureManager()->mFreeIndices->mCount==slots-3,"Repeated replacement leaked slots");
        }
        renderer.Release();renderer.Release();Reject([&]{renderer.Current();});reentrant=nullptr;
    }
    Check(pool.GetFreeMemory()==free && glGetTextureManager()->mFreeIndices->mCount==slots,"Mixed teardown changed parent pool or leaked texture slots");
    // View sorters are retained by their views until views.Release; baseline
    // arena equality is checked after all original graphics objects are gone.
    (void)free1;(void)free2;frames.Release();views.Release();materials.Release();glShutdownMemory();
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t>a(1024*1024),b(1024*1024);ResetStartupMemory();
        StandardAllocator.Initialize(a.data(),a.size()*8);VirtualAllocator.Initialize(b.data(),b.size()*8);gMemoryInitialized=1;
        const auto x=StandardAllocator.TotalFreeMemory(),y=VirtualAllocator.TotalFreeMemory();
        for(unsigned i=0;i<3;++i){Session();Check(StandardAllocator.TotalFreeMemory()==x && VirtualAllocator.TotalFreeMemory()==y,"Mixed repeated sessions leaked arenas");}
        ResetStartupMemory();std::cout<<checks<<" mixed frontend packet checks passed, "<<drains<<" drains\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
