#include "runtime/frontend_visual_options.h"
#include "runtime/frontend_handler.h"
#include "resources/frontend_animation.h"
#include "Game/FE/FrontendVisualOptionsSteps.h"
#include "NL/nlFormat.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <thread>
namespace Detail
{
template<>struct LexicalCastImpl<std::u16string,std::u16string>
{static std::u16string Do(const std::u16string& value){return value;}};
}
namespace mscharged
{
namespace
{
using namespace resources;
void Check(bool good,const char* text){if(!good)throw std::logic_error(text);}
struct Position{struct{float x,y,z;}f;};
struct State
{
    FrontendVisualOptionsStatus status;
    std::array<std::uint32_t,5> buttons{};
    std::array<std::uint32_t,2> modes{};
    std::array<FrontendPointerBinding,7> bindings{};
};
struct Step
{
    FrontendAnimationPlayback& playback;const Localization& localization;State value;
    int mOverlayMode=0,mState=value.status.state;
    bool mSaveStarted=value.status.native_save_admitted;
    std::array<int,2> mSettings=value.status.settings,mBackupSettings=value.status.backup;
    struct Instance;
    struct Slide
    {
        Step& owner;std::uint32_t id;
        FrontendNode Root()const{return{FrontendNodeKind::Slide,id};}
        const FrontendSlide& Value()const
        {const auto& all=owner.playback.Scene().slides;const auto it=std::find_if(all.begin(),all.end(),[&](const auto& s){return s.offset==id;});Check(it!=all.end(),"Visual options slide is absent");return *it;}
        float GetCurrentTime()const{return Value().time;}float GetStartTime()const{return Value().start;}float GetDuration()const{return Value().duration;}
    };
    struct Instance
    {
        Step& owner;std::uint32_t id;
        struct Visibility{Step& owner;std::uint32_t id;void operator=(bool v){FrontendInstanceChange c;c.instance=id;c.property=FrontendInstanceProperty::Visible;c.flag=v;owner.playback.Apply({&c,1});}}m_bVisible{owner,id};
        FrontendNode Root()const{return{FrontendNodeKind::Instance,id};}
        const FrontendInstance& Value()const
        {const auto& all=owner.playback.Scene().instances;const auto it=std::find_if(all.begin(),all.end(),[&](const auto& v){return v.offset==id;});Check(it!=all.end(),"Visual options instance is absent");return *it;}
        Position GetAssetPosition()const{const auto& p=Value().attributes.position;return{{p[0],p[1],p[2]}};}
        Slide* GetActiveSlide()
        {
            const auto& v=Value();Check(v.type==4&&v.library,"Visual options component is required");const auto& all=owner.playback.Scene().library;
            const auto it=std::find_if(all.begin(),all.end(),[&](const auto& l){return l.offset==*v.library;});Check(it!=all.end()&&it->active_slide,"Visual options component has no active slide");return owner.AtSlide(*it->active_slide);
        }
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {const auto& v=Value();Check(v.type==4&&v.library&&owner.playback.SelectComponent(*v.library,name,reset,preserve),"Visual options feedback slide is absent");}
        void SetAssetColour(const std::array<std::uint8_t,4>& colour)
        {FrontendInstanceChange c;c.instance=id;c.property=FrontendInstanceProperty::Colour;c.colour=colour;owner.playback.Apply({&c,1});}
        void SetString(std::u16string text)
        {FrontendInstanceChange c;c.instance=id;c.property=FrontendInstanceProperty::String;c.text=std::move(text);owner.playback.Apply({&c,1});}
    };
    struct Presentation
    {
        Step& owner;Slide* m_currentSlide=nullptr;
        FrontendNode Root()const{return{FrontendNodeKind::Presentation,0};}
        void SetActiveSlide(const char* name,bool reset){Check(owner.playback.SelectPresentation(name,reset),"Visual options presentation is absent");m_currentSlide=owner.ActiveSlide();}
        void Update(float delta){owner.playback.Advance(delta);m_currentSlide=owner.ActiveSlide();}
    } presentation{*this};Presentation* mPresentation=&presentation;
    struct Button
    {
        std::array<int,4> states{};FrontendPointerBinding binding;Step* owner=nullptr;
        bool HasOtherPointerState(int state,int except)const{for(unsigned i=0;i<4;++i)if(int(i)!=except&&states[i]==state)return true;return false;}
        void SetPointerState(int state,int index){Check(index>=0&&index<4,"Visual options pointer index is invalid");states[index]=state;}
        void ResetPointerStates(){states={};}
        void PlayHoverFeedback(int index){Check(owner&&index>=0&&index<4,"Visual options hover pointer is invalid");owner->Command(FrontendVisualOptionsCommandKind::HoverRumble,index);} // Explicit unprovided haptic request.
        void SetInstanceBounds(Instance* instance,bool rotate,float x,float y,float sx,float sy)
        {Check(instance,"Visual options pointer instance is absent");binding={instance->id,rotate,x,y,sx,sy};}
    };

