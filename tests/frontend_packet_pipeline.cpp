#include "frontend_packets_fixture.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMatrix.h"
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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
using namespace mscharged;
namespace
{
std::atomic_uint errors=0;unsigned checks=0,draws=0;bool fail_drain=false;
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
void Log(AuroraLogLevel level,const char*,const char* message,unsigned n)
{if(level>=LOG_ERROR)++errors;std::cerr.write(message,n);std::cerr<<'\n';}
void Drain(){if(fail_drain)throw std::runtime_error("Injected GPU drain failure");AuroraGXSync();}
void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session
{bool live=false;~Session(){if(live){fail_drain=false;Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}};
void Acquire(OriginalFrames& frames)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;)
    {
        for(const auto* event=aurora_update();event->type!=AURORA_NONE;++event)Check(event->type!=AURORA_EXIT,"Mixed window closed");
        if(frames.Acquire())return;
        Check(std::chrono::steady_clock::now()<end,"Mixed frame acquisition timed out");SDL_Delay(1);
    }
}
void Case(const char* name,FrontendPacketRenderer& renderer,GLView& view,OriginalFrames& frames,AuroraFrames& backend,
    FrontendSession::Handle frame,std::array<int,3> expected,bool prepare=true,bool centre_only=false)
{
    if(prepare)renderer.Prepare(frame);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);unsigned quiet=0;
    try
    {
        for(unsigned i=0;;++i)
        {
            Check(std::chrono::steady_clock::now()<end,"Mixed pipeline warmup timed out");
            Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
            renderer.Submit(view,frame);const bool sample=i>=12&&quiet>=2;backend.read_colours=sample;
            glEndFrame();glSendFrame();renderer.FinishFrame();draws+=aurora_get_stats()->drawCallCount;
            const auto pending=std::atomic_ref<const std::uint32_t>(aurora_get_stats()->queuedPipelines).load();quiet=pending?0:quiet+1;
            if(sample&&!pending)break;
        }
        const auto& c=backend.colours[4];std::cout<<name<<" RGB="<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2])<<'\n';
        for(unsigned i=0;i<3;++i)
        {
            Check(std::abs(int(c[i])-expected[i])<=3,"Mixed original packet overlap pixel differs");
            Check(std::abs(int(backend.colours[0][i])-std::array{20,24,30}[i])<=3,"Mixed packets changed background pixels");
            if(centre_only)for(unsigned pixel=0;pixel<9;++pixel)if(pixel!=4)
                Check(std::abs(int(backend.colours[pixel][i])-std::array{20,24,30}[i])<=3,
                    "Original font scissor allowed a pixel outside its rectangle");
        }
    }
    catch(...){frames.Cancel();renderer.FinishFrame();throw;}
}
}
int main(int argc,char** argv)
{
    try
    {
        const char* base=SDL_GetBasePath();Check(base,"Missing mixed pipeline directory");
        const std::string path=std::string(base)+"frontend-mixed-packet-test-data";std::filesystem::create_directories(path);
        AuroraConfig config{};config.appName="Charged mixed frontend packet checks";config.userPath=config.cachePath=path.c_str();config.resourcesPath=base;
        config.desiredBackend=BACKEND_VULKAN;config.enableBackendValidation=true;config.windowWidth=640;config.windowHeight=480;
        config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.logCallback=Log;config.vsync=true;
        config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Session session;const auto info=aurora_initialize(argc,argv,&config);session.live=true;Check(info.window&&info.backend==BACKEND_VULKAN,"Mixed pipeline requires Vulkan");
        ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);
        alignas(32)std::array<unsigned char,65536>fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms materials;OriginalViews views(640,480,Drain);ViewMatrices matrices;glMatrixOrthographic(matrices.projection,640,480);
        auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);
        AuroraFrames backend;OriginalFrames frames(backend);FrontendPacketRenderer renderer(Drain);
        auto first=frontend_packet_fixture::Frame();Case("image over text",renderer,*view,frames,backend,first,{0,0,255});
        auto clipped=std::make_shared<FrontendSessionFrame>(*first);clipped->layout.entries.resize(1);
        auto& text_clip=std::get<resources::FrontendLayoutText>(clipped->layout.entries[0]);
        text_clip.transform=frontend_packet_fixture::Transform();
        auto& wide=text_clip.layout.quads[0];wide.left=80;wide.right=560;wide.top=60;wide.bottom=420;
        text_clip.scissor=std::array<std::uint16_t,4>{300,220,40,40};
        Case("float-UV font clipped in screen space",renderer,*view,frames,backend,clipped,{255,255,255},true,true);
        auto excluded=std::make_shared<FrontendSessionFrame>(*clipped);
        std::get<resources::FrontendLayoutText>(excluded->layout.entries[0]).scissor=std::array<std::uint16_t,4>{5,5,30,30};
        Case("font outside scissor",renderer,*view,frames,backend,excluded,{20,24,30},true,true);
        auto restored=std::make_shared<FrontendSessionFrame>(*excluded);restored->layout.entries.push_back(first->layout.entries[1]);
        Case("following image restores full scissor",renderer,*view,frames,backend,restored,{0,0,255});
        auto text_restored=std::make_shared<FrontendSessionFrame>(*excluded);text_restored->layout.entries.push_back(first->layout.entries[0]);
        Case("following plain font restores full scissor",renderer,*view,frames,backend,text_restored,{255,255,255});
        auto reversed=std::make_shared<FrontendSessionFrame>(*first);std::reverse(reversed->layout.entries.begin(),reversed->layout.entries.end());
        Case("text over image",renderer,*view,frames,backend,reversed,{255,255,255});
        auto triple=std::make_shared<FrontendSessionFrame>(*first);auto text=std::get<resources::FrontendLayoutText>(triple->layout.entries[0]);
        text.layout=resources::LayoutFrontendText(text.layout.font,u" ");triple->layout.entries.push_back(text);
        Case("interleaved page image page",renderer,*view,frames,backend,triple,{255,0,0});
        auto replacement=frontend_packet_fixture::Frame(1);Case("same hash green replacement",renderer,*view,frames,backend,replacement,{0,255,0});
        fail_drain=true;bool rejected=false;try{renderer.Prepare(first);}catch(const std::exception&){rejected=true;}fail_drain=false;
        Check(rejected&&renderer.Current()==replacement,"Failed drain discarded visible mixed frame");
        Case("failed replacement retains green",renderer,*view,frames,backend,replacement,{0,255,0},false);
        // Independent GX blend expectations against an opaque white text quad.
        const std::array<std::array<int,3>,8> expected{{{0,0,255},{0,0,255},{255,255,255},{255,255,255},{0,0,255},{255,255,255},{0,0,255},{255,255,0}}};
        for(unsigned blend=0;blend<8;++blend)
        {
            auto input=std::make_shared<FrontendSessionFrame>(*first);std::get<resources::FrontendLayoutImage>(input->layout.entries[1]).blend=blend;
            const auto name="image blend "+std::to_string(blend);Case(name.c_str(),renderer,*view,frames,backend,input,expected[blend]);
        }
        Acquire(frames);glBeginFrame();renderer.Submit(*view,renderer.Current());frames.Cancel();renderer.FinishFrame();renderer.Release();
        Check(draws>=300,"Mixed original packets did not reach GX");glFinish();frames.Release();views.Release();materials.Release();glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Mixed packet teardown leaked arenas");
        ResetStartupFiles();ResetStartupMemory();aurora_shutdown();session.live=false;Check(!errors,"Mixed GPU validation reported an error");
        std::cout<<checks<<" mixed packet pixel checks passed, "<<draws<<" draws\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
