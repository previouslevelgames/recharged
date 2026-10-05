#include "runtime/frontend_menu_transition.h"
#include "runtime/frontend_camera_assets.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid menu operation accepted at "+std::to_string(at.line()));}
using Blob=std::vector<std::uint8_t>;
Blob Load(const char* filename)
{
    unsigned long size=0;void* raw=nlLoadEntireFile(filename,&size,32,AllocateStart,0,0,0);
    Check(raw&&size,"Actual NL menu bytecode read failed");
    struct Free{void* p;~Free(){nlFree(p);}}free{raw};
    return Blob(static_cast<std::uint8_t*>(raw),static_cast<std::uint8_t*>(raw)+size);
}
void Pump(FrontendSceneStack& stack,FrontendSceneStack::Token token)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(stack.Entry(token).state==FrontendStackState::Queued||stack.Entry(token).state==FrontendStackState::Loading)
    {stack.Service();Check(std::chrono::steady_clock::now()<end,"Scene read timeout");SDL_Delay(1);}
}
void Catalog(CameraAssetLibrary& library)
{
    auto batch=LoadFrontendCameraAssets(library);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(batch.State()==CameraBatchState::Loading)
    {batch.Service();Check(std::chrono::steady_clock::now()<end,"Camera read timeout");SDL_Delay(1);}
    Check(batch.State()==CameraBatchState::Ready&&batch.Progress().succeeded==37,"Original camera catalog incomplete");batch.Publish();
}
// Explicit fixture inspection callbacks, not concrete Main/Options handlers.
FrontendStackCallbacks Inspection()
{
    return {[](auto& c){Check(c.session.Current()==c.handler.Current(),"Creation preceded SetPresentation");},
        [](auto& c){Check(c.movement<=2,"Original ScreenMovement lost");},
        [](auto& c,float){Check(c.session.Current()==c.handler.Current(),"Base update context differs");}};
}
FrontendSceneStack::Token Scene(FrontendSceneStack& stack,unsigned scene,bool permanent=false)
{
    FrontendStackRequest request;request.scene=scene;request.initial_slide=scene==1?"MAIN":"in";
    if(permanent)request.resources_mode=FrontendSessionResourcesMode::PermanentMain;
    const auto token=stack.QueuePush(request,Inspection());Pump(stack,token);
    Check(stack.Entry(token).state==FrontendStackState::AwaitingPublication,"Inspection scene callbacks failed");
    stack.Publish(token,stack.Entry(token).prepared);return token;
}
float Duration(CameraAssetLibrary& library,const char* name)
{
    const auto asset=library.Find(name);Check(bool(asset),"Expected authored camera absent");
    const float duration=float(asset->Data().m_uKeyCount)/30.f;
    Check(duration>0&&duration<=60,"Authored camera duration outside test bound");return duration;
}
bool Pending(const FrontendMenuTransition& flow,FrontendMenuTransitionService service)
{const auto s=flow.Status();return s.state==FrontendMenuTransitionState::AwaitingService&&s.pending==service&&!s.queued_scene;}
unsigned Count(const FrontendMenuTransition& flow,unsigned id)
{return unsigned(std::count(flow.Calls().begin(),flow.Calls().end(),id));}
struct FixtureServices
{
    // Test-only authoritative values and admission spies. Production receives
    // real services explicitly; no such defaults are installed by the owner.
    std::optional<bool> save;
    bool effect=false,navigation=false,music=false;
    unsigned effects=0,navigations=0,musics=0;
    FrontendMenuTransitionServices Bind()
    {
        return {[this]{return save;},[this](unsigned type){Check(type==95,"Original effect argument differs");if(effect)++effects;return effect;},
            [this](std::string_view name){Check(name=="TransitionMainMenuToOptions","Original NAV destination differs");if(navigation)++navigations;return navigation;},
            [this]{if(music)++musics;return music;}};
    }
};
void RoundTrip(const Blob& script,CameraAssetLibrary& library,bool owned)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;unsigned drains=0;FrontendSceneStack stack(input,[&]{++drains;});
    const auto main=Scene(stack,1,true);auto frame=stack.Entry(main).published;
    const auto resources=stack.Resources(main);const auto visuals=frame->visuals;const auto images=frame->images;
    Check(bool(resources),"Permanent Main resource token was not retained");
    FrontendMenuTransition flow(script,cameras,stack);FixtureServices service;
    Reject([&]{flow.NavigationTransition("TransitionMainMenuToOptions");});
    auto wrong=std::make_shared<FrontendSessionFrame>(*frame);
    Reject([&]{flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,wrong);});
    Reject([&]{flow.Begin(static_cast<FrontendMenuTransitionKind>(2),main,frame);});
    FrontendStackCallbacks partial;partial.scene_created=[](auto&){};
    Reject([&]{flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,frame,{},partial);});
    Check(flow.Status().state==FrontendMenuTransitionState::Idle,"Rejected arguments mutated idle state");
    const auto base=cameras.Selection();stack.QueuePop(main);
    flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,frame,service.Bind());
    Check(Pending(flow,FrontendMenuTransitionService::SaveGate)&&cameras.Selection()==base,"Absent save authority changed camera/scene");
    flow.Update(0);service.save=true;flow.Update(0);
    Check(Pending(flow,FrontendMenuTransitionService::SaveGate)&&!Count(flow,5),"Busy save gate was bypassed");
    service.save=false;flow.Update(0);
    Check(Pending(flow,FrontendMenuTransitionService::StadiumEffect)&&cameras.Selection()==base,"Missing effect provider was bypassed");
    service.effect=true;flow.Update(0);
    Check(flow.Status().state==FrontendMenuTransitionState::Running&&cameras.ActiveAlias()=="startmainmenuball"
        &&service.effects==1&&cameras.Size()==2,"Original departure camera/effect order differs");
    const auto selected=flow.Status().camera;
    Check(cameras.IsCurrent(selected)&&cameras.CurrentSelection()==selected,"Blend lost retained top identity");
    Reject([&]{cameras.Selection();});
    stack.Poll();Reject([&]{stack.Entry(main);});frame.reset();wrong.reset();
    const float duration=Duration(library,"startmainmenuball"),dt=1.f/60;
    float animation=0,blend=0;unsigned frames=0;
    do
    {
        // Independent source float oracle: animation clamps at >=1; CameraMan
        // completes its normalized blend strictly after1, not at1.
        animation=std::min(1.f,animation+(dt*1.f)/duration);
        if(blend<=1)blend=blend+dt*(1.f/.25f);
        const bool completed=animation>=1&&blend>1;
        cameras.Advance(dt,dt);flow.Update(dt);++frames;
        Check(Pending(flow,FrontendMenuTransitionService::Navigation)==completed,"Departure crossed a different authored completion frame");
        Check(frames<4000,"Departure exceeded its bounded camera duration");
    }while(!Pending(flow,FrontendMenuTransitionService::Navigation));
    Check(cameras.Selection()==base&&cameras.Size()==1&&service.effects==1&&Count(flow,23)==1&&Count(flow,21)==1,
        "Departure retried an admitted host or failed to pop the camera");
    flow.Update(0);Check(!service.navigations&&stack.Entries().empty(),"Missing NAV manufactured a scene");
    service.navigation=true;flow.Update(0);
    Check(flow.Status().state==FrontendMenuTransitionState::AwaitingNavigation&&service.navigations==1,"NAV admission did not await its pending callback");
    flow.Update(1);Check(!flow.Status().queued_scene,"Elapsed time manufactured NAV completion");
    Reject([&]{flow.NavigationTransition("TransitionMainMenuToDomination");});
    flow.NavigationTransition("TransitionMainMenuToOptions");
    const auto result=flow.Status();Check(result.state==FrontendMenuTransitionState::SceneQueued&&result.queued_scene
        &&cameras.ActiveAlias()=="fechoosecaptains"&&cameras.Size()==2,"Original Options arrival camera/push differs");
    const auto options=*result.queued_scene;
    Check(stack.Entry(options).scene==13&&stack.Entry(options).movement==1,"Options scene/movement arguments differ");
    Pump(stack,options);Check(stack.Entry(options).state==FrontendStackState::AwaitingHandler&&stack.RenderPlan().empty(),
        "Concrete Options readiness was fabricated");
    Check(stack.Entry(options).prepared->request.initial_slide=="in","Options authored intro slide differs");
    Check(stack.Resources(options)==resources&&stack.Entry(options).prepared->visuals==visuals
        &&stack.Entry(options).prepared->images==images,"Options destination replaced permanent resources after source Pop");
    Check(stack.Entry(options).prepared->image_completed_files==0,
        "Shared Options destination reread font or image bundles");
    if(owned)Check(stack.Entry(options).prepared->layout.ImageCount()>0,"Owned Options images missing");
    stack.Bind(options,Inspection());stack.Poll();stack.Publish(options,stack.Entry(options).prepared);
    auto options_frame=stack.Entry(options).published;const auto before_return=cameras.Selection();
    stack.QueuePop(options);flow.Begin(FrontendMenuTransitionKind::OptionsToMain,options,options_frame,service.Bind());
    Check(Pending(flow,FrontendMenuTransitionService::MusicNavigation)&&cameras.Selection()==before_return,"Absent music/NAV authority changed camera");
    service.music=true;flow.Update(0);
    Check(service.musics==1&&cameras.ActiveAlias()=="outofballcam"&&cameras.Size()==2,"Return replace/music admission differs");
    const auto returning=flow.Status().camera;const auto return_duration=Duration(library,"outofballcam");
    cameras.Advance(return_duration,return_duration);flow.Update(return_duration);
    const float pop_start=1.f-blend;
    Check(std::bit_cast<unsigned>(cCameraManager::m_fTransitionTime)==std::bit_cast<unsigned>(pop_start),
        "Original pop lost its carried normalized transition time");
    Check(cameras.Size()==1&&cameras.CurrentSelection()==base&&cameras.IsCurrent(flow.Status().camera)
        &&!flow.Status().queued_scene&&Count(flow,21)==1,"Return did not retain its popped-to blend identity");
    Check(!cameras.DetachEndCallback(returning),"Popped animation callback remained live");
    stack.Poll();Reject([&]{stack.Entry(options);});options_frame.reset();
    const float to_endpoint=1.f-pop_start;
    Check(pop_start+to_endpoint==1.f,"Independent normalized blend equality is not exact");
    cameras.Advance(to_endpoint,to_endpoint);flow.Update(to_endpoint);
    Check(!flow.Status().queued_scene,"Original return blend completed at equality");
    cameras.Advance(.001f,.001f);flow.Update(.001f);
    const auto back=flow.Status();Check(back.state==FrontendMenuTransitionState::SceneQueued&&back.queued_scene,
        "Original return blend did not queue Main after strict endpoint");
    Check(stack.Entry(*back.queued_scene).scene==1&&stack.Entry(*back.queued_scene).movement==2
        &&Count(flow,42)==2&&service.musics==1&&Count(flow,23)==1&&Count(flow,25)==1,"Return host count/scene/movement differs");
    Pump(stack,*back.queued_scene);Check(stack.Entry(*back.queued_scene).state==FrontendStackState::AwaitingHandler
        &&stack.Entry(*back.queued_scene).prepared->request.initial_slide=="MAIN","Missing Main readiness was fabricated");
    Check(stack.Resources(*back.queued_scene)==resources&&stack.Entry(*back.queued_scene).prepared->visuals==visuals
        &&stack.Entry(*back.queued_scene).prepared->images==images,"Returning Main replaced permanent resources");
    Check(stack.Entry(*back.queued_scene).prepared->image_completed_files==0,
        "Returning Main reread font or image bundles");
    flow.Cancel();Check(bool(stack.Entry(*back.queued_scene).prepared),"Cancellation undid an admitted original scene");
    flow.Release();Reject([&]{flow.Update(0);});stack.Release();Check(drains>=4,"Source/destination teardown did not drain owners");
    std::cout<<"Main departure "<<frames<<" frames/"<<duration<<" seconds; Options/Main destinations AwaitingHandler\n";
}
void Cancellation(const Blob& script,CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;FrontendSceneStack stack(input,[]{});const auto main=Scene(stack,1);auto frame=stack.Entry(main).published;
    FrontendMenuTransition flow(script,cameras,stack);FixtureServices service;service.save=false;service.effect=true;
    flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,frame,service.Bind());
    for(float dt:{-1.f,INFINITY,NAN})Reject([&]{flow.Update(dt);});
    Reject([&]{flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,frame,service.Bind());});
    bool rejected=false;std::thread foreign([&]{try{flow.Update(0);}catch(const std::exception&){rejected=true;}});foreign.join();
    Check(rejected&&flow.Status().state==FrontendMenuTransitionState::Running,"Wrong-thread/invalid-delta operation changed owner");
    flow.Cancel();cameras.Advance(std::max(1.f,Duration(library,"startmainmenuball")),0);flow.Update(0);
    Check(flow.Status().state==FrontendMenuTransitionState::Cancelled&&!flow.Status().animation_finished&&!flow.Status().blend_finished
        &&stack.Entries().size()==1,"Cancellation retained camera callbacks or issued a scene");
    cameras.Pop();flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,frame,service.Bind());
    cameras.Push("startidle");const auto replacement=cameras.Selection();
    Reject([&]{flow.Update(0);});Check(flow.Status().state==FrontendMenuTransitionState::Failed,"External active-blend camera change was accepted");
    flow.Cancel();Check(cameras.Selection()==replacement,"Cancellation detached/replaced another camera selection");
    auto services=service.Bind();services.trigger_stadium_effect=[](unsigned)->bool{throw std::runtime_error("real emitter failure");};
    Reject([&]{flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,frame,services);});
    Check(flow.Status().state==FrontendMenuTransitionState::Failed&&!flow.Status().queued_scene,"Host failure exposed transition success");flow.Cancel();
}
void PoppedBlendMutation(const Blob& script,CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");cameras.Push("fechoosecaptains");
    FrontendInput input;FrontendSceneStack stack(input,[]{});const auto options=Scene(stack,13);
    FrontendMenuTransition flow(script,cameras,stack);FixtureServices service;service.music=true;
    flow.Begin(FrontendMenuTransitionKind::OptionsToMain,options,stack.Entry(options).published,service.Bind());
    const auto duration=Duration(library,"outofballcam");cameras.Advance(duration,duration);flow.Update(duration);
    Check(!flow.Status().queued_scene&&cameras.Size()==1,"Return mutation test did not reach original pop blend");
    cameras.Push("startidle");const auto replacement=cameras.Selection();Reject([&]{flow.Update(0);});
    Check(flow.Status().state==FrontendMenuTransitionState::Failed&&!flow.Status().queued_scene,"Aborted popped blend fabricated a destination");
    flow.Cancel();Check(cameras.Selection()==replacement,"Old popped blend cleanup affected its replacement");
}
void QueueFailure(const Blob& script,CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;FrontendSceneStack stack(input,[]{});const auto main=Scene(stack,1);
    FrontendMenuTransition flow(script,cameras,stack);FixtureServices service;service.save=false;service.effect=service.navigation=true;
    flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,stack.Entry(main).published,service.Bind());
    for(unsigned i=1;i<32;++i)stack.QueuePush({});
    const float duration=std::max(1.f,Duration(library,"startmainmenuball"));cameras.Advance(duration,duration);flow.Update(duration);
    Check(flow.Status().state==FrontendMenuTransitionState::AwaitingNavigation,"Queue failure did not reach genuine NAV request");
    Reject([&]{flow.NavigationTransition("TransitionMainMenuToOptions");});
    Check(flow.Status().state==FrontendMenuTransitionState::Failed&&!flow.Status().queued_scene&&stack.Entries().size()==32,
        "Failed scene admission exposed a partial/successful destination");flow.Cancel();
}
void UnsupportedScript(const Blob& script,CameraAssetLibrary& library)
{
    const auto parsed=resources::ReadScriptBytecode(script);
    const auto f=std::find_if(parsed->functions.begin(),parsed->functions.end(),[](const auto& f){return f.hash==0x1b0db3b8;});
    Check(f!=parsed->functions.end(),"Departure source function absent");
    const std::size_t base=72+12*parsed->functions.size()+parsed->tweaks.size()+4*(parsed->globals.size()+parsed->data.size());
    std::size_t at=f->offset/2;while(at<parsed->code.size()&&parsed->code[at]!=((8<<11)|5))++at;
    Check(at<parsed->code.size(),"Departure source stadium host absent");auto changed=script;
    const auto instruction=(8<<11)|43;changed[base+2*at]=std::uint8_t(instruction>>8);changed[base+2*at+1]=std::uint8_t(instruction);
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;FrontendSceneStack stack(input,[]{});const auto main=Scene(stack,1);
    FrontendMenuTransition flow(changed,cameras,stack);FixtureServices service;service.save=false;
    Reject([&]{flow.Begin(FrontendMenuTransitionKind::MainDeparture,main,stack.Entry(main).published,service.Bind());});
    Check(flow.Status().state==FrontendMenuTransitionState::Failed&&cameras.Size()==1&&!flow.Status().queued_scene,
        "Unqualified emission host silently succeeded");flow.Cancel();
}
struct Host
{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";
        Check(owned||std::string_view(argv[3])=="generated","Unknown menu transition mode");
        const auto folder=(std::filesystem::path(argv[2])/"menu-transition-host").string();std::filesystem::create_directories(folder);
        AuroraConfig config{};config.appName="Charged native menu transitions";config.userPath=config.cachePath=folder.c_str();
        config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
        config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();
        Check(aurora_dvd_open(argv[1]),"Menu transition disc missing");host.disc=true;nlInitFileSystem();
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
            {CameraAssetLibrary library;Catalog(library);auto script=Load("art/scripts/fe_presentation.byte_code");
             RoundTrip(script,library,owned);Cancellation(script,library);PoppedBlendMutation(script,library);QueueFailure(script,library);UnsupportedScript(script,library);}
            Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),
                "Menu transitions did not recover native reads/arenas");
        }
        std::cout<<checks<<" original menu bytecode checks passed; concrete handlers/services remain explicitly pending\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
