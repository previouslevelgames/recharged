#include "runtime/frontend_audio_options.h"
#include "runtime/frontend_handler.h"
#include "resources/frontend_animation.h"
#include "Game/FE/FrontendAudioOptionsSteps.h"
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
    FrontendAudioOptionsStatus status;
    std::array<std::uint32_t,6> buttons{};
    std::array<std::array<std::uint32_t,10>,3> bars{};
    std::array<FrontendPointerBinding,6> bindings{};
};
struct Step
{
    FrontendAnimationPlayback& playback;const Localization& localization;State value;
    int mOverlayMode=0,mState=value.status.state;
    std::array<int,3> mSettings=value.status.settings,mBackupSettings=value.status.backup;
    bool mSaveStarted=false;
    struct Instance;
    struct Slide
    {
        Step& owner;std::uint32_t id;
        FrontendNode Root()const{return{FrontendNodeKind::Slide,id};}
        const FrontendSlide& Value()const
        {const auto& all=owner.playback.Scene().slides;const auto it=std::find_if(all.begin(),all.end(),[&](const auto& s){return s.offset==id;});Check(it!=all.end(),"Audio options slide is absent");return *it;}
        float GetCurrentTime()const{return Value().time;}float GetStartTime()const{return Value().start;}float GetDuration()const{return Value().duration;}
    };
    struct Instance
    {
        Step& owner;std::uint32_t id;
        struct Visibility{Step& owner;std::uint32_t id;void operator=(bool v){FrontendInstanceChange c;c.instance=id;c.property=FrontendInstanceProperty::Visible;c.flag=v;owner.playback.Apply({&c,1});}}m_bVisible{owner,id};
        FrontendNode Root()const{return{FrontendNodeKind::Instance,id};}
        const FrontendInstance& Value()const
        {const auto& all=owner.playback.Scene().instances;const auto it=std::find_if(all.begin(),all.end(),[&](const auto& v){return v.offset==id;});Check(it!=all.end(),"Audio options instance is absent");return *it;}
        Position GetAssetPosition()const{const auto& p=Value().attributes.position;return{{p[0],p[1],p[2]}};}
        Slide* GetActiveSlide()
        {
            const auto& v=Value();Check(v.type==4&&v.library,"Audio options component is required");const auto& all=owner.playback.Scene().library;
            const auto it=std::find_if(all.begin(),all.end(),[&](const auto& l){return l.offset==*v.library;});Check(it!=all.end()&&it->active_slide,"Audio options component has no active slide");return owner.AtSlide(*it->active_slide);
        }
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {const auto& v=Value();Check(v.type==4&&v.library&&owner.playback.SelectComponent(*v.library,name,reset,preserve),"Audio options feedback slide is absent");}
        void SetAssetColour(const std::array<std::uint8_t,4>& colour)
        {FrontendInstanceChange c;c.instance=id;c.property=FrontendInstanceProperty::Colour;c.colour=colour;owner.playback.Apply({&c,1});}
        void SetString(std::u16string text)
        {FrontendInstanceChange c;c.instance=id;c.property=FrontendInstanceProperty::String;c.text=std::move(text);owner.playback.Apply({&c,1});}
    };
    struct Presentation
    {
        Step& owner;Slide* m_currentSlide=nullptr;
        FrontendNode Root()const{return{FrontendNodeKind::Presentation,0};}
        void SetActiveSlide(const char* name,bool reset){Check(owner.playback.SelectPresentation(name,reset),"Audio options presentation is absent");m_currentSlide=owner.ActiveSlide();}
    } presentation{*this};Presentation* mPresentation=&presentation;
    struct Button
    {
        std::array<int,4> states{};FrontendPointerBinding binding;Step* owner=nullptr;
        bool HasOtherPointerState(int state,int except)const{for(unsigned i=0;i<4;++i)if(int(i)!=except&&states[i]==state)return true;return false;}
        void SetPointerState(int state,int index){Check(index>=0&&index<4,"Audio options pointer index is invalid");states[index]=state;}
        void PlayHoverFeedback(int index){Check(owner&&index>=0&&index<4,"Audio options hover pointer is invalid");owner->Command(FrontendAudioOptionsCommandKind::HoverRumble,index);} // Explicit unprovided haptic request.
        void SetInstanceBounds(Instance* instance,bool rotate,float x,float y,float sx,float sy)
        {Check(instance,"Audio options pointer instance is absent");binding={instance->id,rotate,x,y,sx,sy};}
    };
    std::array<Instance*,6> mButtons{};std::array<std::array<Instance*,10>,3> mVolumeBars{};
    std::array<Button,6> mButtonComponents{};
    std::map<std::uint32_t,Instance> instances;std::map<std::uint32_t,Slide> slides;
    Step(FrontendAnimationPlayback& p,const Localization& l,State s):playback(p),localization(l),value(std::move(s))
    {
        for(unsigned i=0;i<6;++i){if(value.buttons[i])mButtons[i]=At(value.buttons[i]);mButtonComponents[i]={value.status.pointer_states[i],value.bindings[i],this};}
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<10;++j)if(value.bars[i][j])mVolumeBars[i][j]=At(value.bars[i][j]);
        presentation.m_currentSlide=ActiveSlide();
    }
    Instance* At(std::uint32_t id){return &instances.try_emplace(id,Instance{*this,id}).first->second;}
    Slide* AtSlide(std::uint32_t id){return &slides.try_emplace(id,Slide{*this,id}).first->second;}
    Slide* ActiveSlide(){const auto id=playback.Scene().active_slide;Check(bool(id),"Audio options has no active presentation");return AtSlide(*id);}
    template<class T,int N>struct Finder
    {
        template<class Root,class... Names>static T* Find(Root* root,Names... names)
        {
            const std::array<std::string_view,sizeof...(Names)> path{names...};
            const auto found=FindFrontendNode(root->owner.playback.Scene(),root->Root(),FrontendNamedPath(path));
            Check(found&&found->kind==FrontendNodeKind::Instance,"Audio options authored lookup path is absent");return root->owner.At(found->id);
        }
    };
    bool IsVolumeButtonEnabled(unsigned item)const{return FrontendAudioOptionsEnabled(*this,item);}
    void MarkUnusedVolumeButtons(){FrontendAudioOptionsMarkUnused(*this);}
    void UpdateVolumeBars(int setting)
    {FrontendAudioOptionsBars<std::array<std::uint8_t,4>>(*this,setting,[](auto& c,int r,int g,int b,int a){c={std::uint8_t(r),std::uint8_t(g),std::uint8_t(b),std::uint8_t(a)};});}
    void UpdateVolumeLevelText(int setting)
    {
        FrontendAudioOptionsText(*this,setting,[](Slide* slide,const char* a,const char* b,const char* c){return Finder<Instance,3>::Find(slide,a,b,c);},
            [&](Instance* text,int,const char* key,int level){
                Check(text->Value().type==3,"Audio options label is not text");const auto number=std::to_string(level);std::u16string digits(number.begin(),number.end());
                std::u16string formatted=Format(std::u16string(localization.Get(FrontendLowerHash(key))),digits);
                Check(formatted.size()<16,"Audio options formatted label exceeds original16-unit storage");text->SetString(std::move(formatted));
            });
    }
    void Command(FrontendAudioOptionsCommandKind kind,unsigned argument=0)
    {Check(value.status.commands.size()<32,"Audio options command budget exceeded");value.status.commands.push_back({kind,argument});}
    struct Navigation
    {
        Step& s;void SetButtons(unsigned mask,bool enabled){Check(enabled&&(mask==0||mask==0x24),"Audio options NAV request is unsupported");s.Command(mask?FrontendAudioOptionsCommandKind::ShowBackAndDone:FrontendAudioOptionsCommandKind::ResetNavigation,mask);}
        void SetDoneButtonText(int value){Check(value==1,"Audio options done label request is unsupported");s.Command(FrontendAudioOptionsCommandKind::DoneText,1);}
    } navigation{*this};
    struct SaveButton
    {
        Step& owner;
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {Check(std::string_view(name)=="down"&&reset&&!preserve,"Audio options done feedback differs");owner.Command(FrontendAudioOptionsCommandKind::DoneDown);}
    } saveButton{*this};SaveButton* mSaveButton=&saveButton;
    State Result()
    {
        value.status.settings=mSettings;value.status.state=mState;
        for(unsigned i=0;i<6;++i){Check(mButtons[i]&&mButtons[i]->Value().type==4,"Audio options button is not a component");value.buttons[i]=mButtons[i]->id;value.status.pointer_states[i]=mButtonComponents[i].states;value.bindings[i]=mButtonComponents[i].binding;}
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<10;++j){Check(mVolumeBars[i][j],"Audio options volume bar is absent");value.bars[i][j]=mVolumeBars[i][j]->id;}
        return std::move(value);
    }
};
struct Pending{FrontendPointerCallback kind;unsigned item,index;FrontendSession::Handle frame;};
struct Busy{bool& value;explicit Busy(bool& v):value(v){value=true;}~Busy(){value=false;}};
}
struct FrontendAudioOptions::Implementation
{
    std::shared_ptr<FrontendSession> session;FrontendInput& input;std::shared_ptr<FrontendAudio> audio;AudioCategoryVolumes::Handle volumes;
    unsigned& seed;unsigned controller;const std::thread::id thread=std::this_thread::get_id();
    FrontendSession::Handle current;State state;FrontendPointerHost host;std::shared_ptr<FrontendHandler> handler;
    std::shared_ptr<NativePreferences> native_save;
    std::array<std::shared_ptr<FrontendPointerRegion>,6> regions{};std::vector<Pending> pending;std::vector<FrontendAudioHandle> sounds;
    bool busy=false,failed=false,stack_attached=false,stack_update=false,input_window=false;
    FrontendSession::Handle input_source;
    Implementation(std::shared_ptr<FrontendSession> s,FrontendInput& i,std::shared_ptr<FrontendAudio> a,AudioCategoryVolumes::Handle v,unsigned& rng,unsigned c)
        :session(std::move(s)),input(i),audio(std::move(a)),volumes(std::move(v)),seed(rng),controller(c),host(i,c)
    {
        Check(session&&controller<4&&audio&&audio->Loaded()&&volumes&&audio->CategoryVolumes()==volumes,"Audio options require retained scene and genuine shared category output");
        current=session->Current();Check(current&&current->request.animate&&current->request.image_profile==FrontendImageProfile::Main,"Audio options require animated Main resources");
        const auto it=std::find_if(current->graph.slides.begin(),current->graph.slides.end(),[&](const auto& s){return s.offset==current->graph.active_slide;});
        Check(it!=current->graph.slides.end()&&it->name=="OPTIONS_IN","Audio options require the authored OPTIONS_IN presentation");
        state.status.settings=state.status.backup=volumes->Snapshot().settings;pending.reserve(32);
    }
    void Ready()const{Check(thread==std::this_thread::get_id()&&session&&!failed,"Audio options require their live nonfailed owning thread");}
    void Mutable()const{Ready();Check(!busy&&!nlGetCurrentAsyncRead()&&(!stack_attached||stack_update),"Audio options mutation requires its idle owner or controlled stack update");}
    void Expected(const FrontendSession::Handle& f,bool acknowledge=false)const
    {if(acknowledge){Ready();Check(!busy&&!stack_update&&!nlGetCurrentAsyncRead(),"Cannot acknowledge Audio options during update");}else Mutable();Check(f&&f==current&&f==session->Current(),"Audio options require their exact current frame");}
    FrontendSession::Handle Presented()const{return input_window?input_source:current;}
    void InputExpected(const FrontendSession::Handle& frame)const
    {Expected(current);Check(frame&&frame==Presented(),"Audio input requires its last acknowledged stack frame");}
    bool Interactive()const{return state.status.state==1&&state.status.initialized;}
    bool RegionsPresented()const{return std::all_of(regions.begin(),regions.end(),[&](const auto& region){return region&&region->Current()==Presented();});}
    void Queue(FrontendPointerCallback kind,unsigned item,unsigned index,const FrontendSession::Handle& f)
    {if(kind==FrontendPointerCallback::Enter||kind==FrontendPointerCallback::Leave||kind==FrontendPointerCallback::Press){Check(pending.size()<32,"Audio options pointer event budget exceeded");pending.push_back({kind,item,index,f});}}
    void Play(const std::vector<std::uint32_t>& cues)
    {const auto live=audio->Handles();std::erase_if(sounds,[&](auto h){return std::find(live.begin(),live.end(),h)==live.end();});sounds.reserve(sounds.size()+cues.size());for(auto cue:cues){auto h=audio->Play(cue,seed);Check(bool(h),"Audio options authored cue is unavailable");sounds.push_back(*h);}}
};
FrontendAudioOptions::FrontendAudioOptions(std::shared_ptr<FrontendSession> session,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,AudioCategoryVolumes::Handle volumes,unsigned& seed,unsigned controller)
    :FrontendAudioOptions(std::move(session),input,std::move(audio),std::move(volumes),seed,{},controller){}