    std::array<Instance*,5> mButtons{};std::array<Instance*,2> mZoomButtons{};
    std::array<Button,5> mButtonComponents{};std::array<Button,2> mZoomButtonComponents{};
    std::map<std::uint32_t,Instance> instances;std::map<std::uint32_t,Slide> slides;
    Step(FrontendAnimationPlayback& p,const Localization& l,State s):playback(p),localization(l),value(std::move(s))
    {
        for(unsigned i=0;i<5;++i){if(value.buttons[i])mButtons[i]=At(value.buttons[i]);mButtonComponents[i]={value.status.pointer_states[i],value.bindings[i],this};}
        for(unsigned i=0;i<2;++i){if(value.modes[i])mZoomButtons[i]=At(value.modes[i]);mZoomButtonComponents[i]={value.status.pointer_states[5+i],value.bindings[5+i],this};}
        presentation.m_currentSlide=ActiveSlide();
    }
    Instance* At(std::uint32_t id){return &instances.try_emplace(id,Instance{*this,id}).first->second;}
    Slide* AtSlide(std::uint32_t id){return &slides.try_emplace(id,Slide{*this,id}).first->second;}
    Slide* ActiveSlide(){const auto id=playback.Scene().active_slide;Check(bool(id),"Visual options has no active presentation");return AtSlide(*id);}
    template<class T,int N>struct Finder
    {
        template<class Root,class... Names>static T* Find(Root* root,Names... names)
        {
            const std::array<std::string_view,sizeof...(Names)> path{names...};
            const auto found=FindFrontendNode(root->owner.playback.Scene(),root->Root(),FrontendNamedPath(path));
            Check(found&&found->kind==FrontendNodeKind::Instance,"Visual options authored lookup path is absent");return root->owner.At(found->id);
        }
    };

