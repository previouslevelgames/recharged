#include "runtime/frontend_title.h"
#include "resources/frontend_animation.h"
#include "Game/FE/FrontendTitleSteps.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <thread>
namespace mscharged
{
namespace
{
using namespace resources;
void CheckTitle(bool v,const char* text){if(!v)throw std::logic_error(text);}
struct State
{
    FrontendTitleStatus status;
    std::uint32_t instance=0;
    FrontendPointerBinding binding;
    std::optional<FrontendPointerBounds> measured_bounds;
};
struct Step
{
    FrontendAnimationPlayback& playback;State value;
    float m_fTimeElapsed=value.status.elapsed;
    bool mInitialized=value.status.initialized,mStartedDemo=value.status.started_demo,mUnidentifiedDF=value.status.highlighted;
    unsigned mMovement;
    std::array<bool,9> mControllerReady=value.status.sequence;
    const int mControllerDefaults[10]={
#include "Game/FE/FrontendTitleDefaults.inc"
    };
    struct Slide{Step& owner;std::uint32_t id;};
    struct Instance
    {
        Step& owner;std::uint32_t id;
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {
            const auto& list=owner.playback.Scene().instances;const auto at=std::find_if(list.begin(),list.end(),[&](const auto& v){return v.offset==id;});
            CheckTitle(at!=list.end()&&at->type==4&&at->library,"Title feedback requires its actual component");
            CheckTitle(owner.playback.SelectComponent(*at->library,name,reset,preserve),"Title feedback slide is absent");
        }
    };
    std::map<std::uint32_t,Instance> instances;
    std::optional<Slide> active;
    struct Presentation
    {
        Step& owner;Slide* m_currentSlide=nullptr;
        void SetActiveSlide(const char* name,bool reset)
        {CheckTitle(owner.playback.SelectPresentation(name,reset),"Title presentation slide is absent");owner.SyncSlide();}
    } presentation{*this};Presentation* mPresentation=&presentation;
    Instance* mTextPressStart=nullptr;
    struct Button
    {
        std::array<int,4> states{};FrontendPointerBinding binding;
        void SetPointerState(int value,unsigned index){CheckTitle(index<4,"Title pointer exceeds four");states[index]=value;}
        void SetInstanceBounds(Instance* instance,bool rotate,float x,float y,float sx,float sy)
        {CheckTitle(instance,"Title hit component is absent");binding={instance->id,rotate,x,y,sx,sy};}
    } mControllerComponent;
    Step(FrontendAnimationPlayback& p,State s,unsigned movement):playback(p),value(std::move(s)),mMovement(movement)
    {if(value.instance)mTextPressStart=At(value.instance);mControllerComponent={value.status.pointer_states,value.binding};SyncSlide();}
    Instance* At(std::uint32_t id){return &instances.try_emplace(id,Instance{*this,id}).first->second;}
    void SyncSlide(){CheckTitle(bool(playback.Scene().active_slide),"Title has no active presentation");active.emplace(Slide{*this,*playback.Scene().active_slide});presentation.m_currentSlide=&*active;}
    template<class T,int N>struct Finder
    {
        template<class Root>static T* Find(Root* root,const char* first,const char* second,int a,int b,int c,int d)
        {
            CheckTitle(!a&&!b&&!c&&!d,"Title finder sentinel differs");
            const std::array<std::string_view,2> path{first,second};
            const auto found=FindFrontendNode(root->owner.playback.Scene(),{FrontendNodeKind::Slide,root->id},FrontendNamedPath(path),FrontendNodeType::Any);
            CheckTitle(found&&found->kind==FrontendNodeKind::Instance,"Title press component is absent");return root->owner.At(found->id);
        }
    };
    void Command(FrontendTitleCommandKind kind,unsigned value=0)
    {
        CheckTitle(this->value.status.commands.size()<32,"Title command budget exceeded");
        this->value.status.commands.push_back({kind,value});
        this->value.status.operations.push_back({FrontendTitleOperationKind::Command,{kind,value},0});
    }
    void AudioOperation(FrontendTitleOperationKind kind,unsigned argument=0)
    {CheckTitle(value.status.operations.size()<64,"Title operation budget exceeded");value.status.operations.push_back({kind,{},argument});}
    void NewOperations()
    {value.status.commands.clear();value.status.operations.clear();value.status.admitted_operations=0;}
    void Pointer(int i,const char* name)
    {
        CheckTitle(i>=0&&i<4,"Title global pointer exceeds four");
        const std::string_view v=name;
        CheckTitle(v=="waiting"||v=="cursor"||v=="A","Title pointer slide is unsupported");
        Command(v=="waiting"?FrontendTitleCommandKind::PointerWaiting:v=="cursor"?FrontendTitleCommandKind::PointerCursor:FrontendTitleCommandKind::PointerAccept,unsigned(i));
    }
    State Result()
    {
        CheckTitle(mTextPressStart,"Title press component is absent");value.instance=mTextPressStart->id;value.binding=mControllerComponent.binding;
        value.status.elapsed=m_fTimeElapsed;value.status.initialized=mInitialized;value.status.started_demo=mStartedDemo;
        value.status.highlighted=mUnidentifiedDF;value.status.sequence=mControllerReady;value.status.pointer_states=mControllerComponent.states;return std::move(value);
    }
};
struct Pending{FrontendPointerCallback kind;unsigned index;FrontendSession::Handle frame;};
struct Busy{bool& flag;explicit Busy(bool& f):flag(f){flag=true;}~Busy(){flag=false;}};
}
struct FrontendTitle::Implementation
{
    std::shared_ptr<FrontendSession> session;FrontendInput& input;
    std::shared_ptr<FrontendAudio> audio;std::shared_ptr<FrontendMusic> music;unsigned& seed;FrontendTitleOptions options;
    std::shared_ptr<FrontendHandler> handler;FrontendPointerHost host;std::shared_ptr<FrontendPointerRegion> region;
    FrontendSession::Handle current,input_source;State state;
    std::vector<Pending> pending;std::vector<FrontendAudioHandle> sounds;
    std::thread::id thread=std::this_thread::get_id();
    bool busy=false,failed=false,stack_attached=false,stack_update=false,input_window=false;
    Implementation(std::shared_ptr<FrontendSession> s,FrontendInput& i,std::shared_ptr<FrontendAudio> a,std::shared_ptr<FrontendMusic> m,unsigned& rng,FrontendTitleOptions o)
        :session(std::move(s)),input(i),audio(std::move(a)),music(std::move(m)),seed(rng),options(o),host(i,o.controller)
    {
        CheckTitle(!nlGetCurrentAsyncRead()&&session&&audio&&audio->Loaded()&&music,"Title requires retained scene/audio/music and idle NL services");
        CheckTitle(o.controller<4&&o.movement<=2&&o.device==FrontendTitleDevice::DesktopWithoutWiiServices,"Unsupported Title control profile");
        current=session->Current();CheckTitle(current&&current->request.animate&&current->request.image_profile==FrontendImageProfile::Main,"Title requires animated Main resources");
        CheckTitle(o.deferred_services||music->Status().load!=FrontendMusicLoadState::Loading,"Standalone Title cannot replace another pending music request");pending.reserve(8);
    }
    void Ready()const{CheckTitle(thread==std::this_thread::get_id()&&session&&!failed,"Title requires its live nonfailed owner thread");}
    void Mutable()const{Ready();CheckTitle(!busy&&!nlGetCurrentAsyncRead()&&(!stack_attached||stack_update),"Title mutation requires idle owner or controlled stack update");}
    void Expected(const FrontendSession::Handle& f,bool acknowledge=false)const
    {
        if(acknowledge){Ready();CheckTitle(!busy&&!stack_update&&!nlGetCurrentAsyncRead(),"Title acknowledgement during update");}else Mutable();
        CheckTitle(f&&f==current&&f==session->Current(),"Title requires its exact current frame");
    }
    FrontendSession::Handle Presented()const{return input_window?input_source:current;}
    bool PendingOperations()const{return options.deferred_services&&state.status.admitted_operations<state.status.operations.size();}
    bool CanRoute()const{return state.status.initialized&&!state.status.departure&&(!PendingOperations()||(stack_update&&input_window))&&region&&region->Current()==Presented();}
    void Queue(FrontendPointerCallback kind,unsigned index,const FrontendSession::Handle& f)
    {if(kind==FrontendPointerCallback::Enter||kind==FrontendPointerCallback::Leave||kind==FrontendPointerCallback::Press){CheckTitle(pending.size()<8,"Title callback budget exceeded");pending.push_back({kind,index,f});}}
    void Play(const std::vector<std::uint32_t>& cues)
    {
        const auto live=audio->Handles();std::erase_if(sounds,[&](auto h){return std::find(live.begin(),live.end(),h)==live.end();});sounds.reserve(sounds.size()+cues.size());
        for(auto cue:cues){const auto h=audio->Play(cue,seed);CheckTitle(bool(h),"Title cue was not admitted by the real resident bank");sounds.push_back(*h);}
    }
    void CancelSounds()
    {const auto live=audio->Handles();for(auto h:sounds)if(std::find(live.begin(),live.end(),h)!=live.end())audio->Cancel(h);sounds.clear();}
    template<class Play>void Press(Step& step,unsigned index,const FrontendSession::Handle& source,Play play)
    {
        FrontendTitlePressPrefix(step,int(index),play,[&](bool v){step.Command(FrontendTitleCommandKind::PointerEnabled,v);},[&]{step.Command(FrontendTitleCommandKind::PopScene);},
            []{FrontendTitleGameplaySettingsUpdated();},[]{FrontendTitleCheatSettingsUpdated();},[&](int value){step.Command(FrontendTitleCommandKind::Dimming,unsigned(value));});
        // This explicit native device has no WPAD info endpoint: execute the
        // original no-device/error branch. It does not fabricate battery data.
        FrontendTitleStartMovie([&](int i,const char* slide){step.Pointer(i,slide);},[&]{step.Command(FrontendTitleCommandKind::TransitionTitleToMain);},play);
        step.value.status.departure=FrontendTitleCommandKind::TransitionTitleToMain;step.value.status.source=source;
    }
};
FrontendTitle::FrontendTitle(std::shared_ptr<FrontendSession> s,FrontendInput& i,std::shared_ptr<FrontendAudio> a,std::shared_ptr<FrontendMusic> m,unsigned& seed,FrontendTitleOptions options)
    :FrontendTitle(std::move(s),i,std::move(a),std::move(m),seed,{},options){}
FrontendTitle::FrontendTitle(std::shared_ptr<FrontendSession> session,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,std::shared_ptr<FrontendMusic> music,unsigned& seed,std::shared_ptr<FrontendHandler> base,FrontendTitleOptions options)
    :impl_(std::make_unique<Implementation>(std::move(session),input,std::move(audio),std::move(music),seed,options))
{
    auto& s=*impl_;CheckTitle(!base||base->Binds(s.session),"Title handler belongs to another session");s.handler=base?std::move(base):std::make_shared<FrontendHandler>(s.session,input);
    State next;std::vector<std::uint32_t> cues;bool select_music=false,admitted_music=false;
    try
    {
        s.session->HandlerTransaction(s.current,[&](auto& playback){
            Step step(playback,{},options.movement);step.Command(FrontendTitleCommandKind::Dimming,2);
            FrontendTitleCreated<Step::Instance,Step::Finder,Step::Slide>(step,[&]{return options.widescreen;},[&](int i,const char* name){step.Pointer(i,name);},
                [&](int index){CheckTitle(index==0,"Title music index differs");select_music=true;step.AudioOperation(FrontendTitleOperationKind::SelectMusic,0);},[&](bool v){step.Command(FrontendTitleCommandKind::PointerEnabled,v);},
                [&]{step.Command(FrontendTitleCommandKind::ResetNavigation);},[&](unsigned long cue,const char*,void*,bool){cues.push_back(std::uint32_t(cue));step.AudioOperation(FrontendTitleOperationKind::PlayCue,std::uint32_t(cue));});next=step.Result();
        },[&]{if(!s.options.deferred_services){if(select_music){admitted_music=true;s.music->BeginSelect(0,s.seed);}s.Play(cues);next.status.admitted_operations=next.status.operations.size();}});
        s.state=std::move(next);s.current=s.session->Current();
    }
    catch(...){if(admitted_music)s.music->CancelPending();s.CancelSounds();throw;}
}
FrontendTitle::~FrontendTitle(){try{Release();}catch(...){std::terminate();}}
FrontendSession::Handle FrontendTitle::Current()const{impl_->Ready();return impl_->current;}
FrontendTitleStatus FrontendTitle::Status()const{auto& s=*impl_;CheckTitle(s.thread==std::this_thread::get_id()&&s.session,"Title status requires its owner thread");auto out=s.state.status;out.failed=s.failed;return out;}
bool FrontendTitle::AdmitOperations(const std::function<bool(FrontendTitleCommand,const FrontendSession::Handle&)>& command)
{
    auto& s=*impl_;s.Ready();
    CheckTitle(s.options.deferred_services&&command&&!s.busy&&!s.stack_update&&!nlGetCurrentAsyncRead(),"Title service admission requires its idle deferred owner");
    CheckTitle(s.current==s.session->Current(),"Title service admission lost its resource generation");
    Busy guard(s.busy);
    try
    {
        auto& status=s.state.status;const auto source=status.source?status.source:s.current;
        while(status.admitted_operations<status.operations.size())
        {
            const auto operation=status.operations[status.admitted_operations];
            switch(operation.kind)
            {
            case FrontendTitleOperationKind::Command:if(!command(operation.command,source))return false;break;
            case FrontendTitleOperationKind::PlayCue:s.Play({operation.argument});break;
            case FrontendTitleOperationKind::SelectMusic:CheckTitle(operation.argument==0,"Title stream index differs");s.music->BeginSelect(0,s.seed);break;
            case FrontendTitleOperationKind::StopMusic:s.music->Stop();break;
            }
            ++status.admitted_operations;
        }
        return true;
    }
    catch(...){s.failed=true;throw;}
}
FrontendPointerBounds FrontendTitle::Bounds()const{impl_->Ready();CheckTitle(bool(impl_->region),"Title hit bounds require presented initialized frame");return impl_->region->Bounds();}
std::vector<FrontendAudioHandle> FrontendTitle::TransferAudioOwnership()
{
    auto& s=*impl_;s.Ready();CheckTitle(s.options.deferred_services&&!s.busy&&!s.stack_update&&!nlGetCurrentAsyncRead(),"Title audio transfer requires its idle integrated owner");
    auto result=std::move(s.sounds);s.sounds.clear();return result;
}
void FrontendTitle::Acknowledge(const FrontendSession::Handle& frame,FrontendPointerViewport viewport)
{
    auto& s=*impl_;s.Expected(frame,true);
    try
    {
        if(s.state.status.initialized)
        {
            if(s.region)s.region->RebindFrame(frame);else
            {
                CheckTitle(s.state.measured_bounds.has_value(), "Title source did not measure its pointer at initialization");
                s.region=std::make_shared<FrontendPointerRegion>(s.input,frame,s.state.instance,*s.state.measured_bounds,
                    [&s](auto kind,unsigned index,const auto& source){s.Queue(kind,index,source);});
            }
            const std::array regions{s.region};s.host.Publish(frame,viewport,regions);
        }
        else s.host.Publish(frame,viewport,{});
    }
    catch(...){s.failed=true;throw;}
}
void FrontendTitle::ApplyPending()
{
    auto& s=*impl_;s.Expected(s.current);if(s.pending.empty())return;const auto frame=s.current,shown=s.Presented();Busy guard(s.busy);
    try
    {
        auto events=std::move(s.pending);s.pending={};s.pending.reserve(8);State next;std::vector<std::uint32_t> cues;
        s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state,s.options.movement);if(!(s.options.deferred_services&&s.stack_update&&s.PendingOperations()))step.NewOperations();
            const auto play=[&](unsigned long cue,const char*,void*,bool){cues.push_back(std::uint32_t(cue));step.AudioOperation(FrontendTitleOperationKind::PlayCue,std::uint32_t(cue));};
            for(const auto& e:events){CheckTitle(e.frame==shown,"Title pointer callback is stale");if(step.value.status.departure)break;
                if(e.kind==FrontendPointerCallback::Enter)FrontendTitleEnter(step,int(e.index),play);
                else if(e.kind==FrontendPointerCallback::Leave)FrontendTitleLeave(step,int(e.index));else s.Press(step,e.index,shown,play);}
            next=step.Result();},[&]{if(!s.options.deferred_services){s.Play(cues);next.status.admitted_operations=next.status.operations.size();}});s.state=std::move(next);s.current=s.session->Current();
    }
    catch(...){s.failed=true;throw;}
}
void FrontendTitle::DeliverPointer(const FrontendSession::Handle& frame,const FrontendPointerEvent& event)
{
    auto& s=*impl_;s.Expected(s.current);CheckTitle(frame&&frame==s.Presented()&&s.host.Current()&&s.host.Current()->Frame()==frame&&event.index<4,"Title pointer requires actual acknowledged frame");if(!s.CanRoute())return;
    try{s.region->Deliver(frame,event);ApplyPending();}catch(...){s.failed=true;throw;}
}
FrontendPointerDispatch FrontendTitle::Route(const FrontendPointerDesktopSample& sample)
{auto& s=*impl_;s.Expected(s.current);CheckTitle(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Title route requires acknowledged frame");if(!s.CanRoute())return {};try{auto event=s.host.Route(s.host.Current(),sample);ApplyPending();return event;}catch(...){s.failed=true;throw;}}
FrontendPointerDispatch FrontendTitle::Poll(SDL_Window* window,bool capture)
{auto& s=*impl_;s.Expected(s.current);CheckTitle(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Title poll requires acknowledged frame");if(!s.CanRoute())return {};try{auto event=s.host.Poll(s.host.Current(),window,capture);ApplyPending();return event;}catch(...){s.failed=true;throw;}}
void FrontendTitle::AdvanceVisual(const FrontendSession::Handle& frame,float delta)
{auto& s=*impl_;CheckTitle(!s.stack_attached,"Stack owns Title base update");s.Expected(frame);if(s.state.status.departure||s.PendingOperations())return;AfterBaseUpdate(s.handler->UpdateOnce(frame,delta));}
void FrontendTitle::AfterBaseUpdate(FrontendHandler::UpdateProof&& proof)
{
    auto& s=*impl_;s.Mutable();const auto shown=s.input_source?s.input_source:s.current;const auto dt=proof.Delta();s.current=s.handler->ConsumeUpdate(std::move(proof),s.current);if(s.state.status.departure)return;
    Busy guard(s.busy);
    try
    {
        State next;std::vector<std::uint32_t> cues;bool stop_music=false;
        // FE query focus is established once, before entering the clone: the
        // handler validates published snapshots and cannot query an uncommitted clone.
        s.input.Focus(s.handler.get());
        s.session->HandlerTransaction(s.current,[&](auto& playback){
            Step step(playback,s.state,s.options.movement);step.NewOperations();
            CheckTitle(std::isfinite(step.m_fTimeElapsed+dt),"Title clock overflow");
            bool base_seen=false;const auto play=[&](unsigned long cue,const char*,void*,bool){cues.push_back(std::uint32_t(cue));step.AudioOperation(FrontendTitleOperationKind::PlayCue,std::uint32_t(cue));};
            if(FrontendTitleBeginUpdate(step,dt,[&](float d){CheckTitle(!base_seen&&d==dt,"Title base proof differs");base_seen=true;},[&]{FrontendTitleBind(step);step.value.measured_bounds=MeasureFrontendPointerBounds(s.current,step.mControllerComponent.binding);},[&](int i,const char* name){step.Pointer(i,name);}))
            {
                // Explicit retail desktop profile: soak/smoke are not enabled.
                for(int pad=0;pad<4;++pad)
                {
                    if(unsigned(pad)!=s.options.controller){step.Pointer(pad,"waiting");continue;}
                    if(FrontendTitleIdle(step,[&]{stop_music=true;step.AudioOperation(FrontendTitleOperationKind::StopMusic);},[&](bool v){step.Command(FrontendTitleCommandKind::PointerEnabled,v);},[&]{step.Command(FrontendTitleCommandKind::IntroMovie,22);step.value.status.departure=FrontendTitleCommandKind::IntroMovie;step.value.status.source=shown;}))break;
                    FrontendTitlePadInput(step,pad,[&]{step.Pointer(pad,"A");},[&](int p,int action,bool remap,void* found){CheckTitle(remap&&!found,"Title FE query profile differs");return s.input.Button(static_cast<FrontendAction>(action),FrontendButtonQuery::Pressed,p);},[&](int p){s.Press(step,unsigned(p),shown,play);});
                    // Desktop backend class is -1, so the original Wii type2
                    // acceleration/unlock branch is not executed or synthesized.
                }
            }
            CheckTitle(base_seen,"Title source skipped base update");next=step.Result();
        },[&]{if(!s.options.deferred_services){if(stop_music)s.music->Stop();s.Play(cues);next.status.admitted_operations=next.status.operations.size();}});
        s.state=std::move(next);s.current=s.session->Current();
    }
    catch(...){s.failed=true;throw;}
}
std::shared_ptr<FrontendSession> FrontendTitle::StackSession()const{impl_->Ready();return impl_->session;}
std::shared_ptr<FrontendHandler> FrontendTitle::StackHandler()const{impl_->Ready();return impl_->handler;}
unsigned FrontendTitle::StackScene()const{return 0;}
bool FrontendTitle::CanUpdateStack() const
{
    auto& s=*impl_;s.Ready();
    CheckTitle(s.current&&s.current==s.session->Current(), "Title update requires exact source frame");
    // A typed departure has reached the selected owner boundary. The actual
    // coordinator must admit the VM/scene service before this owner advances.
    return !s.state.status.departure.has_value()&&!s.PendingOperations();
}
void FrontendTitle::AttachStack(){auto& s=*impl_;s.Mutable();CheckTitle(!s.stack_attached,"Title already belongs to stack");s.stack_attached=true;}
void FrontendTitle::UpdateStack(FrontendHandler::UpdateProof&& proof,const FrontendSession::Handle& shown,const std::function<void()>& input)
{
    auto& s=*impl_;s.Ready();CheckTitle(s.stack_attached&&!s.stack_update&&!s.busy&&!nlGetCurrentAsyncRead(),"Title stack update requires idle owner");
    CheckTitle(shown&&shown==s.current&&proof.Before()==shown&&proof.After()==s.session->Current(),"Title stack proof mismatch");
    s.stack_update=true;s.input_source=shown;struct Reset{Implementation& s;~Reset(){s.input_window=false;s.input_source.reset();s.stack_update=false;}}reset{s};
    try{AfterBaseUpdate(std::move(proof));s.input_window=true;if(input)input();CheckTitle(s.current==s.session->Current(),"Title input mutated session externally");}catch(...){s.failed=true;throw;}
}
void FrontendTitle::ReleaseStack(){auto& s=*impl_;CheckTitle(!s.stack_update&&!s.busy,"Cannot remove updating Title");s.stack_attached=false;Release();}
void FrontendTitle::Release()
{
    auto& s=*impl_;CheckTitle(s.thread==std::this_thread::get_id()&&!s.busy&&!nlGetCurrentAsyncRead(),"Title release requires idle owner thread");if(!s.session)return;CheckTitle(!s.stack_attached,"Stack owns Title release");Busy guard(s.busy);
    s.host.Release();s.region.reset();s.pending.clear();if(s.handler&&s.handler.use_count()==1)s.handler->Release();s.handler.reset();s.CancelSounds();s.current.reset();s.input_source.reset();s.state={};s.session.reset();s.audio.reset();s.music.reset();
}
}
