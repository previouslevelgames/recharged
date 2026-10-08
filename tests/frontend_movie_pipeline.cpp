#include "platform/graphics_stats.h"
#include "runtime/frontend_movie_render.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/nlFileGC.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
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
std::atomic_uint errors=0;unsigned checks=0,draws=0;
void Check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
template<class F>void Reject(F f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid movie GPU operation accepted");}
void Log(AuroraLogLevel level,const char*,const char* message,unsigned n){if(level>=LOG_ERROR)++errors;std::cerr.write(message,n);std::cerr<<'\n';}
void Drain(){AuroraGXSync();}void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Runtime{bool live=false,disc=false;~Runtime(){if(live){Drain();glShutdownMemory();ResetStartupFiles();if(disc)aurora_dvd_close();ResetStartupMemory();aurora_shutdown();}}};
void Acquire(OriginalFrames& frames)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;){for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)Check(e->type!=AURORA_EXIT,"Movie GPU window closed");
        if(frames.Acquire())return;Check(std::chrono::steady_clock::now()<end,"Movie frame acquisition timeout");SDL_Delay(1);}
}
resources::ThpMovieFrameHandle Plane(unsigned y,unsigned u,unsigned v)
{
    auto p=std::make_shared<resources::ThpMovieFrame>();p->width=p->height=16;
    p->y.assign(256,y);p->u.assign(64,u);p->v.assign(64,v);return p;
}
void Case(GLResourcePool& pool,GLView& view,OriginalFrames& frames,AuroraFrames& backend,unsigned y,unsigned u,unsigned v)
{
    FrontendMovieRenderer renderer(pool,16,16,Drain);auto plane=Plane(y,u,v);
    Reject([&]{FrontendMovieRenderer collision(pool,16,16,Drain);});Reject([&]{renderer.Submit(view,plane);});
    FrontendMovieQuad quad;quad.positions={{{120,80},{520,80},{520,400},{120,400}}};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);unsigned quiet=0;
    try
    {
        for(unsigned n=0;;++n)
        {
            Check(std::chrono::steady_clock::now()<deadline,"Movie shader warmup timeout");Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
            renderer.Submit(view,plane,quad);Reject([&]{renderer.Submit(view,plane,quad);});Reject([&]{renderer.Release();});
            const bool sample=n>=12&&quiet>=2;backend.read_colours=sample;glEndFrame();glSendFrame();
            auto receipt=renderer.FinishFrame();draws+=aurora_get_stats()->drawCallCount;
            if(sample)Check(bool(receipt),"Warm movie packet presentation did not issue encoded receipt");
            const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;
            if(sample&&!pending)break;
        }
        // Independent algebraic YUV conversion oracle from original TEV
        // constants, permitting quantization/filter rounding within3bytes.
        const std::array expected{std::clamp(double(y)-180+2.0*v*179/255,0.0,255.0),
            std::clamp(double(y)+135-double(u)*88/255-double(v)*182/255,0.0,255.0),
            std::clamp(double(y)-228+2.0*u*226/255,0.0,255.0)};
        const auto& c=backend.colours[4];std::cout<<"draws="<<draws<<" YUV "<<y<<','<<u<<','<<v<<" -> "<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2])<<'\n';
        for(unsigned i=0;i<3;++i)Check(std::abs(double(c[i])-expected[i])<=3,"Original movie TEV pixel oracle differs");
        Acquire(frames);glBeginFrame();renderer.Submit(view,plane,quad);frames.Cancel();Check(!renderer.FinishFrame(),"Cancelled movie frame issued presentation receipt");
        renderer.Release();
    }
    catch(...){frames.Cancel();renderer.FinishFrame();throw;}
}
void Playback(const char* path,GLResourcePool& pool,GLView& view,OriginalFrames& frames,AuroraFrames& backend,bool full)
{
    FrontendMoviePlayback movie({77,path},{false,100,0},0);FrontendMovieRenderer renderer(pool,movie.Info().width,movie.Info().height,Drain);
    const unsigned limit=full?movie.Info().frame_count:12;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(full?35:10);
    // Explicit test retrace samples qualify source cadence; the production
    // host VI-rate adapter is separate. Real SDL backpressure is still active.
    try
    {
        for(unsigned n=0;;++n)
        {
            Check(std::chrono::steady_clock::now()<deadline,"Movie playback GPU timeout");
            const auto count=movie.Status().published_frames;movie.Advance(count*2);
            if(auto frame=movie.Current())
            {
                Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
                renderer.Submit(view,frame);backend.read_colours=false;glEndFrame();glSendFrame();auto receipt=renderer.FinishFrame();if(receipt)movie.Acknowledge(receipt);draws+=aurora_get_stats()->drawCallCount;
            }
            SDL_Delay(1);
            if(full&&movie.Completion())
            {
                Check(movie.Completion()->Generation()==77&&movie.Completion()->Path()==path&&movie.Completion()->FinalFrame()+1==movie.Info().frame_count&&movie.Completion()->AudioFrames()==movie.Info().total_audio_samples,"Opaque movie completion identity differs");break;
            }
            if(!full&&movie.Status().published_frames>=limit){Check(!movie.Completion(),"Partial owned movie fabricated completion");break;}
        }
        std::cout<<path<<" presented output frames="<<movie.Status().published_frames<<" audio="<<movie.Status().submitted_audio_frames<<" complete="<<bool(movie.Completion())<<'\n';
        renderer.Release();movie.Cancel();
    }
    catch(...){frames.Cancel();renderer.FinishFrame();throw;}
}
}
int main(int argc,char** argv)
{
    try
    {
        const char* base=SDL_GetBasePath();Check(base,"Missing binary directory");const std::string path=std::string(base)+"frontend-movie-pipeline-data";std::filesystem::create_directories(path);
        AuroraConfig config{};config.appName="Charged original movie YUV checks";config.userPath=config.cachePath=path.c_str();config.resourcesPath=base;config.desiredBackend=BACKEND_VULKAN;config.enableBackendValidation=true;config.windowWidth=640;config.windowHeight=480;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.logCallback=Log;config.vsync=true;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Runtime runtime;const auto info=aurora_initialize(argc,argv,&config);runtime.live=true;Check(info.window&&info.backend==BACKEND_VULKAN,"Movie pipeline requires Vulkan");
        ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);
        alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms materials;OriginalViews views(640,480,Drain);ViewMatrices matrices;glMatrixOrthographic(matrices.projection,640,480);
        auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);AuroraFrames backend;OriginalFrames frames(backend);auto& pool=*glGetCurrentResourcePool();
        // An empty tagged span must not become a receipt merely because the
        // application sends and presents a frame. The tag is producer-scoped.
        Acquire(frames);glBeginFrame();
        const auto empty_tag=AuroraGXBeginDrawReceipt();Check(empty_tag!=0,"Empty span tag allocation failed");
        Check(AuroraGXBeginDrawReceipt()==0,"Nested receipt span was admitted");AuroraGXEndDrawReceipt();
        glEndFrame();glSendFrame();Drain();
        Check(!AuroraGXWasDrawEncoded(empty_tag)&&!AuroraGXWasDrawEncoded(0),"Non-drawn span received encoded evidence");
        for(auto yuv:{std::array{128u,128u,128u},std::array{90u,80u,200u},std::array{180u,200u,70u}})Case(pool,*view,frames,backend,yuv[0],yuv[1],yuv[2]);
        Check(!AuroraGXWasDrawEncoded(empty_tag),"Foreign later movie draw reused an empty span tag");
        if(argc==3)
        {
            Check(aurora_dvd_open(argv[1]),"Movie pipeline disc failed");runtime.disc=true;nlInitFileSystem();
            const bool owned=std::string_view(argv[2])=="owned";
            Playback(owned?"/Art/movies/nlgintrowide.thp":"/Art/movies/test.thp",pool,*view,frames,backend,true);
            if(owned)Playback("/Art/movies/credits.thp",pool,*view,frames,backend,false);
            ResetStartupFiles();aurora_dvd_close();runtime.disc=false;
        }
        glFinish();frames.Release();views.Release();materials.Release();glShutdownMemory();Check(a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Movie renderer leaked original arenas");ResetStartupFiles();ResetStartupMemory();aurora_shutdown();runtime.live=false;
        Check(!errors,"Movie GPU validation failed");std::cout<<checks<<" movie pipeline checks passed, "<<draws<<" GX draws\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
