#include "runtime/frontend_menu_transition.h"
#include "Game/FE/FrontendTransitionSteps.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <thread>

namespace mscharged
{
namespace
{
constexpr unsigned idle=0x0017a6dd,departure=0x1b0db3b8,to_options=0x6f23e5f3,to_main=0xc18f9013;
constexpr std::string_view options_function="TransitionMainMenuToOptions";
void Require(bool value,const char* message){if(!value)throw std::logic_error(message);}
struct Guard{bool& busy;explicit Guard(bool& value):busy(value){busy=true;}~Guard(){busy=false;}};
float Float(const InterpreterValue& value)
{
    const auto f=std::bit_cast<float>(std::get<unsigned>(value));
    Require(std::isfinite(f)&&f>=0&&f<=60,"Menu camera duration is outside supported range");return f;
}
void CheckCallbacks(const FrontendStackCallbacks& c)
{
    const bool complete=c.scene_created&&c.initialize_subhandlers&&c.after_base_update;
    Require(complete||(!c.scene_created&&!c.initialize_subhandlers&&!c.after_base_update),
        "Menu destination callbacks must be complete or explicitly unavailable");
}
}
struct FrontendMenuTransition::Implementation
{
    struct Completion{bool animation=false,blend=false;};
    NativeInterpreter vm;
    FrontendCameras& cameras;FrontendSceneStack& stack;
    const std::thread::id thread=std::this_thread::get_id();
    FrontendMenuTransitionState state=FrontendMenuTransitionState::Idle;
    FrontendMenuTransitionKind kind=FrontendMenuTransitionKind::MainDeparture;
    FrontendMenuTransitionServices services;FrontendStackCallbacks callbacks;
    FrontendSession::Handle source;
    FrontendSessionResources::Handle resources;
    FrontendCameraSelectionHandle camera,blend_camera;
    std::vector<FrontendCameraSelectionHandle> end_callbacks;
    std::shared_ptr<Completion> completion=std::make_shared<Completion>();
    std::optional<FrontendMenuTransitionService> pending;
    std::optional<FrontendSceneStack::Token> queued;
    std::vector<unsigned> calls;std::size_t trace_limit;
    bool busy=false,navigation_started=false,options_stage=false;
    Implementation(resources::Bytes bytes,FrontendCameras& c,FrontendSceneStack& s,InterpreterLimits limits)
        :vm(bytes,limits),cameras(c),stack(s),trace_limit(std::min<std::size_t>(limits.host_calls,10000))
    {
        const auto script=resources::ReadScriptBytecode(bytes);
        for(auto hash:{idle,departure,to_options,to_main})
            Require(std::any_of(script->functions.begin(),script->functions.end(),[&](const auto& f){return f.hash==hash&&!f.arguments&&!f.flags;}),
                "Menu transition requires the original zero-argument frontend functions");
        calls.reserve(trace_limit);end_callbacks.reserve(4);
        for(unsigned id:{5u,8u,21u,23u,25u,35u,41u,42u,44u,46u,47u})
        {
            InterpreterHostCall host{id,{},false,[this,id](auto args){return Invoke(id,args);}};
            if(id==23)host.arguments={InterpreterValueKind::String,InterpreterValueKind::Word,InterpreterValueKind::Word};
            if(id==25)host.arguments={InterpreterValueKind::Word,InterpreterValueKind::Word};
            if(id==5||id==21||id==35)host.arguments={InterpreterValueKind::Word};
            vm.Bind(std::move(host));
        }
        Idle();
    }
    void Thread()const{Require(thread==std::this_thread::get_id(),"Menu transition requires its owner thread");}
    void Mutable(bool cleanup=false)const
    {
        Thread();Require(!busy&&state!=FrontendMenuTransitionState::Released,"Menu transition is busy or released");
        Require(cleanup||state!=FrontendMenuTransitionState::Failed,"Failed menu transition requires cancellation");
    }
    void Idle()
    {vm.Reset();Require(vm.Execute(idle)&&vm.Status()==InterpreterStatus::Paused,"Original frontend Idle did not wait");}
    void Trace(unsigned id)
    {
        if(!trace_limit)return;if(id==8&&!calls.empty()&&calls.back()==8)return;
        if(calls.size()==trace_limit)calls.erase(calls.begin());calls.push_back(id);
    }
    InterpreterHostResult Wait(FrontendMenuTransitionService service)
    {pending=service;state=FrontendMenuTransitionState::AwaitingService;return {{},InterpreterFlow::Retry};}
    void Current()const{Require(cameras.IsCurrent(camera),"Menu transition camera was selected or removed externally");}
    InterpreterHostResult Invoke(unsigned id,std::span<const InterpreterValue> args)
    {
        Trace(id);if(id==8)return {{},InterpreterFlow::Retry};
        Require(state==FrontendMenuTransitionState::Running||state==FrontendMenuTransitionState::AwaitingService,
            "Menu host ran outside an active transition");
        switch(id)
        {
        case 47:
        {
            Require(kind==FrontendMenuTransitionKind::MainDeparture&&!options_stage,"Unexpected save gate in menu script");
            const auto value=services.save_busy?services.save_busy():std::optional<bool>{};
            if(!value||*value)return Wait(FrontendMenuTransitionService::SaveGate);
            break;
        }
        case 5:
            Require(kind==FrontendMenuTransitionKind::MainDeparture&&!options_stage&&std::get<unsigned>(args[0])==95,
                "Unqualified frontend stadium effect");
            if(!services.trigger_stadium_effect||!services.trigger_stadium_effect(95))return Wait(FrontendMenuTransitionService::StadiumEffect);
            break;
        case 23:
        {
            const auto& name=std::get<std::string>(args[0]);const auto duration=Float(args[1]);const auto replace=std::get<unsigned>(args[2]);
            if(kind==FrontendMenuTransitionKind::OptionsToMain)
                Require(name=="outofballcam"&&duration==0&&replace==1,"Unexpected Options return camera");
            else if(options_stage)
                Require(name=="fechoosecaptains"&&duration==0&&replace==0,"Unexpected Options arrival camera");
            else Require(name=="startmainmenuball"&&duration==.25f&&replace==0,"Unexpected Main departure camera");
            Current();Require(end_callbacks.size()<4,"Menu camera callback budget exhausted");
            const std::weak_ptr<Completion> weak=completion;
            camera=cameras.PushAnimated(name,false,[weak]{if(auto c=weak.lock())c->animation=true;},
                [weak](auto){if(auto c=weak.lock())c->blend=true;},duration,replace!=0);
            end_callbacks.push_back(camera);blend_camera=camera;
            // Original host23 clears both flags after the push, including an
            // abort of an older CameraMan transition during that operation.
            completion->animation=completion->blend=false;
            break;
        }
        case 44:Current();if(FrontendPresentationWaitCamera(completion->animation))return {{},InterpreterFlow::Retry};break;
        case 46:
            Current();
            // After Pop the remaining top identity is retained separately;
            // completion comes from the actual manager blend callback.
            if(FrontendPresentationWaitCamera(completion->blend))return {{},InterpreterFlow::Retry};break;
        case 21:
        {
            const auto duration=Float(args[0]);Current();
            Require(duration==(kind==FrontendMenuTransitionKind::OptionsToMain?1.f:0.f)&&!options_stage,
                "Unexpected frontend camera pop duration");
            const std::weak_ptr<Completion> weak=completion;blend_camera=camera;
            cameras.PopAnimated(camera,[weak](auto){if(auto c=weak.lock())c->blend=true;},duration);
            camera=cameras.CurrentSelection();completion->blend=false;break;
        }
        case 35:Current();Require(options_stage&&std::get<unsigned>(args[0])==1,"Unexpected frontend cyclic state");cameras.SetCyclic(camera,true);break;
        case 41:
            Require(kind==FrontendMenuTransitionKind::MainDeparture&&!options_stage,"Unexpected NAV transition request");
            if(!services.start_navigation||!services.start_navigation(options_function))return Wait(FrontendMenuTransitionService::Navigation);
            navigation_started=true;break;
        case 42:
            Require(kind==FrontendMenuTransitionKind::OptionsToMain,"Unexpected return music request");
            if(!services.start_music_navigation||!services.start_music_navigation())return Wait(FrontendMenuTransitionService::MusicNavigation);
            break;
        case 25:
        {
            const unsigned scene=std::get<unsigned>(args[0]),movement=std::get<unsigned>(args[1]);
            Require((options_stage&&scene==13&&movement==1)||(kind==FrontendMenuTransitionKind::OptionsToMain&&scene==1&&movement==2),
                "Menu script requested an unqualified destination");
            Require(source&&!queued,"Menu script has no retained source or already queued its destination");
            FrontendStackRequest request;request.scene=scene;request.movement=movement;request.language=source->request.language;
            request.image_profile=FrontendImageProfile::Main;request.initial_slide=scene==13?"in":"MAIN";
            if(resources)request.shared_resources=resources;
            queued=stack.QueuePush(std::move(request),callbacks);break;
        }
        default:throw std::logic_error("Unqualified menu transition host");
        }
        pending.reset();state=FrontendMenuTransitionState::Running;return {};
    }
    void Complete()
    {
        if(vm.Status()!=InterpreterStatus::Ready)return;
        Require(queued||(kind==FrontendMenuTransitionKind::MainDeparture&&navigation_started&&!options_stage),
            "Menu script ended without its original destination or NAV request");
        completion->animation=completion->blend=false;Idle();
        state=queued?FrontendMenuTransitionState::SceneQueued:FrontendMenuTransitionState::AwaitingNavigation;
    }
    void Detach()
    {
        for(const auto& selection:end_callbacks)cameras.DetachEndCallback(selection);
        end_callbacks.clear();if(blend_camera)cameras.DetachTransitionCallback(blend_camera);
        camera.reset();blend_camera.reset();completion->animation=completion->blend=false;
    }
};
FrontendMenuTransition::FrontendMenuTransition(resources::Bytes bytes,FrontendCameras& cameras,FrontendSceneStack& stack,InterpreterLimits limits)
    :impl_(std::make_unique<Implementation>(bytes,cameras,stack,limits)){}
FrontendMenuTransition::~FrontendMenuTransition(){try{Release();}catch(...){std::terminate();}}
void FrontendMenuTransition::Begin(FrontendMenuTransitionKind kind,FrontendSceneStack::Token token,
    const FrontendSession::Handle& frame,FrontendMenuTransitionServices services,FrontendStackCallbacks callbacks)
{
    auto& s=*impl_;s.Mutable();CheckCallbacks(callbacks);
    Require(kind==FrontendMenuTransitionKind::MainDeparture||kind==FrontendMenuTransitionKind::OptionsToMain,
        "Unknown menu transition kind");
    Require(s.state!=FrontendMenuTransitionState::Running&&s.state!=FrontendMenuTransitionState::AwaitingService
        &&s.state!=FrontendMenuTransitionState::AwaitingNavigation,"Menu transition is already active");
    const auto entry=s.stack.Entry(token);
    Require(frame&&entry.published==frame&&entry.scene==(kind==FrontendMenuTransitionKind::MainDeparture?1u:13u)
        &&(entry.state==FrontendStackState::Published||entry.state==FrontendStackState::AwaitingPublication),
        "Menu transition requires its exact published source scene");
    auto selected=s.cameras.Selection();auto resources=s.stack.Resources(token);
    auto completion=std::make_shared<Implementation::Completion>();Guard guard(s.busy);
    try
    {
        s.Detach();s.kind=kind;s.source=frame;s.resources=std::move(resources);s.services=std::move(services);s.callbacks=std::move(callbacks);
        s.camera=std::move(selected);s.completion=std::move(completion);s.queued.reset();s.pending.reset();s.calls.clear();
        s.navigation_started=s.options_stage=false;s.vm.Reset();s.state=FrontendMenuTransitionState::Running;
        Require(s.vm.Execute(kind==FrontendMenuTransitionKind::MainDeparture?departure:to_main),"Original menu transition is absent");s.Complete();
    }
    catch(...){s.state=FrontendMenuTransitionState::Failed;throw;}
}
void FrontendMenuTransition::NavigationTransition(std::string_view function)
{
    auto& s=*impl_;s.Mutable();Require(s.state==FrontendMenuTransitionState::AwaitingNavigation&&function==options_function,
        "Menu transition requires its actual pending NAV destination");Guard guard(s.busy);
    try
    {
        s.Current();s.camera=s.cameras.Selection();s.options_stage=true;s.vm.Reset();s.state=FrontendMenuTransitionState::Running;
        Require(s.vm.Execute(to_options),"Original Main-to-Options function is absent");s.Complete();
    }
    catch(...){s.state=FrontendMenuTransitionState::Failed;throw;}
}
void FrontendMenuTransition::Update(float delta)
{
    auto& s=*impl_;s.Mutable();CheckCameraDelta(delta);Guard guard(s.busy);
    if(s.state!=FrontendMenuTransitionState::Running&&s.state!=FrontendMenuTransitionState::AwaitingService)return;
    try
    {if(s.camera)s.Current();if(s.vm.Status()==InterpreterStatus::Paused)s.vm.Resume();s.Complete();}
    catch(...){s.state=FrontendMenuTransitionState::Failed;throw;}
}
FrontendMenuTransitionStatus FrontendMenuTransition::Status()const
{
    const auto& s=*impl_;s.Thread();return {s.state,s.pending,s.queued,s.camera,s.completion->animation,s.completion->blend};
}
std::span<const unsigned> FrontendMenuTransition::Calls()const{impl_->Thread();return impl_->calls;}
void FrontendMenuTransition::Cancel()
{
    auto& s=*impl_;s.Mutable(true);Guard guard(s.busy);s.Detach();s.vm.Reset();s.source.reset();s.resources.reset();s.services={};s.callbacks={};s.pending.reset();
    s.state=FrontendMenuTransitionState::Cancelled;
}
void FrontendMenuTransition::Release()
{
    auto& s=*impl_;s.Thread();if(s.state==FrontendMenuTransitionState::Released)return;s.Mutable(true);Guard guard(s.busy);
    s.Detach();s.vm.Reset();s.source.reset();s.resources.reset();s.services={};s.callbacks={};s.pending.reset();s.state=FrontendMenuTransitionState::Released;
}
}
