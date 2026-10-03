#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/static_inventory.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxMemory.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace mscharged;
namespace
{
std::atomic_uint errors=0;
void Log(AuroraLogLevel level,const char*,const char* message,unsigned size)
{
    if(level>=LOG_ERROR) ++errors;
    std::cerr.write(message,size); std::cerr << '\n';
}
void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void Drain() { AuroraGXSync(); }
void Invalidate() { GXInvalidateVtxCache(); GXInvalidateTexAll(); }
struct Session
{
    bool live=false;
    ~Session() { if(live) { Drain(); glShutdownMemory(); ResetStartupFiles(); ResetStartupMemory(); aurora_shutdown(); } }
};
resources::StaticModel Model()
{
    resources::Packet p;
    p.primitive=0; p.material.program=0x21db4385; p.material.textures[0]={2,3}; p.raster=0xC0007;
    p.vertices={{{-.8f,-.8f,-.3f},{0,0}},{{.8f,-.8f,-.3f},{1,0}},{{0,.8f,-.3f},{.5f,1}}};
    p.indices={0,1,2}; return {1,{p}};
}
resources::Texture Texture()
{
    resources::Texture t;
    t.id=2; t.width=t.height=4; t.levels=1; t.game_format=3; t.gx_format=6; t.bits={8,8,8,0}; t.pixels.resize(64);
    for(unsigned i=0;i<16;++i) { t.pixels[i*2]=255; t.pixels[i*2+1]=80; t.pixels[i*2+32]=100; t.pixels[i*2+33]=120; }
    return t;
}
void Acquire(OriginalFrames& frames)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;)
    {
        for(const auto* event=aurora_update();event->type!=AURORA_NONE;++event)
            Check(event->type!=AURORA_EXIT,"Frame test window closed");
        if(frames.Acquire()) return;
        Check(std::chrono::steady_clock::now()<deadline,"Frame acquisition timed out"); SDL_Delay(1);
    }
}
void Pixel(const ColourSamples& samples)
{
    const auto& c=samples[4];
    std::cout << "Frame centre: " << unsigned(c[0]) << ',' << unsigned(c[1]) << ',' << unsigned(c[2]) << '\n';
    Check(std::abs(int(c[0])-80)<=3 && std::abs(int(c[1])-100)<=3 && std::abs(int(c[2])-120)<=3,
          "Original frame lifecycle changed the rendered pixels");
}
}
int main(int argc,char** argv)
{
    try
    {
        const char* base=SDL_GetBasePath(); Check(base,"Missing executable directory");
        const std::string path=std::string(base)+"frame-test-data"; std::filesystem::create_directories(path);
        AuroraConfig cfg{}; cfg.appName="Charged original graphics frame check";
        cfg.userPath=cfg.cachePath=path.c_str(); cfg.resourcesPath=base;
        cfg.desiredBackend=BACKEND_VULKAN; cfg.enableBackendValidation=true;
        cfg.windowWidth=640; cfg.windowHeight=480; cfg.windowPosX=cfg.windowPosY=-1;
        cfg.logLevel=LOG_WARNING; cfg.logCallback=Log; cfg.vsync=true;
        cfg.mem1Size=MEM1_DEFAULT_SIZE; cfg.mem2Size=64*1024*1024;
        Session session;
        const auto info=aurora_initialize(argc,argv,&cfg); session.live=true;
        Check(info.window && info.backend==BACKEND_VULKAN,"Frame check requires Vulkan");
        ImGui::GetIO().IniFilename=nullptr; ImGui::GetIO().LogFilename=nullptr;
        InitializeStartupOS(); nlInitMemory();
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools(); InitializeOriginalGraphicsMemory(); InitializeOriginalGraphicsState();
        VIInit(); VIConfigure(&GXNtsc480IntDf);
        alignas(32) std::array<unsigned char,65536> fifo{}; GXInit(fifo.data(),fifo.size());
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT); SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms materials;
        StaticInventory inventory(*glGetCurrentResourcePool(),{Model()},{Texture()},Drain);
        OriginalViews views(640,480,Drain); ViewMatrices matrices;
        glMatrixOrthographicCentered(matrices.projection,2,2,0,1);
        auto* view=new (8,false) GLView(&matrices,{},GLViewSort_None); gRootView.AddChild(view);
        AuroraFrames backend; OriginalFrames frames(backend);
        auto draw=[&] {
            glBeginFrame();
            GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR); GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
            nlMatrix4 identity; identity.SetIdentity(); auto* model=inventory.Model(1);
            glModelSetMatrix(model,identity); view->AttachModel(model,0); glEndFrame();
        };
        unsigned sent=0;
        for(unsigned i=0;i<40;++i)
        {
            Acquire(frames); draw(); backend.read_colours=i==39; glSendFrame(); ++sent;
        }
        Pixel(backend.colours);
        glDiscardFrame(12); backend.read_colours=false;
        for(unsigned i=0;i<12;++i)
        {
            Acquire(frames); draw(); glSendFrame(); ++sent;
            Check(aurora_get_stats()->drawCallCount==0,"Discard submitted game draws");
        }
        Acquire(frames); draw(); backend.read_colours=true; glSendFrame(); ++sent; Pixel(backend.colours);
        // Fail after the view has already emitted geometry. Recovery must close
        // the host frame without presenting it and permit a clean next frame.
        Acquire(frames); draw(); view->m_Target=1234;
        const auto generation=glNativeFrameGeneration(); bool rejected=false;
        try { glSendFrame(); } catch(const std::invalid_argument&) { rejected=true; }
        view->m_Target=GLViewTarget_None;
        Check(rejected && glGetCurrentFrame()==int(sent) && !glIsFrameActive()
            && glNativeFrameGeneration()==generation+1,"Failed GPU frame did not recover its original state");
        Acquire(frames); draw(); glSendFrame(); ++sent; Pixel(backend.colours);
        Acquire(frames); glBeginFrame(); frames.Cancel();
        Acquire(frames); draw(); glSendFrame(); ++sent; Pixel(backend.colours);
        Check(glGetCurrentFrame()==int(sent),"Original frame count includes failed/cancelled frames");
        glFinish(); frames.Release(); views.Release(); inventory.Release(); materials.Release(); glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory()==free1 && VirtualAllocator.TotalFreeMemory()==free2,"Frame pipeline leaked game memory");
        ResetStartupFiles(); ResetStartupMemory(); aurora_shutdown(); session.live=false;
        Check(!errors,"Graphics backend reported an error");
        std::cout << "Original GPU frames: pixels, twelve discards, partial-render failure recovery, cancellation and arena recovery passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