    void FormatText(Instance* text,int level)
    {
        Check(text->Value().type==3,"Visual options label is not text");
        const auto n=std::to_string(level);std::u16string number(n.begin(),n.end());
        auto text_value=Format(std::u16string(localization.Get(FrontendLowerHash("OPTIONS_VISUAL_ZOOMLEVEL"))),number);
        Check(text_value.size()<16,"Visual options formatted label exceeds original16-unit storage");
        text->SetString(std::move(text_value));
    }
    void FormatText(int level){FormatText(Finder<Instance,4>::Find(presentation.m_currentSlide,"Layer","visual_options","SERIES SETTING"),level);}
    void Command(FrontendVisualOptionsCommandKind kind,unsigned argument=0)
    {Check(value.status.commands.size()<32,"Visual options command budget exceeded");value.status.commands.push_back({kind,argument});}
    struct Navigation
    {
        Step& s;
        void SetPopScene(bool value){Check(!value,"Visual options requires source false-pop navigation");}
        void SetButtons(unsigned mask,bool enabled){Check(enabled&&(mask==0||mask==0x24),"Visual options NAV request is unsupported");s.Command(mask?FrontendVisualOptionsCommandKind::ShowBackAndDone:FrontendVisualOptionsCommandKind::ResetNavigation,mask);}
        void SetDoneButtonText(int value){Check(value==1,"Visual options done text differs");s.Command(FrontendVisualOptionsCommandKind::DoneText,1);}
    } navigation{*this};Navigation& mNavigation=navigation;
    struct SaveButton
    {
        Step& s;void SetActiveSlide(const char* name,bool reset,bool preserve)
        {Check(std::string_view(name)=="down"&&reset&&!preserve,"Visual options save feedback differs");s.Command(FrontendVisualOptionsCommandKind::DoneDown);}
    } save_button{*this};SaveButton* mSaveButton=&save_button;
    State Result()
    {
        value.status.settings=mSettings;value.status.backup=mBackupSettings;value.status.state=mState;value.status.native_save_admitted=mSaveStarted;
        for(unsigned i=0;i<5;++i){Check(mButtons[i]&&mButtons[i]->Value().type==4,"Visual level button is not a component");value.buttons[i]=mButtons[i]->id;value.status.pointer_states[i]=mButtonComponents[i].states;value.bindings[i]=mButtonComponents[i].binding;}
        for(unsigned i=0;i<2;++i){Check(mZoomButtons[i]&&mZoomButtons[i]->Value().type==4,"Visual mode button is not a component");value.modes[i]=mZoomButtons[i]->id;value.status.pointer_states[5+i]=mZoomButtonComponents[i].states;value.bindings[5+i]=mZoomButtonComponents[i].binding;}
        return std::move(value);
    }
};
struct Pending{FrontendPointerCallback kind;unsigned item,index;FrontendSession::Handle frame;};
struct Busy{bool& value;explicit Busy(bool& v):value(v){value=true;}~Busy(){value=false;}};
enum class EffectKind{Cue,Zoom,Auto,NativeSave};
struct Effect{EffectKind kind;std::uint32_t cue=0;float zoom=0;bool automatic=false;};
struct VisualProxy
{
    struct Zoom{std::vector<Effect>& effects;void operator=(float v){effects.push_back({EffectKind::Zoom,0,v,false});}}mCameraZoomLevel;
    struct Auto{std::vector<Effect>& effects;void operator=(bool v){effects.push_back({EffectKind::Auto,0,0,v});}}mIsAutoZoomCamera;
};
}
struct FrontendVisualOptions::Implementation
{
    std::shared_ptr<FrontendSession> session;FrontendInput& input;std::shared_ptr<FrontendAudio> audio;FrontendVisualSettings::Handle settings;
    unsigned& seed;unsigned controller;const std::thread::id thread=std::this_thread::get_id();
    FrontendVisualSettingsSnapshot expected;
    FrontendSession::Handle current;State state;FrontendPointerHost host;std::shared_ptr<FrontendHandler> handler;
    std::array<std::shared_ptr<FrontendPointerRegion>,7> regions{};std::vector<Pending> pending;std::vector<FrontendAudioHandle> sounds;
    std::shared_ptr<NativePreferences> save;
    bool busy=false,failed=false,stack_attached=false,stack_update=false,input_window=false;
    FrontendSession::Handle input_source;
    Implementation(std::shared_ptr<FrontendSession> s,FrontendInput& i,std::shared_ptr<FrontendAudio> a,FrontendVisualSettings::Handle v,unsigned& rng,unsigned c)
        :session(std::move(s)),input(i),audio(std::move(a)),settings(std::move(v)),seed(rng),controller(c),expected(settings?settings->Snapshot():FrontendVisualSettingsSnapshot{false,0,0}),host(i,c)
    {
        Check(session&&controller<4&&audio&&audio->Loaded()&&settings,"Visual options require retained scene/audio and explicit desired settings");
        current=session->Current();Check(current&&current->request.animate&&current->request.image_profile==FrontendImageProfile::Main,"Visual options require animated Main resources");
        const auto it=std::find_if(current->graph.slides.begin(),current->graph.slides.end(),[&](const auto& s){return s.offset==current->graph.active_slide;});
        Check(it!=current->graph.slides.end()&&it->name=="OPTIONS_IN","Visual options require the authored OPTIONS_IN presentation");pending.reserve(32);
    }
    void Ready()const{Check(thread==std::this_thread::get_id()&&session&&!failed,"Visual options require their live nonfailed owning thread");}
    void Mutable()const{Ready();Check(!busy&&!nlGetCurrentAsyncRead()&&(!stack_attached||stack_update),"Visual options mutation requires its idle owner or controlled stack update");}
    void Expected(const FrontendSession::Handle& f,bool acknowledge=false)const
    {
        if(acknowledge){Ready();Check(!busy&&!stack_update&&!nlGetCurrentAsyncRead(),"Cannot acknowledge Visual options during update");}
        else Mutable();
        Check(f&&f==current&&f==session->Current(),"Visual options require their exact current frame");
        Check(settings->Snapshot()==expected,"Visual options settings changed outside this owner");
    }
    FrontendSession::Handle Presented()const{return input_window?input_source:current;}
    void InputExpected(const FrontendSession::Handle& frame)const
    {Expected(current);Check(frame&&frame==Presented(),"Visual input requires its last acknowledged stack frame");}
    bool Interactive()const{return state.status.state==1&&state.status.initialized&&!input.InputLocked();}
    bool RegionsPresented()const{return std::all_of(regions.begin(),regions.end(),[&](const auto& region){return region&&region->Current()==Presented();});}
    void Queue(FrontendPointerCallback kind,unsigned item,unsigned index,const FrontendSession::Handle& f)
    {if(kind==FrontendPointerCallback::Enter||kind==FrontendPointerCallback::Leave||kind==FrontendPointerCallback::Press){Check(pending.size()<32,"Visual options pointer budget exceeded");pending.push_back({kind,item,index,f});}}
    void Play(std::uint32_t cue)
    {const auto live=audio->Handles();std::erase_if(sounds,[&](auto h){return std::find(live.begin(),live.end(),h)==live.end();});sounds.reserve(sounds.size()+1);auto h=audio->Play(cue,seed);Check(bool(h),"Visual options authored cue is unavailable");sounds.push_back(*h);}
    void Effects(const std::vector<Effect>& effects,const std::shared_ptr<NativePreferences>& preferences={})
    {
        for(const auto& e:effects)
        {
            if(e.kind==EffectKind::Cue)Play(e.cue);
            else if(e.kind==EffectKind::NativeSave)
            {
                Check(bool(preferences),"Visual options require actual native persistence");auto values=*preferences->Current();values.auto_zoom=expected.auto_zoom;values.camera_zoom=expected.zoom;preferences->StartSave(values);
            }
            else{settings->Set(expected,e.kind==EffectKind::Auto?e.automatic:expected.auto_zoom,e.kind==EffectKind::Zoom?e.zoom:expected.zoom);expected=settings->Snapshot();}
        }
    }
};
FrontendVisualOptions::FrontendVisualOptions(std::shared_ptr<FrontendSession> session,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,FrontendVisualSettings::Handle settings,unsigned& seed,unsigned controller)
    :FrontendVisualOptions(std::move(session),input,std::move(audio),std::move(settings),seed,{},controller){}
FrontendVisualOptions::FrontendVisualOptions(std::shared_ptr<FrontendSession> session,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,FrontendVisualSettings::Handle settings,unsigned& seed,std::shared_ptr<FrontendHandler> base,unsigned controller)
    :impl_(std::make_unique<Implementation>(std::move(session),input,std::move(audio),std::move(settings),seed,controller))
{
    auto& s=*impl_;Check(!base||base->Binds(s.session),"Visual options shared handler belongs to another session");s.handler=base?std::move(base):std::make_shared<FrontendHandler>(s.session,input);State next;
    s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);
        struct Visual{bool mIsAutoZoomCamera;float mCameraZoomLevel;}values{s.expected.auto_zoom,s.expected.zoom};FrontendVisualOptionsInitialize(step,values);
        step.Command(FrontendVisualOptionsCommandKind::HideNavigation);step.Command(FrontendVisualOptionsCommandKind::BindBack,4);step.Command(FrontendVisualOptionsCommandKind::BindDone,0x20);
        FrontendVisualOptionsCreated<Step::Instance,Step::Instance,Step::Instance,Step::Finder>(step,[&](int i){step.Command(FrontendVisualOptionsCommandKind::PointerWaiting,i);},[](char* name,int size,int i){std::snprintf(name,size,"BUTTON_%d",i);},[&](auto* text,int level){step.FormatText(text,level);});next=step.Result();});
    s.state=std::move(next);s.current=s.session->Current();
}
FrontendVisualOptions::~FrontendVisualOptions(){try{Release();}catch(...){std::terminate();}}
FrontendSession::Handle FrontendVisualOptions::Current()const{impl_->Ready();return impl_->current;}
FrontendVisualOptionsStatus FrontendVisualOptions::Status()const
{auto& s=*impl_;Check(s.thread==std::this_thread::get_id()&&s.session,"Visual options status requires its owning thread");auto result=s.state.status;result.failed=s.failed;return result;}
void FrontendVisualOptions::AdvanceVisual(const FrontendSession::Handle& frame,float delta)
{
    auto& s=*impl_;Check(!s.stack_attached,"Stack owns the only Visual options base update");s.Expected(frame);if(s.input.InputLocked())return;Check(s.pending.empty(),"Visual options pointer events are pending");
    AfterBaseUpdate(s.handler->UpdateOnce(frame,delta));
}
void FrontendVisualOptions::AfterBaseUpdate(FrontendHandler::UpdateProof&& proof)
{
    auto& s=*impl_;s.Mutable();s.current=s.handler->ConsumeUpdate(std::move(proof),s.current);
    try
    {
        State next;
        s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();
            const bool ready=FrontendVisualOptionsGate(step,[&]{return &step.navigation;},[&](int i){step.Command(FrontendVisualOptionsCommandKind::PointerWaiting,i);},[&](int id){Check(id==13,"Visual options transition scene differs");step.Command(FrontendVisualOptionsCommandKind::PushOptions,id);},[](int){throw UnsupportedResource("Overlay visual options are unavailable");});
            if(ready){if(!step.value.status.initialized){FrontendVisualOptionsBind<Step::Instance,Position,Step::Finder>(step);step.value.status.initialized=true;}for(unsigned i=0;i<4;++i)step.Command(i==s.controller?FrontendVisualOptionsCommandKind::PointerCursor:FrontendVisualOptionsCommandKind::PointerWaiting,i);}
            next=step.Result();});s.state=std::move(next);s.current=s.session->Current();
    }catch(...){s.failed=true;throw;}
}
void FrontendVisualOptions::Acknowledge(const FrontendSession::Handle& frame,FrontendPointerViewport viewport)
{
    auto& s=*impl_;s.Expected(frame,true);
    try
    {
        if(s.state.status.initialized)
        {for(unsigned i=0;i<7;++i){if(s.regions[i])s.regions[i]->RebindFrame(frame);else s.regions[i]=std::make_shared<FrontendPointerRegion>(s.input,frame,s.state.bindings[i],[&s,i](auto kind,unsigned index,const auto& f){s.Queue(kind,i,index,f);});}s.host.Publish(frame,viewport,s.regions);}
        else s.host.Publish(frame,viewport,{});
    }catch(...){s.failed=true;throw;}
}
std::array<FrontendPointerBounds,7> FrontendVisualOptions::Bounds()const
{impl_->Ready();std::array<FrontendPointerBounds,7> out;for(unsigned i=0;i<7;++i){Check(bool(impl_->regions[i]),"Visual options bounds await presentation");out[i]=impl_->regions[i]->Bounds();}return out;}
void FrontendVisualOptions::ApplyPending()
{
    auto& s=*impl_;s.Expected(s.current);if(s.pending.empty())return;const auto presented=s.Presented();Busy guard(s.busy);
    try
    {
        auto events=std::move(s.pending);s.pending.clear();s.pending.reserve(32);State next;std::vector<Effect> effects;
        s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();VisualProxy visual{effects,effects};
            auto play=[&](unsigned long cue,const void* a,void* b,bool restart){Check(cue<=UINT32_MAX&&!a&&!b&&restart,"Visual options cue contract differs");effects.push_back({EffectKind::Cue,std::uint32_t(cue)});};
            for(const auto& event:events)
            {
                Check(event.frame==presented&&event.item<7&&event.index<4,"Visual options event is stale or invalid");if(step.mState!=1)break;
                if(event.item<5)
                {
                    if(event.kind==FrontendPointerCallback::Enter)FrontendVisualOptionsLevelEnter(step,event.index,event.item,play);
                    else if(event.kind==FrontendPointerCallback::Leave)FrontendVisualOptionsLevelLeave(step,event.index,event.item);
                    else FrontendVisualOptionsLevelPress(step,event.index,event.item,[&]() -> VisualProxy& {return visual;},play,[&](int n){step.FormatText(n);});
                }
                else
                {
                    const auto item=event.item-5;
                    if(event.kind==FrontendPointerCallback::Enter)FrontendVisualOptionsModeEnter(step,event.index,item,play);
                    else if(event.kind==FrontendPointerCallback::Leave)FrontendVisualOptionsModeLeave(step,event.index,item);
                    else FrontendVisualOptionsModePress(step,event.index,item,[&]() -> VisualProxy& {return visual;},play,[&](int n){step.FormatText(n);});
                }
            }
            next=step.Result();},[&]{s.Effects(effects);});s.state=std::move(next);s.current=s.session->Current();
    }catch(...){s.failed=true;throw;}
}
void FrontendVisualOptions::DeliverPointer(const FrontendSession::Handle& frame,const FrontendPointerEvent& event)
{
    auto& s=*impl_;s.InputExpected(frame);Check(s.host.Current()&&s.host.Current()->Frame()==frame&&event.index<4,"Visual options input requires actual presentation");if(!s.Interactive()||!s.RegionsPresented())return;
    try{for(auto& region:s.regions)region->Deliver(frame,event);ApplyPending();}catch(...){s.failed=true;throw;}
}
FrontendPointerDispatch FrontendVisualOptions::Route(const FrontendPointerDesktopSample& sample)
{auto& s=*impl_;s.Expected(s.current);Check(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Visual options input requires actual presentation");if(!s.Interactive()||!s.RegionsPresented())return{};try{auto out=s.host.Route(s.host.Current(),sample);ApplyPending();return out;}catch(...){s.failed=true;throw;}}
FrontendPointerDispatch FrontendVisualOptions::Poll(SDL_Window* window,bool capture)
{auto& s=*impl_;s.Expected(s.current);Check(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Visual options input requires actual presentation");if(!s.Interactive()||!s.RegionsPresented())return{};try{auto out=s.host.Poll(s.host.Current(),window,capture);ApplyPending();return out;}catch(...){s.failed=true;throw;}}
void FrontendVisualOptions::NotifyBackButton(const FrontendSession::Handle& frame)
{
    auto& s=*impl_;s.InputExpected(frame);Check(s.Interactive()&&s.RegionsPresented()&&s.host.Current()&&s.host.Current()->Frame()==frame,"Visual options back requires its genuinely presented interactive frame");Busy guard(s.busy);State next;std::vector<Effect> effects;
    try{s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();VisualProxy visual{effects,effects};
        FrontendVisualOptionsBack(step,[&]() -> VisualProxy& {return visual;},[&](unsigned long cue,const void* a,void* b,bool restart){Check(cue<=UINT32_MAX&&!a&&!b&&restart,"Visual options back cue contract differs");effects.push_back({EffectKind::Cue,std::uint32_t(cue)});},[&]{return &step.navigation;});next=step.Result();
        },[&]{s.Effects(effects);});s.state=std::move(next);s.current=s.session->Current();}
    catch(...){s.failed=true;throw;}
}
void FrontendVisualOptions::Save(const FrontendSession::Handle& frame)
{impl_->InputExpected(frame);throw UnsupportedResource("Original visual options SaveLoad::StartSave(false) requires a full game-save service");}
void FrontendVisualOptions::SaveNativePreferences(const FrontendSession::Handle& frame,std::shared_ptr<NativePreferences> preferences)
{
    auto& s=*impl_;s.InputExpected(frame);Check(s.Interactive()&&s.RegionsPresented()&&s.host.Current()&&s.host.Current()->Frame()==frame&&!s.state.status.native_save_admitted&&preferences,"Visual options native save requires its presented interactive frame and retained service");
    const auto status=preferences->Status();Check(status.save_enabled&&!status.host_pending&&(status.state==NativePreferencesState::Ready||status.state==NativePreferencesState::Missing),"Native preferences must finish loading before save admission");
    Busy guard(s.busy);State next;std::vector<Effect> effects;
    try{s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();
        FrontendVisualOptionsSave(step,[&]{return &step.navigation;},[&](unsigned long cue,const void* a,void* b,bool restart){Check(cue<=UINT32_MAX&&!a&&!b&&restart,"Visual options save cue contract differs");effects.push_back({EffectKind::Cue,std::uint32_t(cue)});},[&](bool online){Check(!online,"Visual options requested online save");effects.push_back({EffectKind::NativeSave});});next=step.Result();
        },[&]{s.Effects(effects,preferences);});s.save=std::move(preferences);s.state=std::move(next);s.current=s.session->Current();}
    catch(...){s.failed=true;throw;}
}
std::shared_ptr<FrontendSession> FrontendVisualOptions::StackSession()const{impl_->Ready();return impl_->session;}
std::shared_ptr<FrontendHandler> FrontendVisualOptions::StackHandler()const{impl_->Ready();return impl_->handler;}
unsigned FrontendVisualOptions::StackScene()const{return 15;}
bool FrontendVisualOptions::CanUpdateStack()const
{
    auto& s=*impl_;s.Ready();Check(s.stack_attached&&!s.stack_update&&!s.busy&&!nlGetCurrentAsyncRead(),"Visual pre-base admission requires its idle retained stack owner");
    Check(s.settings->Snapshot()==s.expected,"Visual options settings changed outside this owner");
    return !s.input.InputLocked();
}
void FrontendVisualOptions::AttachStack()
{auto& s=*impl_;s.Mutable();Check(!s.stack_attached,"Visual owner already belongs to a stack");s.stack_attached=true;}
void FrontendVisualOptions::UpdateStack(FrontendHandler::UpdateProof&& proof,const FrontendSession::Handle& presented,const std::function<void()>& input)
{
    auto& s=*impl_;s.Ready();Check(s.stack_attached&&!s.stack_update&&!s.busy&&!nlGetCurrentAsyncRead(),"Visual stack update requires its idle retained owner");
    Check(presented&&presented==s.current&&proof.Before()==presented&&proof.After()==s.session->Current(),"Visual stack proof/presentation differs");
    s.stack_update=true;s.input_source=presented;
    struct Reset{Implementation& s;~Reset(){s.input_window=false;s.input_source.reset();s.stack_update=false;}}reset{s};
    try{AfterBaseUpdate(std::move(proof));s.input_window=true;if(input)input();Check(s.current==s.session->Current(),"Stack input mutated outside its selected Visual owner");}
    catch(...){s.failed=true;throw;}
}
void FrontendVisualOptions::ReleaseStack()
{auto& s=*impl_;Check(!s.stack_update&&!s.busy,"Cannot remove an active Visual update");s.stack_attached=false;Release();}
void FrontendVisualOptions::Release()
{
    auto& s=*impl_;Check(s.thread==std::this_thread::get_id()&&!s.busy&&!nlGetCurrentAsyncRead(),"Visual options teardown requires its idle owning thread");if(!s.session)return;Check(!s.stack_attached,"Stack owns selected Visual teardown");Busy guard(s.busy);
    std::exception_ptr failure;try{s.host.Release();}catch(...){failure=std::current_exception();}s.regions={};s.pending.clear();
    if(s.audio->Loaded()){const auto live=s.audio->Handles();for(auto h:s.sounds)if(std::find(live.begin(),live.end(),h)!=live.end())try{s.audio->Cancel(h);}catch(...){if(!failure)failure=std::current_exception();}}
    s.sounds.clear();if(s.handler&&s.handler.use_count()==1)s.handler->Release();s.handler.reset();s.current.reset();s.session.reset();s.audio.reset();s.settings.reset();s.save.reset();if(failure)std::rethrow_exception(failure);
}
}
