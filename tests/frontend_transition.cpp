#include "runtime/frontend_transition.h"
#include "runtime/frontend_camera_assets.h"
#include "runtime/animated_camera.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/FE/FrontendTransitionSteps.h"
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
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
void Near(float a,float b,float tolerance=.0001f){Check(std::isfinite(a)&&std::abs(a-b)<=tolerance,"Camera/transition float oracle differs");}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid transition operation accepted at "+std::to_string(at.line()));}
using Blob=std::vector<std::uint8_t>;
Blob Load(const char* filename)
{
    unsigned long size=0;void* raw=nlLoadEntireFile(filename,&size,32,AllocateStart,0,0,0);
    Check(raw&&size,"Actual NL bytecode read failed");
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
    Check(batch.State()==CameraBatchState::Ready&&batch.Progress().succeeded==37,"Actual37-camera catalog did not complete");
    batch.Publish();
    for(const auto& entry:FrontendCameraCatalog())Check(bool(library.Find(entry.animationName)),"Catalog alias missing after publish");
}
FrontendStackCallbacks Inspection(unsigned& calls)
{
    // Explicit fixture/native inspection callbacks, never a concrete Title/Main
    // implementation. They validate source-order context and record calls.
    return {
        [&](auto& context){Check(context.session.Current()==context.handler.Current(),"SetPresentation did not precede callback");Check(context.movement<=2,"Original movement lost");++calls;},
        [&](auto&){++calls;},
        [&](auto&,float){++calls;}
    };
}
FrontendSceneStack::Token Title(FrontendSceneStack& stack,unsigned& calls)
{
    FrontendStackRequest request;request.scene=0;request.initial_slide="regular";
    const auto token=stack.QueuePush(request,Inspection(calls));Pump(stack,token);
    Check(stack.Entry(token).state==FrontendStackState::AwaitingPublication,"Inspection title resource callbacks failed");
    stack.Publish(token,stack.Entry(token).prepared);return token;
}
void SharedWaits()
{
    bool done=false;Check(FrontendPresentationWaitCamera(done)&&!done,"Missing end did not retry");
    done=true;Check(!FrontendPresentationWaitCamera(done)&&!done,"End was not consumed once");
    for(float delta:{0.f,.025f,.1f,std::nextafter(.1f,0.f),std::nextafter(.1f,1.f),1.f})
    {
        float wait=.1f;const float subtraction=.1f-delta;const bool retry=FrontendPresentationWaitTime(wait,delta);
        Check(retry==(subtraction>0),"Original wait boundary differs");
        Check(std::bit_cast<unsigned>(wait)==std::bit_cast<unsigned>(subtraction>0?subtraction:0.f),"Original single float subtraction bits differ");
    }
}
void CameraOperations(CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras cameras(core,library);
    Reject([&]{cameras.Selection();});cameras.Push("startidle");
    auto before=cameras.Selection();cameras.Seek(before,.375f);
    const auto* identity=cameras.ActiveCamera();const auto pose=identity->GetViewMatrix();const auto old_fov=identity->GetFOV();
    unsigned ended=0;
    auto selected=cameras.Select(before,"startmainmenumove",false,[&]{++ended;Reject([&]{cameras.Release();});Reject([&]{cameras.Selection();});});
    Check(selected!=before&&identity==cameras.ActiveCamera(),"Asset selection replaced original CameraMan identity");
    Near(cameras.Time(selected),.375f);Near(identity->GetFOV(),old_fov);
    Check(std::memcmp(&pose,&identity->GetViewMatrix(),sizeof(pose))==0,"Selection rebuilt view before source seek");
    Reject([&]{cameras.Seek(before,0);});Check(!cameras.DetachEndCallback(before),"Stale selection removed newer callback");
    Reject([&]{cameras.Select(selected,"absent",false);});Check(cameras.Selection()==selected&&!cameras.Failed(),"Invalid alias changed camera");
    for(float time:{-1.f,INFINITY,NAN,1.01f})Reject([&]{cameras.Seek(selected,time);});
    for(float time:{0.f,std::nextafter(1.f,0.f),1.f}){cameras.Seek(selected,time);Near(cameras.Time(selected),time);}
    cameras.Seek(selected,1);Check(ended==0,"Source seek fabricated an end callback");
    cameras.Advance(0,0);Check(ended==1,"Source endpoint Update did not call completion");
    cameras.Advance(0,0);Check(ended==2,"Noncyclic endpoint callback was incorrectly one-shot");
    cameras.SetCyclic(selected,true);cameras.Seek(selected,1);cameras.Advance(0,0);
    Near(cameras.Time(selected),0);Check(ended==3,"Cyclic source callback/wrap differs");
    cameras.SetCyclic(selected,false);cameras.Seek(selected,0);
    const float duration=cameras.Duration(selected);
    cameras.Advance(duration,duration);Near(cameras.Time(selected),1);Check(ended==4,"Duration endpoint did not finish");
    Check(cameras.DetachEndCallback(selected),"Current callback did not detach");cameras.Advance(0,0);Check(ended==4,"Detached completion fired");
    auto stale=selected;cameras.Pop();Check(!cameras.DetachEndCallback(stale),"Popped identity remained callback owner");
    cameras.Push("startidle");Reject([&]{cameras.Time(stale);});
    core.Release();Check(!cameras.DetachEndCallback(cameras.Failed()?FrontendCameraSelectionHandle{}:stale),"Core release retained callback");
}
void SelectionLifetime(CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras a(core,library),b(core,library);a.Push("startidle");
    unsigned ends=0;auto selected=a.Select(a.Selection(),"startmainmenumove",false,[&]{++ends;});
    std::weak_ptr<const CameraAsset> weak=library.Find("startmainmenumove");library.Erase("startmainmenumove");
    a.Seek(selected,.25f);Check(!weak.expired()&&a.Duration(selected)>0,"Reselected camera lost its retained data");
    b.Push("startidle");auto foreign=b.Selection();Reject([&]{b.Seek(selected,0);});Reject([&]{a.Seek(selected,0);});
    Check(a.DetachEndCallback(selected),"Inactive owned callback could not be detached");
    b.Pop();a.Seek(selected,1);a.Advance(0,0);Check(!ends,"Inactive callback detach removed wrong registration");
    a.Pop();Check(weak.expired(),"Popped reselected camera retained its native key data");
    for(const auto& source:FrontendCameraCatalog())if(std::string_view(source.animationName)=="startmainmenumove")
        library.Insert(LoadCameraAsset(source.fileName,source.animationName));
    Check(bool(library.Find("startmainmenumove")),"Retained asset test did not restore catalog");
    Reject([&]{a.Time(selected);});Reject([&]{b.Time(foreign);});
}
void CallbackFailure(CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    auto selection=cameras.Select(cameras.Selection(),"startmainmenumove",false,[]{throw std::runtime_error("real end callback failure");});
    cameras.Seek(selection,1);Reject([&]{core.Advance(0,0);});Check(cameras.Failed(),"Failed end callback did not poison frontend owner");
    Check(cameras.DetachEndCallback(selection),"Failure prevented callback cleanup");cameras.Release();
}
void Transition(const Blob& script,CameraAssetLibrary& library,bool owned)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;unsigned drains=0,calls=0;
    FrontendSceneStack stack(input,[&]{++drains;});const auto title=Title(stack,calls);auto frame=stack.Entry(title).published;
    FrontendTransition transition(script,cameras,stack);
    auto wrong=std::make_shared<FrontendSessionFrame>(*frame);Reject([&]{transition.Begin(title,wrong);});
    Check(transition.Status().state==FrontendTransitionState::Idle,"Rejected frame mutated VM");
    // Title's original callback queues Pop before invoking this script.
    stack.QueuePop(title);transition.Begin(title,frame);const auto selected=transition.Status().camera;
    Check(transition.Status().state==FrontendTransitionState::Running&&cameras.ActiveAlias()=="startmainmenumove", "Original transition did not select named track");
    Near(cameras.Time(selected),0);
    const std::vector<unsigned> begin{15,28,44};Check(std::vector(transition.Calls().begin(),transition.Calls().end())==begin,"Source initial host order differs");
    stack.Poll();Reject([&]{stack.Entry(title);});frame.reset();
    const float duration=cameras.Duration(selected),dt=1.f/60.f;
    float oracle_time=0,oracle_wait=0;bool camera_end=false,waiting_time=false,expected_push=false;
    unsigned frames=0;
    while(transition.Status().state==FrontendTransitionState::Running)
    {
        // Independent source float state machine; the VM executes actual
        // authored instructions and the original camera generates completion.
        oracle_time+=(dt*1.f)/duration;
        if(oracle_time>=1){oracle_time=1;camera_end=true;}
        cameras.Advance(dt,dt);
        if(!waiting_time&&camera_end){camera_end=false;oracle_wait=.1f;waiting_time=true;}
        if(waiting_time){oracle_wait-=dt;if(oracle_wait<=0){oracle_wait=0;expected_push=true;}}
        transition.Update(dt);++frames;
        Near(cameras.Time(selected),oracle_time);
        Check(bool(transition.Status().queued_scene)==expected_push,"VM queued Main at a different source frame");
        Near(transition.Status().wait,oracle_wait);
        Check(frames<2000,"Title transition exceeded bounded authored duration");
    }
    const auto result=transition.Status();Check(result.state==FrontendTransitionState::SceneQueued&&result.queued_scene,"Transition did not return to Idle after Push");
    const auto main=*result.queued_scene;Check(stack.Entry(main).scene==1&&stack.Entry(main).movement==1,"Original Main/forward queue arguments lost");
    const auto trace=std::vector(transition.Calls().begin(),transition.Calls().end());
    Check(std::count(trace.begin(),trace.end(),15)==1&&std::count(trace.begin(),trace.end(),28)==1
        &&std::count(trace.begin(),trace.end(),32)==1&&std::count(trace.begin(),trace.end(),25)==1&&trace.back()==8,"Transition host ordering/count differs");
    Pump(stack,main);Check(stack.Entry(main).state==FrontendStackState::AwaitingHandler&&stack.RenderPlan().empty(),"Missing Main handler was fabricated");
    const auto target=stack.Entry(main).prepared;
    Check(target&&target->request.initial_slide=="MAIN","Actual Main presentation selection lost");
    if(owned)Check(target->graph.slides.size()==134&&target->images->textures.size()==30,"Owned Main resources differ");
    transition.Cancel();Check(stack.Entry(main).prepared==target,"Cancel undid already issued original scene push");
    const auto old_time=cameras.Time(selected);cameras.Advance(0,0);Near(cameras.Time(selected),old_time);
    transition.Release();Reject([&]{transition.Update(0);});
    stack.Cancel(main);stack.Release();
    Check(drains>=3,"Retained title/scene teardown skipped explicit drain");
    std::cout<<"Title transition frames "<<frames<<", duration "<<duration<<", Main AwaitingHandler; no concrete menu readiness\n";
}
void Cancellation(const Blob& script,CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;unsigned calls=0;FrontendSceneStack stack(input,[]{});const auto title=Title(stack,calls);auto frame=stack.Entry(title).published;
    FrontendTransition transition(script,cameras,stack);transition.Begin(title,frame);auto selection=transition.Status().camera;
    transition.Cancel();cameras.Advance(cameras.Duration(selection),0);transition.Update(.1f);
    Check(transition.Status().state==FrontendTransitionState::Cancelled&&!transition.Status().camera_finished
        &&stack.Entries().size()==1,"Cancelled transition retained callback or queued Main");
    transition.Begin(title,frame);selection=transition.Status().camera;
    auto replacement=cameras.Select(selection,"startidle",true);
    Reject([&]{transition.Update(.01f);});Check(transition.Status().state==FrontendTransitionState::Failed,"External reselection was not detected");
    transition.Cancel();Check(cameras.Selection()==replacement,"Stale transition cancellation removed new selection");
    transition.Begin(title,frame);Reject([&]{transition.Begin(title,frame);});
    for(float dt:{-1.f,INFINITY,NAN})Reject([&]{transition.Update(dt);});
    Check(transition.Status().state==FrontendTransitionState::Running,"Invalid delta poisoned validated state");
    bool reject=false;std::thread wrong([&]{try{transition.Update(0);}catch(const std::exception&){reject=true;}});wrong.join();Check(reject,"Wrong-thread transition accepted");
    transition.Cancel();auto bad=FrontendStackRequest{};bad.movement=3;Reject([&]{stack.QueuePush(bad);});
    for(unsigned movement=0;movement<3;++movement)
    {
        auto request=FrontendStackRequest{};request.movement=movement;auto cb=Inspection(calls);
        cb.scene_created=[&](auto& context){Check(context.movement==movement,"Movement missing from creation callback");++calls;};
        const auto token=stack.QueuePush(request,cb);Pump(stack,token);Check(stack.Entry(token).movement==movement,"Movement lost in entry");stack.Cancel(token);
    }
    core.Release();transition.Release();
}
void QueueFailure(const Blob& script,CameraAssetLibrary& library)
{
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;unsigned calls=0;FrontendSceneStack stack(input,[]{});const auto title=Title(stack,calls);
    FrontendTransition transition(script,cameras,stack);transition.Begin(title,stack.Entry(title).published);
    for(unsigned i=1;i<32;++i)stack.QueuePush({});
    const auto selected=transition.Status().camera;cameras.Seek(selected,1);cameras.Advance(0,0);
    Reject([&]{transition.Update(.1f);});Check(transition.Status().state==FrontendTransitionState::Failed
        &&!transition.Status().queued_scene&&stack.Entries().size()==32,"Failed Main admission exposed success or partial queue entry");
    transition.Cancel();cameras.Advance(0,0);Check(!transition.Status().camera_finished,"Failed queue cleanup retained camera callback");
}
void UnsupportedScript(const Blob& script,CameraAssetLibrary& library)
{
    auto parsed=resources::ReadScriptBytecode(script);
    const auto function=std::find_if(parsed->functions.begin(),parsed->functions.end(),[](const auto& f){return f.hash==0xc41b2549;});
    Check(function!=parsed->functions.end(),"Transition fixture entry is absent");
    const std::size_t base=72+12*parsed->functions.size()+parsed->tweaks.size()+4*(parsed->globals.size()+parsed->data.size());
    std::size_t at=function->offset/2;
    while(at<parsed->code.size()&&parsed->code[at]!=((8<<11)|28))++at;
    Check(at<parsed->code.size(),"Source transition has no seek host");
    auto modified=script;const auto opcode=(8<<11)|20;modified[base+2*at]=opcode>>8;modified[base+2*at+1]=opcode;
    OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
    FrontendInput input;unsigned calls=0;FrontendSceneStack stack(input,[]{});const auto title=Title(stack,calls);
    FrontendTransition transition(modified,cameras,stack);
    Reject([&]{transition.Begin(title,stack.Entry(title).published);});
    Check(transition.Status().state==FrontendTransitionState::Failed&&!transition.Status().queued_scene,
        "Unqualified presentation audio host was treated as success");
    transition.Cancel();Check(stack.Entries().size()==1,"Unsupported script queued a scene");
}
struct Host
{
    bool live=false,disc=false;
    ~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}
};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==4,"Supply disc, output folder and generated/owned mode");const bool owned=std::string_view(argv[3])=="owned";
        Check(owned||std::string_view(argv[3])=="generated","Unknown transition test mode");
        const auto folder=(std::filesystem::path(argv[2])/"frontend-transition-data").string();std::filesystem::create_directories(folder);
        AuroraConfig config{};config.appName="Charged frontend transition";config.userPath=config.cachePath=folder.c_str();
        config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
        config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Host host;auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora core failed");
        InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot mount transition disc");host.disc=true;nlInitFileSystem();
        SharedWaits();
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
            {
                CameraAssetLibrary library;Catalog(library);auto script=Load("art/scripts/fe_presentation.byte_code");
                CameraOperations(library);SelectionLifetime(library);CallbackFailure(library);Transition(script,library,owned);Cancellation(script,library);QueueFailure(script,library);UnsupportedScript(script,library);
            }
            Check(!nlAsyncReadsPending(nullptr)&&StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Transition did not recover native files/arenas");
        }
        std::cout<<checks<<" frontend transition checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
