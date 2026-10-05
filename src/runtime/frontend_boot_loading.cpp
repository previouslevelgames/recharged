#include "runtime/frontend_boot_loading.h"
#include "runtime/frontend_boot_audio.h"
#include "resources/frontend_animation.h"
#include "Game/FE/FrontendBootLoadingSteps.h"
#include "Game/FE/feInput.h"
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
void CheckBoot(bool ok,const char* message){if(!ok)throw std::logic_error(message);}
struct AudioBoundary {};
enum class AudioCommand { None, PlayLogo, Unload };
struct BootState
{
    FrontendBootStatus status;
    std::uint32_t strap=0,home=0;
    bool widescreen=false;
};
struct BootStep
{
    FrontendAnimationPlayback& playback;
    FrontendInput& input;
    struct Colour { unsigned char c[4]; };
    struct Slide
    {
        BootStep& owner;std::uint32_t id;
        const FrontendSlide& Value() const
        {const auto& list=owner.playback.Scene().slides;const auto it=std::find_if(list.begin(),list.end(),[&](const auto& p){return p.offset==id;});CheckBoot(it!=list.end(),"Boot slide is absent");return *it;}
        float GetCurrentTime()const{return Value().time;}float GetStartTime()const{return Value().start;}float GetDuration()const{return Value().duration;}
    };
    struct Instance
    {
        BootStep& owner;std::uint32_t id;
        const FrontendInstance& Value() const
        {const auto& list=owner.playback.Scene().instances;const auto it=std::find_if(list.begin(),list.end(),[&](const auto& p){return p.offset==id;});CheckBoot(it!=list.end(),"Boot instance is absent");return *it;}
        void Change(FrontendInstanceChange edit){edit.instance=id;owner.playback.Apply(std::span(&edit,1));}
        void SetVisible(bool flag){FrontendInstanceChange edit;edit.property=FrontendInstanceProperty::Visible;edit.flag=flag;Change(edit);}
        void SetAssetColour(Colour colour){FrontendInstanceChange edit;edit.property=FrontendInstanceProperty::Colour;std::copy_n(colour.c,4,edit.colour.begin());Change(edit);}
        FrontendReference GetTextureResource()const{return Value().resource;}
        void SetTextureResource(FrontendReference resource){FrontendInstanceChange edit;edit.property=FrontendInstanceProperty::ImageResource;edit.image_resource=resource;Change(edit);}
        const FrontendLibraryObject& Component() const
        {const auto& i=Value();CheckBoot(i.type==4&&i.library,"Boot warning requires an authored component");const auto& list=owner.playback.Scene().library;const auto found=std::find_if(list.begin(),list.end(),[&](const auto& p){return p.offset==*i.library;});CheckBoot(found!=list.end()&&found->type==3,"Boot warning component library is absent");return *found;}
        Slide* GetActiveSlide(){const auto active=Component().active_slide;CheckBoot(active.has_value(),"Boot warning has no active authored slide");return owner.SlideAt(*active);}
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {const auto id=Component().offset;CheckBoot(owner.playback.SelectComponent(id,name,reset,preserve),"Boot warning named slide is missing");}
        void Update(float delta){owner.playback.AdvanceComponent(id,delta);}
    };
    std::map<std::uint32_t,Slide> slides;
    std::map<std::uint32_t,Instance> instances;
    Slide* SlideAt(std::uint32_t id){return &slides.try_emplace(id,Slide{*this,id}).first->second;}
    Instance* InstanceAt(std::uint32_t id){return &instances.try_emplace(id,Instance{*this,id}).first->second;}
    struct Presentation
    {
        BootStep& owner;Slide* m_currentSlide=nullptr;
        void Refresh(){const auto active=owner.playback.Scene().active_slide;CheckBoot(active.has_value(),"Boot requires an active authored presentation slide");m_currentSlide=owner.SlideAt(*active);}
        void SetActiveSlide(const char* name,bool reset){CheckBoot(owner.playback.SelectPresentation(name,reset),"Boot named presentation slide is absent");Refresh();}
        Slide* GetActiveSlide(){Refresh();return m_currentSlide;}
    } presentation{*this};
    Presentation* mPresentation=&presentation;
    float mElapsedTime,mStrapAlpha;
    bool mStrapDismissed;
    int mPhase;
    Instance* mStrapImage=nullptr;
    Instance* mHomeButtonWarning=nullptr;
    bool mHomeButtonWarningActive=false,mWidescreen;
    FrontendBootBoundary boundary;
    bool audio_available;
    AudioCommand audio_command = AudioCommand::None;
    BootStep(FrontendAnimationPlayback& p,FrontendInput& i,const BootState& state,bool audio=false):playback(p),input(i),mElapsedTime(state.status.elapsed),mStrapAlpha(state.status.strap_alpha),mStrapDismissed(state.status.strap_dismissed),mPhase(state.status.phase),mWidescreen(state.widescreen),boundary(state.status.boundary),audio_available(audio)
    {presentation.Refresh();if(state.strap)mStrapImage=InstanceAt(state.strap);if(state.home)mHomeButtonWarning=InstanceAt(state.home);}
    Instance* Find(FrontendNode root,std::span<const std::string_view> names,FrontendNodeType type,bool required=true)
    {const auto result=FindFrontendNode(playback.Scene(),root,FrontendNamedPath(names),type);if(!result&&!required)return nullptr;CheckBoot(result.has_value(),"Boot handler requires its authored instance; native default substitutes are unavailable");return InstanceAt(result->id);}
    struct ComponentFinder
    {
        BootStep& s;
        Instance* operator()(Presentation*,const char* slide,const char* layer,const char* name)const
        {const std::array<std::string_view,3> names{slide,layer,name};return s.Find({},names,FrontendNodeType::Component);}
        Instance* operator()(Slide* slide,const char* layer,const char* name,int=0,int=0,int=0,int=0)const
        {CheckBoot(slide!=nullptr,"Boot current slide is absent");const std::array<std::string_view,2> names{layer,name};return s.Find({FrontendNodeKind::Slide,slide->id},names,FrontendNodeType::Component);}
    };
    void SetPhase()
    {
        FrontendBootSetPhase<Slide>(*this,[&](int bank,std::uint32_t cue,const void* a,void* b){
            CheckBoot(bank==0x17&&cue==0xde83984e&&a==0&&b==0,"Unexpected boot audio request");
            if (!audio_available) { boundary=FrontendBootBoundary::PlayLogoSound;throw AudioBoundary{}; }
            CheckBoot(audio_command==AudioCommand::None,"Multiple boot audio requests in one update");
            audio_command=AudioCommand::PlayLogo;
        },ComponentFinder{*this});
    }
    void Created(int language,bool wide)
    {
        FrontendBootCreated<Instance,Instance,Presentation>(*this,language,language,[&]{return wide;},[&]{return language;},
            [&](Presentation*,bool useDefault,const char* slide,const char* layer,const char* name){const std::array<std::string_view,3> names{slide,layer,name};return Find({},names,FrontendNodeType::Image,useDefault||std::string_view(slide)=="strap");},
            ComponentFinder{*this},[&]{SetPhase();});
    }
    void Advance(float delta)
    {
        try
        {
            FrontendBootUpdate<Colour,Slide>(*this,delta,[&](float dt){playback.Advance(dt);presentation.Refresh();},[]{return 0;},
                [&](int pad,int action,bool remap,eFEINPUT_PAD* found){return g_pFEInput->JustPressed(static_cast<eFEINPUT_PAD>(pad),action,remap,found);},
                [&]{SetPhase();},[&](int bank){
                    CheckBoot(audio_available&&bank==0x17,"Boot bank unload requires its resident audio owner");
                    CheckBoot(audio_command==AudioCommand::None,"Multiple boot audio requests in one update");
                    audio_command=AudioCommand::Unload;
                });
        }
        catch(const AudioBoundary&){/* Publish only the source prefix preceding the actual unavailable service. */}
    }
    BootState Result()const
    {return {{mPhase,mElapsedTime,mStrapAlpha,mStrapDismissed,boundary},mStrapImage?mStrapImage->id:0,mHomeButtonWarning?mHomeButtonWarning->id:0,mWidescreen};}
};
}
struct FrontendBootLoading::Implementation
{
    std::shared_ptr<FrontendSession> session;
    std::shared_ptr<FrontendBootAudio> audio;
    FrontendInput& input;
    std::thread::id thread=std::this_thread::get_id();
    Frame current;
    BootState state;
    bool widescreen;
    int language=0;
    Implementation(std::shared_ptr<FrontendSession> s,FrontendInput& i,bool wide):session(std::move(s)),input(i),widescreen(wide)
    {
        CheckBoot(bool(session),"Boot handler requires a retained session");current=session->Current();
        CheckBoot(current&&current->request.animate&&current->request.image_profile==FrontendImageProfile::BootLoading,"Boot handler requires an animated BootLoading resource profile");
        language=current->request.language==FrontendLanguage::English?0:current->request.language==FrontendLanguage::NAFrench?7:8;
        input.HasFocusLock(this);
    }
    void Ready()const{CheckBoot(thread==std::this_thread::get_id(),"Boot handler requires its creating thread");CheckBoot(bool(session),"Boot handler was released");}
    void Mutable(const Frame& expected)const
    {Ready();CheckBoot(nlGetCurrentAsyncRead()==nullptr,"Boot handler mutation inside an NL callback is unsupported");CheckBoot(expected&&expected==current&&session->Current()==current,"Boot handler requires its visible current retained frame");}
};
FrontendBootLoading::FrontendBootLoading(std::shared_ptr<FrontendSession> session,FrontendInput& input,bool widescreen,
    std::shared_ptr<FrontendBootAudio> audio)
    :impl_(std::make_unique<Implementation>(std::move(session),input,widescreen))
{
    auto& s=*impl_;BootState next;
    s.audio=std::move(audio);
    CheckBoot(!s.audio||s.audio->Status().state==FrontendBootAudioState::Loaded,"Boot handler requires a loaded unused audio owner");
    s.Mutable(s.current);
    s.session->HandlerTransaction(s.current,[&](auto& playback){BootStep step(playback,s.input,{});step.Created(s.language,s.widescreen);next=step.Result();});
    s.state=next;s.current=s.session->Current();
}
FrontendBootLoading::~FrontendBootLoading(){try{Release();}catch(...){std::terminate();}}
FrontendBootLoading::Frame FrontendBootLoading::Current()const{impl_->Ready();return impl_->current;}
FrontendBootStatus FrontendBootLoading::Status()const{impl_->Ready();return impl_->state.status;}
void FrontendBootLoading::Update(const Frame& expected,float delta)
{
    auto& s=*impl_;s.Mutable(expected);CheckBoot(std::isfinite(delta)&&delta>=0&&delta<=60,"Boot handler delta exceeds its bounded profile");
    if(s.state.status.boundary!=FrontendBootBoundary::None)return;
    s.input.Focus(&s);BootState next;AudioCommand command=AudioCommand::None;
    s.session->HandlerTransaction(expected,[&](auto& playback){
        BootStep step(playback,s.input,s.state,bool(s.audio));step.Advance(delta);
        next=step.Result();command=step.audio_command;
    },[&]{
        if(command==AudioCommand::PlayLogo)s.audio->PlayLogo();
        else if(command==AudioCommand::Unload)s.audio->Unload(0x17);
    });
    s.state=next;s.current=s.session->Current();
}
void FrontendBootLoading::Reset(const Frame& expected)
{
    auto& s=*impl_;s.Mutable(expected);BootState next;
    CheckBoot(!s.audio||s.audio->Status().state==FrontendBootAudioState::Loaded,
        "Restart after logo playback requires newly loaded boot resources");
    s.session->HandlerTransaction(expected,[&](auto& playback){playback.Reset();BootStep step(playback,s.input,{});step.Created(s.language,s.widescreen);next=step.Result();});
    s.state=next;s.current=s.session->Current();
}
void FrontendBootLoading::Release()
{auto& s=*impl_;CheckBoot(s.thread==std::this_thread::get_id(),"Boot handler requires its creating thread");if(!s.session)return;CheckBoot(nlGetCurrentAsyncRead()==nullptr,"Boot handler release inside an NL callback is unsupported");s.audio.reset();s.current.reset();s.session.reset();}
}
