#include "frontend_font_fixture.h"
#include "runtime/frontend_font_registry.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glDraw2.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
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
unsigned checks = 0, drains = 0;
FrontendFontRegistry* reentrant = nullptr;
bool fail_drain = false;
void Check(bool yes, const char* message) { ++checks; if (!yes) throw std::runtime_error(message); }
template<class F> void Reject(F f, std::source_location at = std::source_location::current())
{ ++checks; try { f(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid font registry operation accepted at line " + std::to_string(at.line())); }
void Drain()
{
    ++drains;
    if (reentrant) { Reject([&] { reentrant->Release(); }); Reject([&] { reentrant->FinishFrame(); }); }
    if (fail_drain) throw std::runtime_error("Injected font drain failure");
}
std::vector<const glModelPacket*> packets;
void Inspect(GLView*, unsigned long, const glModelPacket* packet) { if (packet) packets.push_back(packet); }
struct Backend : FrameBackend
{
    GLView* view = nullptr;
    bool Acquire() override { return true; }
    void Render() override { if (view) view->Iterate(Inspect); }
    void Finish(bool) override {} void Drain() override {} void WaitIdle() override {} void Cancel() noexcept override {}
};
void Begin(OriginalFrames& frames) { Check(frames.Acquire(), "CPU font frame acquisition failed"); glBeginFrame(); }
void End(FrontendFontRegistry& fonts) { glEndFrame(); glSendFrame(); fonts.FinishFrame(); }
void Session()
{
    const GLMemoryRequirement req[] = {{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};
    const GLMemoryConfig config{65536,65536,req,3,4};
    glInitResourcePools(); glInitMemory(&config); InitializeOriginalGraphicsState(); SetGraphicsCacheInvalidator(Drain);
    MaterialPrograms materials; OriginalViews views(640,480,Drain); ViewMatrices matrices;
    auto* view = new(8,false) GLView(&matrices,{},GLViewSort_None); gRootView.AddChild(view);
    Backend backend; backend.view = view; OriginalFrames frames(backend); auto& pool = *glGetCurrentResourcePool();
    auto font = resources::ReadFrontendFont(font_fixture::Font(), "fe/fonts/fixture", "FiXtUrE");
    const std::array input{font}; const auto alias = resources::FrontendNameHash("fixture");
    const auto free = pool.GetFreeMemory(); const auto slots = glGetTextureManager()->mFreeIndices->mCount;
    nlMatrix4 model; model.SetIdentity(); model.e[12] = 7; model.e[13] = 11;
    Reject([&] { FrontendFontRegistry bad(pool,input,nullptr); });
    Reject([&] { FrontendFontRegistry bad(pool,{},Drain); });
    Reject([&] { FrontendFontRegistry bad(pool,std::array{font,font},Drain); });
    for (unsigned mode = 0; mode < 8; ++mode)
    {
        auto malformed = std::make_shared<resources::FrontendFont>(*font);
        if (mode == 0) malformed->pages[0].pixels.pop_back();
        if (mode == 1) malformed->pages[0].palette.clear();
        if (mode == 2) malformed->pages[0].id = 0xffffffffU;
        if (mode == 3) malformed->pages[1].id = malformed->pages[0].id;
        if (mode == 4) malformed->pages[0].game_format = 3;
        if (mode == 5) malformed->pages[0].width = 0;
        if (mode == 6) malformed->glyphs.at('A').page = 2;
        if (mode == 7) malformed->spacing = NAN;
        std::array<std::shared_ptr<const resources::FrontendFont>,1> bad{malformed};
        Reject([&] { FrontendFontRegistry registry(pool,bad,Drain); });
    }
    Check(pool.GetFreeMemory() == free && glGetTextureManager()->mFreeIndices->mCount == slots,
        "Font preflight mutated pool or texture slots");
    {
        FrontendFontRegistry fonts(pool,input,Drain); reentrant = &fonts;
        struct Unwind
        {
            OriginalFrames& frames; FrontendFontRegistry& fonts;
            ~Unwind()
            {
                if (std::uncaught_exceptions())
                {
                    fail_drain = false; reentrant = nullptr;
                    frames.Cancel(); fonts.FinishFrame();
                }
            }
        } unwind{frames, fonts};
        Check(fonts.Find(alias) == font, "Alias did not retain original font identity");
        Reject([&] { fonts.Find(resources::FrontendNameHash("FiXtUrE")); });
        Reject([&] { fonts.PageIndex(alias,2); });
        const auto first = fonts.PageIndex(alias,0), second = fonts.PageIndex(alias,1);
        Check(first != 0xffff && second != 0xffff && first != second
            && glGetTextureIndex(font->pages[0].id) == first && glGetTextureIndex(font->pages[1].id) == second
            && glGetTextureManager()->mFreeIndices->mCount == slots - 2, "Real font page slots missing");
        Check(glGetTextureManager()->mTextures[first]->m_SwizzledData == font->pages[0].pixels.data(), "Font atlas was not retained");
        Reject([&] { FrontendFontRegistry duplicate(pool,input,Drain); });
        std::thread wrong([&] { Reject([&] { fonts.Find(alias); }); Reject([&] { fonts.Release(); }); }); wrong.join();
        const auto layout = resources::LayoutFrontendText(font,u"A ");
        Reject([&] { fonts.Submit(*view,layout,model); }); fonts.FinishFrame();
        Begin(frames);
        auto empty = resources::LayoutFrontendText(font,u""); Check(fonts.Submit(*view,empty,model) == 0, "Empty text generated packets");
        auto forged = layout; forged.font = std::make_shared<resources::FrontendFont>(*font);
        Reject([&] { fonts.Submit(*view,forged,model); });
        forged = layout; forged.quads[0].page = 2; Reject([&] { fonts.Submit(*view,forged,model); });
        forged = layout; forged.quads[0].u0 = 32; Reject([&] { fonts.Submit(*view,forged,model); });
        forged = layout; forged.quads[0].left = NAN; Reject([&] { fonts.Submit(*view,forged,model); });
        auto invalid_model = model; invalid_model.e[4] = INFINITY; Reject([&] { fonts.Submit(*view,layout,invalid_model); });
        glSetRasterState(GLS_AlphaBlend,2); glSetCurrentRasterState(glHandleizeRasterState());
        glSetTextureState(GLTS_DiffuseWrap,3); glSetCurrentTextureState(glHandleizeTextureState());
        glSetCurrentTexture(123,GLTT_Detail);
        glStateBundle before,after; glStateSave(before);
        const auto old_raster = glHandleizeRasterState(); const auto old_texture = glHandleizeTextureState();
        Check(fonts.Submit(*view,layout,model,{12,34,56,78}) == 2, "Font quad count changed");
        glStateSave(after); Check(std::memcmp(&before,&after,sizeof(before)) == 0
            && old_raster == glHandleizeRasterState() && old_texture == glHandleizeTextureState(), "Font caller state was not restored");
        packets.clear(); view->Iterate(Inspect); Check(packets.size() == 2, "Font source page batches were lost");
        for (unsigned n = 0; n < 2; ++n)
        {
            const auto& p = *packets[n]; const auto& q = layout.quads[n];
            Check(p.numUniqueVertices == 4 && p.primType == GLP_QuadList && !p.indexBuffer && p.numStreams == 3,
                "Original font packet shape changed");
            Check(p.streams[0].stride == 12 && p.streams[1].stride == 4 && p.streams[2].stride == 4, "Original font stream widths changed");
            const auto* pos = static_cast<const float*>(p.streams[0].address);
            const auto* uv = static_cast<const short*>(p.streams[1].address);
            const auto* colour = static_cast<const std::uint8_t*>(p.streams[2].address);
            const std::array<float,8> expected_pos{q.left,q.top,q.left,q.bottom,q.right,q.bottom,q.right,q.top};
            const std::array<float,8> expected_uv{q.u0,q.v0,q.u0,q.v1,q.u1,q.v1,q.u1,q.v0};
            for (unsigned i = 0; i < 4; ++i)
            {
                Check(pos[i*3] == expected_pos[i*2] && pos[i*3+1] == expected_pos[i*2+1] && pos[i*3+2] == 0, "Original quad vertex order changed");
                Check(uv[i*2] == short(expected_uv[i*2]*1024) && uv[i*2+1] == short(expected_uv[i*2+1]*1024), "Original UV quantization changed");
                Check(std::memcmp(colour+i*4,std::array<std::uint8_t,4>{12,34,56,78}.data(),4) == 0, "Font colour native byte width changed");
            }
            auto* binding = static_cast<glTextureBinding*>(p.materialParameters);
            Check(binding->texture == font->pages[n].id && binding->textureIndex == 0xffff && binding->flags == 0,
                "Font authored page hash or original repeat binding changed");
            nlMatrix4 actual; glGetMatrix(p.matrix,actual); Check(std::memcmp(actual.e,model.e,sizeof(model.e)) == 0, "Font model matrix lost");
            Check(glGetRasterState(p.rasterState,GLS_DepthWrite) == 0 && glGetRasterState(p.rasterState,GLS_DepthTest) == 0
                && glGetRasterState(p.rasterState,GLS_AlphaBlend) == 1 && glGetRasterState(p.rasterState,GLS_AlphaTest) == 1
                && glGetRasterState(p.rasterState,GLS_AlphaTestRef) == 0 && glGetRasterState(p.rasterState,GLS_Culling) == 0,
                "Original font raster state changed");
        }
        // Independent packed fixture: A's authored x=16, extent15, y=0.
        const auto* a_pos = static_cast<const float*>(packets[0]->streams[0].address);
        const auto* a_uv = static_cast<const short*>(packets[0]->streams[1].address);
        Check(a_pos[0] == -1 && a_pos[1] == -2 && a_pos[6] == 14 && a_pos[7] == 13
            && a_uv[0] == 512 && a_uv[1] == 0 && a_uv[4] == 992 && a_uv[5] == 480, "Independent glyph geometry oracle differs");
        Check(fonts.Submit(*view,layout,model) == 2, "Multiple text submissions were rejected");
        Reject([&] { fonts.Release(); }); Reject([&] { fonts.FinishFrame(); });
        End(fonts);
        Begin(frames); fonts.Submit(*view,layout,model); frames.Cancel();
        fail_drain = true; Reject([&] { fonts.FinishFrame(); }); fail_drain = false;
        Reject([&] { fonts.Release(); }); fonts.FinishFrame();
        Begin(frames); glFrameAlloc(65536-128,GLM_VertexData);
        glStateSave(before); Reject([&] { fonts.Submit(*view,layout,model); }); glStateSave(after);
        Check(std::memcmp(&before,&after,sizeof(before)) == 0, "Failed polygon allocation lost caller state");
        Reject([&] { fonts.Submit(*view,empty,model); });
        Reject([&] { fonts.Release(); }); frames.Cancel(); fonts.FinishFrame();
        Begin(frames);
        const auto caller_matrix = glGetCurrentMatrix(); gl_GetCurrentStateBundle()->matrix = 0;
        Reject([&] { fonts.Submit(*view,layout,model); });
        gl_GetCurrentStateBundle()->matrix = caller_matrix;
        Reject([&] { fonts.Submit(*view,empty,model); }); frames.Cancel(); fonts.FinishFrame();
        Begin(frames); fonts.Submit(*view,layout,model); End(fonts);
        const auto nested = pool.MarkResource(); Reject([&] { fonts.Release(); }); pool.ReleaseResource(nested);
        auto* original = glGetTextureManager()->mTextures[first]; glGetTextureManager()->mTextures[first] = nullptr;
        Reject([&] { fonts.PageIndex(alias,0); }); glGetTextureManager()->mTextures[first] = original;
        fail_drain = true; Reject([&] { fonts.Release(); }); fail_drain = false;
        Check(fonts.Active(), "Failed drain discarded ownership"); fonts.Release(); fonts.Release();
        Check(!fonts.Active(), "Font registry stayed active"); Reject([&] { fonts.Find(alias); }); reentrant = nullptr;
    }
    Check(pool.GetFreeMemory() == free && glGetTextureManager()->mFreeIndices->mCount == slots
        && glGetTextureIndex(font->pages[0].id) == 0xffff, "Font teardown leaked pools or texture indices");
    // Only one free slot remains: registration succeeds for page0 then fails
    // for page1. The real mark must unwind both inventory records and the slot.
    {
        const auto mark = pool.MarkResource(); PlatTexture held[3];
        for (unsigned i = 0; i < 3; ++i) glRegisterTexture(100+i,held+i,&pool);
        const auto before = pool.GetFreeMemory();
        Reject([&] { FrontendFontRegistry fonts(pool,input,Drain); });
        Check(pool.GetFreeMemory() == before && glGetTextureManager()->mFreeIndices->mCount == 1
            && glGetTextureIndex(font->pages[0].id) == 0xffff && glGetTextureIndex(font->pages[1].id) == 0xffff,
            "Partial page registration did not roll back"); pool.ReleaseResource(mark);
    }
    Check(pool.GetFreeMemory() == free && glGetTextureManager()->mFreeIndices->mCount == slots, "Font rollback lost manager capacity");
    // Retained atlases outlive file/description/layout producers; Release drops
    // the last owner only after the real pool has discarded its borrowed data.
    std::weak_ptr<const resources::FrontendFont> weak;
    {
        auto only = resources::ReadFrontendFont(font_fixture::Font("fe/fonts/retained"),"fe/fonts/retained","retained"); weak = only;
        auto owner = std::make_unique<FrontendFontRegistry>(pool,std::array{only},Drain); only.reset();
        Check(!weak.expired(), "Registered atlas lifetime was not retained"); owner->Release(); Check(weak.expired(), "Released atlas owner leaked");
    }
    frames.Release(); views.Release(); materials.Release(); glShutdownMemory();
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> a(1024*1024),b(1024*1024); ResetStartupMemory();
        StandardAllocator.Initialize(a.data(),a.size()*8); VirtualAllocator.Initialize(b.data(),b.size()*8); gMemoryInitialized=1;
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        for (unsigned n=0;n<3;++n) { Session(); Check(StandardAllocator.TotalFreeMemory()==free1 && VirtualAllocator.TotalFreeMemory()==free2,"Repeated font sessions leaked arenas"); }
        ResetStartupMemory(); std::cout<<checks<<" font registry checks passed, "<<drains<<" drains\n";
    }
    catch(const std::exception& e) { std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n"; return 1; }
}
