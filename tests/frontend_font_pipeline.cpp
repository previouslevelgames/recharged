#include "platform/graphics_stats.h"
#include "frontend_font_fixture.h"
#include "runtime/frontend_font_registry.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
using namespace mscharged;
namespace
{
std::atomic_uint errors = 0; unsigned checks = 0, draws = 0;
void Check(bool yes,const char* message) { ++checks; if (!yes) throw std::runtime_error(message); }
void Log(AuroraLogLevel level,const char*,const char* message,unsigned n)
{ if (level >= LOG_ERROR) ++errors; std::cerr.write(message,n); std::cerr << '\n'; }
void Drain() { AuroraGXSync(); }
void Invalidate() { GXInvalidateVtxCache(); GXInvalidateTexAll(); }
struct Session
{
    bool live = false;
    ~Session() { if (live) { Drain(); glShutdownMemory(); ResetStartupFiles(); ResetStartupMemory(); aurora_shutdown(); } }
};
void Acquire(OriginalFrames& frames)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
            Check(event->type != AURORA_EXIT,"Font pipeline window closed");
        if (frames.Acquire()) return;
        Check(std::chrono::steady_clock::now() < end,"Font pipeline acquisition timed out"); SDL_Delay(1);
    }
}
void Case(const char* name, GLResourcePool& pool, GLView& view, OriginalFrames& frames, AuroraFrames& backend,
    std::shared_ptr<const resources::FrontendFont> font, resources::FontLayout layout,
    std::array<std::uint8_t,4> modulation, std::array<int,3> expected, float x = 315, bool second_submission = false)
{
    FrontendFontRegistry registry(pool,std::array{font},Drain);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    unsigned quiet = 0; nlMatrix4 model; model.SetIdentity(); model.e[12] = x; model.e[13] = 237;
    try
    {
        for (unsigned frame=0;;++frame)
        {
            Check(std::chrono::steady_clock::now() < deadline,"Font pipeline warmup timed out");
            Acquire(frames); glBeginFrame(); GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR); GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
            Check(registry.Submit(view,layout,model,modulation) == layout.quads.size(),"Font packet count differs");
            if (second_submission) registry.Submit(view,resources::LayoutFrontendText(font,u" "),model);
            const bool sample = frame >= 12 && quiet >= 2;
            backend.read_colours = sample; glEndFrame(); glSendFrame(); registry.FinishFrame();
            draws += aurora_get_stats()->drawCallCount;
            const auto pending = mscharged::platform::GetQueuedPipelineCount();
            quiet = pending ? 0 : quiet + 1;
            if (sample && !pending) break;
        }
        const auto& c = backend.colours[4];
        std::cout << name << " RGB=" << unsigned(c[0]) << ',' << unsigned(c[1]) << ',' << unsigned(c[2]) << '\n';
        for (unsigned i=0;i<3;++i)
        {
            Check(std::abs(int(c[i])-expected[i]) <= 3,"Original font packet pixel oracle differs");
            Check(std::abs(int(backend.colours[0][i])-std::array{20,24,30}[i]) <= 3,"Font packets changed background pixels");
        }
        // Queue actual geometry then cancel. Registry pages remain alive until
        // cancellation drains and clears original view packets.
        Acquire(frames); glBeginFrame(); registry.Submit(view,layout,model); frames.Cancel(); registry.FinishFrame(); registry.Release();
    }
    catch (...) { frames.Cancel(); registry.FinishFrame(); throw; }
}
}
int main(int argc,char** argv)
{
    try
    {
        const char* base=SDL_GetBasePath(); Check(base,"Missing font pipeline binary directory");
        const std::string path=std::string(base)+"frontend-font-registry-test-data"; std::filesystem::create_directories(path);
        AuroraConfig config{}; config.appName="Charged original font polygon checks"; config.userPath=config.cachePath=path.c_str(); config.resourcesPath=base;
        config.desiredBackend=BACKEND_VULKAN; config.enableBackendValidation=true; config.windowWidth=640; config.windowHeight=480;
        config.windowPosX=config.windowPosY=-1; config.logLevel=LOG_WARNING; config.logCallback=Log; config.vsync=true;
        config.mem1Size=MEM1_DEFAULT_SIZE; config.mem2Size=64*1024*1024;
        Session session; const auto info=aurora_initialize(argc,argv,&config); session.live=true;
        Check(info.window&&info.backend==BACKEND_VULKAN,"Font polygon pipeline requires Vulkan");
        ImGui::GetIO().IniFilename=nullptr; ImGui::GetIO().LogFilename=nullptr; InitializeStartupOS(); nlInitMemory();
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools(); InitializeOriginalGraphicsMemory(); InitializeOriginalGraphicsState(); VIInit(); VIConfigure(&GXNtsc480IntDf);
        alignas(32) std::array<unsigned char,65536> fifo{}; GXInit(fifo.data(),fifo.size()); AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT); SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms materials; OriginalViews views(640,480,Drain); ViewMatrices matrices;
        // Projection matrices retain GX row-major storage; model matrices use
        // the original column-vector accessor convention. Use the selected
        // original projection constructor, as the inspector integration does.
        glMatrixOrthographic(matrices.projection,640,480);
        auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None); gRootView.AddChild(view);
        AuroraFrames backend; OriginalFrames frames(backend); auto& pool=*glGetCurrentResourcePool();
        auto font=resources::ReadFrontendFont(font_fixture::Font(),"fe/fonts/fixture","fixture");
        const auto first=resources::LayoutFrontendText(font,u"A"),red=resources::LayoutFrontendText(font,u" ");
        Case("white",pool,*view,frames,backend,font,first,{255,255,255,255},{255,255,255});
        Case("page1",pool,*view,frames,backend,font,red,{255,255,255,255},{255,0,0});
        Case("modulation",pool,*view,frames,backend,font,first,{128,64,192,255},{128,64,192});
        Case("alpha",pool,*view,frames,backend,font,first,{255,255,255,128},{138,140,143});
        Case("submission order",pool,*view,frames,backend,font,first,{255,255,255,255},{255,0,0},315,true);
        auto spaced=std::make_shared<resources::FrontendFont>(*font); spaced->spacing=1.5f;
        Case("fractional forward kerning",pool,*view,frames,backend,spaced,resources::LayoutFrontendText(spaced,u"AB"),{255,255,255,255},{20,24,30},293);
        auto pages=std::make_shared<resources::FrontendFont>(*font); pages->glyphs.at('A').page=1; pages->glyphs.at('B').offset=-10;
        Case("ascending page batches",pool,*view,frames,backend,pages,resources::LayoutFrontendText(pages,u"AB"),{255,255,255,255},{255,0,0});
        auto repeat=std::make_shared<resources::FrontendFont>(*font); auto& texture=repeat->pages[0]; texture.palette[4]=0xfc; texture.palette[5]=0;
        for (unsigned y=0;y<32;++y) for (unsigned x=0;x<32;++x)
            texture.pixels[(y/4*4+x/8)*32+(y%4)*8+x%8]=x<16?1:2;
        auto wrapping=resources::LayoutFrontendText(repeat,u"A"); wrapping.quads[0].u0=1.25f; wrapping.quads[0].u1=1.35f;
        Case("original repeat wrapping",pool,*view,frames,backend,repeat,wrapping,{255,255,255,255},{255,255,255});
        Check(draws>=100,"Font packets did not reach actual GX draws"); glFinish(); frames.Release(); views.Release(); materials.Release(); glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Font packet sessions leaked game arenas");
        ResetStartupFiles(); ResetStartupMemory(); aurora_shutdown(); session.live=false; Check(!errors,"GPU validation reported a font pipeline error");
        std::cout<<checks<<" original font packet checks passed, "<<draws<<" draws\n";
    }
    catch (const std::exception& e) { std::cerr<<"FAILED: "<<e.what()<<'\n'; return 1; }
}