FrontendAudioOptions::FrontendAudioOptions(std::shared_ptr<FrontendSession> session,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,AudioCategoryVolumes::Handle volumes,unsigned& seed,std::shared_ptr<FrontendHandler> base,unsigned controller)
    :impl_(std::make_unique<Implementation>(std::move(session),input,std::move(audio),std::move(volumes),seed,controller))
{
    auto& s=*impl_;Check(!base||base->Binds(s.session),"Audio options shared handler belongs to another session");s.handler=base?std::move(base):std::make_shared<FrontendHandler>(s.session,input);State next;
    s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);
        step.Command(FrontendAudioOptionsCommandKind::HideNavigation);step.Command(FrontendAudioOptionsCommandKind::BindBack,4);step.Command(FrontendAudioOptionsCommandKind::BindDone,0x20);
        for(unsigned i=0;i<4;++i)step.Command(FrontendAudioOptionsCommandKind::PointerWaiting,i);
        FrontendAudioOptionsCreated<Step::Instance,Step::Instance,Step::Finder>(step,[](char* name,int size,int index){if(index)std::snprintf(name,size,"whitebox%d",index);else std::snprintf(name,size,"whitebox");});next=step.Result();});
    s.state=std::move(next);s.current=s.session->Current();
}
FrontendAudioOptions::~FrontendAudioOptions(){try{Release();}catch(...){std::terminate();}}
FrontendSession::Handle FrontendAudioOptions::Current()const{impl_->Ready();return impl_->current;}
FrontendAudioOptionsStatus FrontendAudioOptions::Status()const
{auto& s=*impl_;Check(s.thread==std::this_thread::get_id()&&s.session,"Audio options status requires its owning thread");auto result=s.state.status;result.failed=s.failed;return result;}
void FrontendAudioOptions::AdvanceVisual(const FrontendSession::Handle& frame,float delta)
{
    auto& s=*impl_;Check(!s.stack_attached,"Stack owns the only Audio options base update");s.Expected(frame);if(s.input.InputLocked())return;Check(s.pending.empty(),"Audio options pointer events are pending");
    AfterBaseUpdate(s.handler->UpdateOnce(frame,delta));
}
void FrontendAudioOptions::AfterBaseUpdate(FrontendHandler::UpdateProof&& proof)
{
    auto& s=*impl_;s.Mutable();s.current=s.handler->ConsumeUpdate(std::move(proof),s.current);
    try
    {
        State next;
        s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();
            const bool ready=FrontendAudioOptionsGate(step,[&]{return &step.navigation;},[&](int i){step.Command(FrontendAudioOptionsCommandKind::PointerWaiting,i);},[&](int id){Check(id==13,"Audio options transition scene differs");step.Command(FrontendAudioOptionsCommandKind::PushOptions,id);},[](int){throw UnsupportedResource("Overlay audio options are unavailable");});
            if(ready){if(!step.value.status.initialized){FrontendAudioOptionsBind<Step::Instance,Position,Step::Finder>(step);step.value.status.initialized=true;}for(unsigned i=0;i<4;++i)step.Command(i==s.controller?FrontendAudioOptionsCommandKind::PointerCursor:FrontendAudioOptionsCommandKind::PointerWaiting,i);}
            next=step.Result();});s.state=std::move(next);s.current=s.session->Current();
    }catch(...){s.failed=true;throw;}
}
void FrontendAudioOptions::Acknowledge(const FrontendSession::Handle& frame,FrontendPointerViewport viewport)
{
    auto& s=*impl_;s.Expected(frame,true);
    try
    {
        if(s.state.status.initialized)
        {for(unsigned i=0;i<6;++i){if(s.regions[i])s.regions[i]->RebindFrame(frame);else s.regions[i]=std::make_shared<FrontendPointerRegion>(s.input,frame,s.state.bindings[i],[&s,i](auto kind,unsigned index,const auto& f){s.Queue(kind,i,index,f);});}s.host.Publish(frame,viewport,s.regions);}
        else s.host.Publish(frame,viewport,{});
    }catch(...){s.failed=true;throw;}
}
std::array<FrontendPointerBounds,6> FrontendAudioOptions::Bounds()const
{impl_->Ready();std::array<FrontendPointerBounds,6> out;for(unsigned i=0;i<6;++i){Check(bool(impl_->regions[i]),"Audio options bounds await presentation");out[i]=impl_->regions[i]->Bounds();}return out;}
void FrontendAudioOptions::ApplyPending()
{
    auto& s=*impl_;s.Expected(s.current);if(s.pending.empty())return;const auto presented=s.Presented();Busy guard(s.busy);
    try
    {
        auto events=std::move(s.pending);s.pending.clear();s.pending.reserve(32);State next;struct Effect{bool cue;std::uint32_t value;AudioCategory category;};std::vector<Effect> effects;
        Check(s.volumes->Snapshot().settings==s.state.status.settings,"Audio options settings were changed by another authority");
        s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();
            struct Settings
            {
                std::vector<Effect>& effects;int MusicVolume,SFXVolume,VoiceVolume;
                void ApplyMusicVolume(){effects.push_back({false,unsigned(MusicVolume),AudioCategory::Music});}void ApplySFXVolume(){effects.push_back({false,unsigned(SFXVolume),AudioCategory::Sfx});}void ApplyVoiceVolume(){effects.push_back({false,unsigned(VoiceVolume),AudioCategory::Voice});}
            }settings{effects,step.mSettings[0],step.mSettings[1],step.mSettings[2]};
            auto play=[&](unsigned long cue,const void* a,void* b,bool restart){Check(cue<=UINT32_MAX&&!a&&!b&&restart,"Audio options cue contract differs");effects.push_back({true,std::uint32_t(cue),AudioCategory::Music});};
            for(const auto& event:events){Check(event.frame==presented&&event.item<6&&event.index<4,"Audio options event is stale or invalid");if(step.mState!=1)break;
                if(event.kind==FrontendPointerCallback::Enter)FrontendAudioOptionsEnter(step,event.index,event.item,play);
                else if(event.kind==FrontendPointerCallback::Leave)FrontendAudioOptionsLeave(step,event.index,event.item);
                else FrontendAudioOptionsPress(step,event.index,event.item,[&]{return &settings;},play,[](int,unsigned long,const void*,void*){throw UnsupportedResource("Overlay voice preview is unavailable");});}
            next=step.Result();},[&]{for(auto effect:effects){if(effect.cue)s.Play({effect.value});else s.volumes->Set(effect.category,int(effect.value));}});
        s.state=std::move(next);s.current=s.session->Current();
    }catch(...){s.failed=true;throw;}
}
void FrontendAudioOptions::DeliverPointer(const FrontendSession::Handle& frame,const FrontendPointerEvent& event)
{
    auto& s=*impl_;s.InputExpected(frame);Check(s.host.Current()&&s.host.Current()->Frame()==frame&&event.index<4,"Audio options input requires actual presentation");if(!s.Interactive()||!s.RegionsPresented())return;
    try{for(auto& region:s.regions)region->Deliver(frame,event);ApplyPending();}catch(...){s.failed=true;throw;}
}
FrontendPointerDispatch FrontendAudioOptions::Route(const FrontendPointerDesktopSample& sample)
{auto& s=*impl_;s.Expected(s.current);Check(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Audio options input requires actual presentation");if(!s.Interactive()||!s.RegionsPresented())return{};try{auto out=s.host.Route(s.host.Current(),sample);ApplyPending();return out;}catch(...){s.failed=true;throw;}}
FrontendPointerDispatch FrontendAudioOptions::Poll(SDL_Window* window,bool capture)
{auto& s=*impl_;s.Expected(s.current);Check(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Audio options input requires actual presentation");if(!s.Interactive()||!s.RegionsPresented())return{};try{auto out=s.host.Poll(s.host.Current(),window,capture);ApplyPending();return out;}catch(...){s.failed=true;throw;}}
void FrontendAudioOptions::NotifyBackButton(const FrontendSession::Handle& frame)
{
    auto& s=*impl_;s.InputExpected(frame);Check(s.Interactive()&&s.RegionsPresented()&&s.host.Current()&&s.host.Current()->Frame()==frame,"Audio options back requires its genuinely presented interactive frame");Busy guard(s.busy);State next;
    std::array<int,3> restored{};std::vector<std::uint32_t> cues;
    try{s.session->HandlerTransaction(s.current,[&](auto& p){
        Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();
        struct Settings{int MusicVolume=0,SFXVolume=0,VoiceVolume=0;std::array<int,3>& restored;void ApplySettings(){restored={MusicVolume,SFXVolume,VoiceVolume};}}settings{0,0,0,restored};
        FrontendAudioOptionsBack(step,[&]{return &settings;},[&](unsigned long cue,const void* a,void* b,bool restart){Check(cue<=UINT32_MAX&&!a&&!b&&restart,"Audio options back cue contract differs");cues.push_back(cue);},[&]{return &step.navigation;});next=step.Result();
        },[&]{s.volumes->SetAll(restored);s.Play(cues);});s.state=std::move(next);s.current=s.session->Current();}
    catch(...){s.failed=true;throw;}
}
void FrontendAudioOptions::Save(const FrontendSession::Handle& frame)
{impl_->InputExpected(frame);throw UnsupportedResource("Original audio options SaveLoad::StartSave(false) requires a real save service");}
void FrontendAudioOptions::SaveNativePreferences(const FrontendSession::Handle& frame,std::shared_ptr<NativePreferences> preferences)
{
    auto& s=*impl_;s.InputExpected(frame);
    Check(s.Interactive()&&s.RegionsPresented()&&s.host.Current()&&s.host.Current()->Frame()==frame&&!s.state.status.native_save_admitted,"Native audio save requires its presented interactive frame");
    Check(bool(preferences),"Native audio save requires actual preferences ownership");
    const auto status=preferences->Status();
    Check((status.state==NativePreferencesState::Ready||status.state==NativePreferencesState::Missing)&&status.save_enabled&&!status.host_pending,"Native preferences load must finish before saving Audio options");
    Check(s.volumes->Snapshot().settings==s.state.status.settings,"Audio settings changed outside their selected owner");
    auto values=*preferences->Current();values.audio=s.state.status.settings;resources::ValidateNativePreferences(values);
    Busy guard(s.busy);State next;std::vector<std::uint32_t> cues;
    try
    {
        s.session->HandlerTransaction(s.current,[&](auto& p){Step step(p,*s.current->visuals->localization,s.state);step.value.status.commands.clear();
            FrontendAudioOptionsSaveVisual(step,[&]{return &step.navigation;},[&](unsigned long cue,const void* name,void* context,bool restart){Check(cue<=UINT32_MAX&&!name&&!context&&restart,"Audio save cue contract differs");cues.push_back(cue);});
            Check(step.mSaveStarted,"Original audio save visual did not mark its request");step.value.status.native_save_admitted=true;next=step.Result();
        },[&]{s.Play(cues);preferences->StartSave(values);});
        s.native_save=std::move(preferences);s.state=std::move(next);s.current=s.session->Current();
    }
    catch(...){s.failed=true;throw;}
}
std::shared_ptr<FrontendSession> FrontendAudioOptions::StackSession()const{impl_->Ready();return impl_->session;}
std::shared_ptr<FrontendHandler> FrontendAudioOptions::StackHandler()const{impl_->Ready();return impl_->handler;}
unsigned FrontendAudioOptions::StackScene()const{return 14;}
bool FrontendAudioOptions::CanUpdateStack()const
{auto& s=*impl_;s.Ready();Check(s.stack_attached&&!s.stack_update&&!s.busy&&!nlGetCurrentAsyncRead(),"Audio pre-base admission requires its idle retained stack owner");return !s.input.InputLocked();}
void FrontendAudioOptions::AttachStack()
{auto& s=*impl_;s.Mutable();Check(!s.stack_attached,"Audio visual already belongs to a stack");s.stack_attached=true;}
void FrontendAudioOptions::UpdateStack(FrontendHandler::UpdateProof&& proof,const FrontendSession::Handle& presented,const std::function<void()>& input)
{
    auto& s=*impl_;s.Ready();Check(s.stack_attached&&!s.stack_update&&!s.busy&&!nlGetCurrentAsyncRead(),"Audio stack update requires its idle retained owner");
    Check(presented&&presented==s.current&&proof.Before()==presented&&proof.After()==s.session->Current(),"Audio stack proof/presentation differs");
    s.stack_update=true;s.input_source=presented;
    struct Reset{Implementation& s;~Reset(){s.input_window=false;s.input_source.reset();s.stack_update=false;}}reset{s};
    try{AfterBaseUpdate(std::move(proof));s.input_window=true;if(input)input();Check(s.current==s.session->Current(),"Stack input mutated outside its selected Audio owner");}
    catch(...){s.failed=true;throw;}
}
void FrontendAudioOptions::ReleaseStack()
{auto& s=*impl_;Check(!s.stack_update&&!s.busy,"Cannot remove an active Audio update");s.stack_attached=false;Release();}
void FrontendAudioOptions::Release()
{
    auto& s=*impl_;Check(s.thread==std::this_thread::get_id()&&!s.busy&&!nlGetCurrentAsyncRead(),"Audio options teardown requires its idle owning thread");if(!s.session)return;Check(!s.stack_attached,"Stack owns selected Audio visual teardown");Busy guard(s.busy);
    std::exception_ptr failure;try{s.host.Release();}catch(...){failure=std::current_exception();}s.regions={};s.pending.clear();
    if(s.audio->Loaded()){const auto live=s.audio->Handles();for(auto h:s.sounds)if(std::find(live.begin(),live.end(),h)!=live.end())try{s.audio->Cancel(h);}catch(...){if(!failure)failure=std::current_exception();}}
    s.sounds.clear();if(s.handler&&s.handler.use_count()==1)s.handler->Release();s.handler.reset();s.native_save.reset();s.current.reset();s.session.reset();s.audio.reset();s.volumes.reset();if(failure)std::rethrow_exception(failure);
}
}
