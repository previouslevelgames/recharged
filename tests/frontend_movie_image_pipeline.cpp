#include "platform/graphics_stats.h"
#include "runtime/frontend_packets.h"
#include "resources/frontend_animation.h"
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
#include "NL/gl/glState.h"
#include <cstring>
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

std::shared_ptr<FrontendSession> Load(const char* slide,bool owned)
{
    auto session=std::make_shared<FrontendSession>();session->Begin({owned?"/Art/fe/credits.fen":"/Art/fe/credits-pending.fen",FrontendLanguage::English,FrontendImageProfile::Main,slide,true},owned?FrontendSessionResourcesMode::Scene:FrontendSessionResourcesMode::PermanentMain);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(session->State()==FrontendSessionState::Loading){session->Service();Check(std::chrono::steady_clock::now()<end,"Movie FEN async load timeout");SDL_Delay(1);}session->Result();return session;
}
unsigned Target(const FrontendSession::Handle& frame)
{
    const std::array<std::string_view,2> names{"Layer","movie"};const auto node=resources::FindFrontendNode(frame->graph,{},resources::FrontendNamedPath(names),resources::FrontendNodeType::Image);
    Check(bool(node),"Source Layer/movie lookup absent");return node->id;
}
FrontendSession::Handle Swap(const FrontendSession::Handle& frame,const FrontendMovieImageBinding::Handle& binding)
{
    auto next=std::make_shared<FrontendSessionFrame>(*frame);ApplyFrontendMovieBinding(next->graph,binding);
    next->layout=resources::BuildFrontendLayout(next->graph,*next->visuals->localization,std::array{next->visuals->text,next->visuals->heading},next->graph.active_slide,*next->images,{true,true});return next;
}
void Case(FrontendPacketRenderer& renderer,GLView& view,OriginalFrames& frames,AuroraFrames& backend,
    FrontendSession::Handle input,const std::array<int,3>* expected,bool append_empty=false)
{
    renderer.Prepare(input);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);unsigned quiet=0;
    try
    {
        for(unsigned n=0;;++n)
        {
            Check(std::chrono::steady_clock::now()<end,"Authored movie pipeline warmup timeout");Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
            glStateBundle before,after;glStateSave(before);const auto raster=glHandleizeRasterState();const auto texture=glHandleizeTextureState();
            renderer.Submit(view,input);
            if(append_empty)
            {
                auto following=std::make_shared<FrontendSessionFrame>(*input);following->layout.entries.clear();
                renderer.Submit(view,following);const auto observation=renderer.MovieStatus();
                Check(observation.current.Pending()==0&&observation.frame.Pending()==1&&observation.submitted_packets==0,
                    "Following empty layout erased this frame's pending movie observation");
            }
            glStateSave(after);
            Check(!std::memcmp(&before,&after,sizeof before)&&raster==glHandleizeRasterState()&&texture==glHandleizeTextureState(),"Movie image changed caller graphics state");
            const bool sample=n>=12&&quiet>=2;backend.read_colours=sample;
            glEndFrame();glSendFrame();renderer.FinishFrame();draws+=aurora_get_stats()->drawCallCount;
            const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;if(sample&&!pending)break;
        }
        const auto& c=backend.colours[4];
        std::cout<<"actual movie centre RGB="<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2])<<'\n';
        if(expected)
        {
            std::cout<<"mixed movie RGB="<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2])<<'\n';
            for(unsigned i=0;i<3;++i)Check(std::abs(int(c[i])-(*expected)[i])<=3,"Authored mixed movie order pixel differs");
        }
    }
    catch(...){frames.Cancel();renderer.FinishFrame();throw;}
}
void Run(const char* disc,bool owned,GLView& view,OriginalFrames& frames,AuroraFrames& backend)
{
    Check(aurora_dvd_open(disc),"Movie data mount failed");nlInitFileSystem();
    struct DiscCleanup{~DiscCleanup(){ResetStartupFiles();aurora_dvd_close();}}disc_cleanup;
    auto session=Load(owned?"NLG 4:3":"NLG",owned);if(owned)session->Advance(0);FrontendPacketRenderer renderer(Drain);renderer.Prepare(session->Current());
    const std::array background{20,24,30};
    if(!owned)
    {
        auto pending=std::make_shared<FrontendSessionFrame>(*session->Current());
        Check(pending->layout.MovieCount()==1&&renderer.MovieStatus().current.awaiting_binding==1,"Unbound movie is not explicitly pending");
        resources::FrontendLayoutText text;text.layout=resources::LayoutFrontendText(pending->visuals->text,u"A");
        text.transform={1,0,0,0,0,1,0,0,0,0,1,0,315,237,0,1};text.colour={255,255,255,255};pending->layout.entries.push_back(text);
        const std::array white{255,255,255};Case(renderer,view,frames,backend,pending,&white,true);
        resources::FrontendLayoutImage image;image.texture=pending->images->textures.begin()->second;
        image.transform={1,0,0,0,0,1,0,0,0,0,1,0,320,240,0,1};image.colour={255,255,255,255};
        image.vertices={{{-20,20,0,0},{-20,-20,0,1},{20,-20,1,1},{20,20,1,0}}};pending->layout.entries.push_back(image);
        const std::array blue{0,0,255};Case(renderer,view,frames,backend,pending,&blue,true);
        Check(renderer.MovieStatus().frame.awaiting_binding==1&&renderer.MovieStatus().submitted_packets==0,"Unbound entry queued a movie packet");
    }
    auto movie=std::make_shared<FrontendMoviePlayback>(FrontendMovieRequest{1,owned?"/Art/movies/nlgintrowide.thp":"/Art/movies/test.thp"},FrontendMovieOptions{},0);
    auto binding=renderer.BindMovie(session,Target(session->Current()),movie);auto frame=Swap(session->Current(),binding);
    Case(renderer,view,frames,backend,frame,&background,true);
    Check(renderer.MovieStatus().frame.awaiting_frame==1&&renderer.MovieStatus().submitted_packets==0
        &&!movie->Status().final_presented&&!movie->Completion(),"Registration before decode minted a movie presentation");
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!movie->Current()){movie->Advance(0);Check(std::chrono::steady_clock::now()<end,"First movie frame decode timed out");SDL_Delay(1);}
    if(owned)
    {
        const auto sample_end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(movie->Status().published_frames<91){movie->Advance(movie->Status().published_frames*2);Check(std::chrono::steady_clock::now()<sample_end,"Owned representative movie frame timeout");SDL_Delay(1);}
        Check(movie->Current()->index==90&&!movie->Completion(),"Owned representative decode fabricated completion");
    }
    const std::array gray{128,127,127},white{255,255,255},blue{0,0,255};Case(renderer,view,frames,backend,frame,owned?nullptr:&gray);
    if(!owned)
    {
        // Independently supplied overlay entries retain real source font/image
        // owners. Their order must stay interleaved with the authored movie.
        auto mixed=std::make_shared<FrontendSessionFrame>(*frame);
        resources::FrontendLayoutText text;text.layout=resources::LayoutFrontendText(frame->visuals->text,u"A");
        text.transform={1,0,0,0,0,1,0,0,0,0,1,0,315,237,0,1};text.colour={255,255,255,255};mixed->layout.entries.push_back(text);
        Case(renderer,view,frames,backend,mixed,&white);
        resources::FrontendLayoutImage image;image.texture=frame->images->textures.begin()->second;
        image.transform={1,0,0,0,0,1,0,0,0,0,1,0,320,240,0,1};image.colour={255,255,255,255};
        image.vertices={{{-20,20,0,0},{-20,-20,0,1},{20,-20,1,1},{20,20,1,0}}};mixed->layout.entries.push_back(image);
        Case(renderer,view,frames,backend,mixed,&blue);std::rotate(mixed->layout.entries.begin(),mixed->layout.entries.begin()+1,mixed->layout.entries.end());
        Case(renderer,view,frames,backend,mixed,&gray);
    }
    // Actual NLG->Credits new registration uses the same source resource name;
    // old retained snapshots lose readiness, and the next provider stays exact.
    Acquire(frames);glBeginFrame();renderer.Submit(view,frame);frames.Cancel();renderer.FinishFrame();movie->Cancel();renderer.RetireMovie();
    Check(!binding->Active(),"Drained movie binding stayed active");renderer.Prepare(frame);Check(renderer.MovieStatus().current.cancelled==1,"Cancelled epoch was not explicitly pending");
    resources::FrontendAnimationPlayback stopped(frame->graph);Check(stopped.SelectPresentation(owned?"Credits 4:3":"CREDITS",true),"Next source presentation absent");
    auto pending_next=std::make_shared<FrontendSessionFrame>(*frame);
    if(owned)stopped.Advance(0); // Actual next BaseUpdate evaluates the authored 4:3 slide.
    pending_next->graph=stopped.Scene();
    pending_next->layout=resources::BuildFrontendLayout(pending_next->graph,*pending_next->visuals->localization,std::array{pending_next->visuals->text,pending_next->visuals->heading},pending_next->graph.active_slide,*pending_next->images,{true,true});
    Case(renderer,view,frames,backend,pending_next,owned?nullptr:&white,true);
    Check(renderer.MovieStatus().frame.cancelled==1&&renderer.MovieStatus().submitted_packets==0&&!movie->Completion(),"Cancelled source transition queued a movie or completion");
    session->SelectPresentation(owned?"Credits 4:3":"CREDITS",true);if(owned)session->Advance(0);
    auto next=std::make_shared<FrontendMoviePlayback>(FrontendMovieRequest{2,owned?"/Art/movies/credits.thp":"/Art/movies/test.thp"},FrontendMovieOptions{},0);
    auto rebound=renderer.BindMovie(session,Target(session->Current()),next);
    Case(renderer,view,frames,backend,pending_next,owned?nullptr:&white,true);
    Check(!next->Status().final_presented&&!next->Completion(),"Obsolete metadata credited the replacement provider");
    auto second=Swap(session->Current(),rebound);
    const auto second_end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!next->Current()){next->Advance(0);Check(std::chrono::steady_clock::now()<second_end,"Next movie frame decode timed out");SDL_Delay(1);}
    Check(rebound->Resource()==binding->Resource()&&rebound->Instance()!=binding->Instance(),"Source Credits resource/instance replacement differs");
    Case(renderer,view,frames,backend,second,owned?nullptr:&gray);next->Cancel();renderer.RetireMovie();renderer.Release();
    session->Pop();session.reset();ResetStartupFiles();aurora_dvd_close();
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc!=3)throw std::invalid_argument("Expected disc and generated|owned");
        const char* base=SDL_GetBasePath();Check(base,"Missing executable directory");const std::string path=std::string(base)+"frontend-movie-image-data";std::filesystem::create_directories(path);
        AuroraConfig config{};config.appName="Charged authored movie image checks";config.userPath=config.cachePath=path.c_str();config.resourcesPath=base;config.desiredBackend=BACKEND_VULKAN;config.enableBackendValidation=true;config.windowWidth=640;config.windowHeight=480;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.logCallback=Log;config.vsync=true;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Runtime runtime;const auto info=aurora_initialize(argc,argv,&config);runtime.live=true;Check(info.window&&info.backend==BACKEND_VULKAN,"Movie image requires Vulkan");
        ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);
        alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms materials;OriginalViews views(640,480,Drain);ViewMatrices matrices;glMatrixOrthographic(matrices.projection,640,480);
        auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);AuroraFrames backend;OriginalFrames frames(backend);
        Run(argv[1],std::string_view(argv[2])=="owned",*view,frames,backend);
        glFinish();frames.Release();views.Release();materials.Release();glShutdownMemory();Check(a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Movie image GPU teardown leaked original arenas");ResetStartupFiles();ResetStartupMemory();aurora_shutdown();runtime.live=false;
        Check(!errors,"Movie image GPU validation failed");std::cout<<checks<<" authored movie image checks passed, "<<draws<<" GX draws\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
