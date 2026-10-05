#include "runtime/frontend_navigation.h"
#include "runtime/frontend_handler.h"
#include "resources/frontend_animation.h"
#include "Game/FE/FrontendNavigationSteps.h"
#include "Game/FE/FrontendNavigationTransitionSteps.h"
#include "Game/FE/FrontendMainMenuSteps.h"
#include "Game/FE/FrontendDoneButtonSteps.h"
#include "NL/nlFileGC.h"
#include "NL/nlMath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>

namespace mscharged
{
namespace
{
using namespace resources;
void CheckNav(bool value,const char* text){if(!value)throw std::logic_error(text);}
struct Position {struct {float x,y,z;} f;};
struct State
{
    FrontendNavigationStatus status;
    std::array<std::uint32_t,4> pointers{};
    std::array<std::uint32_t,8> buttons{};
    std::uint32_t transition=0,home=0,timer=0,back=0;
    FrontendPointerBinding binding;
    Position back_position{};
    bool pressed=false,back_ce=false;
    float back_time=0;
};
struct Step
{
    FrontendAnimationPlayback* playback=nullptr;const FrontendScene* snapshot=nullptr;State state;
    const FrontendScene& Scene()const{return playback?playback->Scene():*snapshot;}
    FrontendAnimationPlayback& Playback(){CheckNav(playback,"Read-only NAV adapter mutation");return *playback;}
    struct Node
    {
        Step& owner;FrontendNode root;
        Node(Step& s,FrontendNode n):owner(s),root(n){}
        struct Time {const Node& node;operator float()const
        {const auto& slides=node.owner.Scene().slides;auto it=std::find_if(slides.begin(),slides.end(),[&](const auto& v){return v.offset==node.root.id;});CheckNav(node.root.kind==FrontendNodeKind::Slide&&it!=slides.end(),"NAV transition slide is absent");return it->time;}} m_time{*this};
        struct Visible {Node& node;void operator=(bool v){node.SetVisible(v);}} m_bVisible{*this};
        const FrontendInstance& Value()const
        {const auto& v=owner.Scene().instances;auto it=std::find_if(v.begin(),v.end(),[&](const auto& n){return n.offset==root.id;});CheckNav(it!=v.end(),"NAV instance is absent");return *it;}
        FrontendNode Root()const{return root;}
        void SetVisible(bool value){if(root.id){FrontendInstanceChange c;c.instance=root.id;c.flag=value;owner.Playback().Apply(std::span(&c,1));}}
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {if(root.id){const auto& v=Value();CheckNav(v.type==4&&v.library,"NAV selection requires a component");owner.Playback().SelectComponent(*v.library,name,reset,preserve);}}
        Node* GetActiveSlide()
        {
            if(!root.id)return owner.At({FrontendNodeKind::Slide,0});
            const auto& v=Value();CheckNav(v.type==4&&v.library,"NAV active slide requires a component");
            const auto& libs=owner.Scene().library;auto it=std::find_if(libs.begin(),libs.end(),[&](const auto& n){return n.offset==*v.library;});
            CheckNav(it!=libs.end(),"NAV component library is absent");return it->active_slide?owner.At({FrontendNodeKind::Slide,*it->active_slide}):nullptr;
        }
        Position GetAssetPosition()const{const auto& a=Value().attributes.position;return {{a[0],a[1],a[2]}};}
        void SetAssetPosition(float x,float y,float z){ChangeVector(FrontendInstanceProperty::Position,{x,y,z});}
        void SetAssetRotation(float x,float y,float z){ChangeVector(FrontendInstanceProperty::Rotation,{x,y,z});}
        void ChangeVector(FrontendInstanceProperty property,std::array<float,3> value)
        {FrontendInstanceChange c;c.instance=root.id;c.property=property;c.vector=value;owner.Playback().Apply(std::span(&c,1));}
        void SetStringId(const char* name)
        {if(root.id){CheckNav(Value().type==3,"NAV label requires a text instance");FrontendInstanceChange c;c.instance=root.id;c.property=FrontendInstanceProperty::StringId;c.string_id=name;owner.Playback().Apply(std::span(&c,1));}}
    };
    std::map<std::pair<int,std::uint32_t>,Node> nodes;
    Node* At(FrontendNode root){return &nodes.try_emplace({int(root.kind),root.id},*this,root).first->second;}
    Node* Instance(std::uint32_t id){return At({FrontendNodeKind::Instance,id});}
    struct Presentation {Step& owner;Node* GetActiveSlide(){const auto id=owner.Scene().active_slide;CheckNav(bool(id),"NAV presentation is absent");return owner.At({FrontendNodeKind::Slide,*id});}} presentation{*this};
    Presentation* GetPresentation(){return &presentation;}
    template<class T,int N>struct Finder
    {
        template<class Root,class...Names>static T* Find(Root* root,Names...names)
        {
            if(!root||!root->root.id)return nullptr;
            const std::array<std::string_view,sizeof...(Names)> path{names...};
            auto found=FindFrontendNode(root->owner.Scene(),root->Root(),FrontendNamedPath(path),FrontendNodeType::Any);
            return found?root->owner.At(*found):nullptr;
        }
        template<class Root,class...Names>static T* FindOrDefault(Root* root,Names...names)
        {CheckNav(root,"NAV source default lookup lacks its root");if(auto found=Find(root,names...))return found;return root->owner.Instance(0);}
    };
    std::array<Node*,4> mPointerInstances{};
    Node *mPlusButton,*mMinusButton,*mBackButton,*mBreadcrumbs,*mPlayButton,*mDoneButton,*mLowerDoneButton,*mProgressButton,*mTransition,*mHomeWarning,*mTimer;
    unsigned mVisibleButtons;bool mIsWidescreen;
    bool mTransitionPlaying,mTransitionPending;
    Node* mButtonInstance;Position mButtonPosition;
    bool mPressed,mPushBackScene=false,mPopScene=false,mUnidentifiedCE;
    int mBackScene=-2;float mUnidentifiedB8;
    std::array<bool,4> mPointerInside;
    Step(FrontendAnimationPlayback& p,State v):Step(&p,nullptr,std::move(v)){}
    Step(const FrontendScene& graph,State v):Step(nullptr,&graph,std::move(v)){}
    Step(FrontendAnimationPlayback* p,const FrontendScene* graph,State v):playback(p),snapshot(graph),state(std::move(v)),
        mVisibleButtons(state.status.visible_buttons),mIsWidescreen(state.status.widescreen),
        mTransitionPlaying(state.status.transition_playing),mTransitionPending(state.status.transition_pending),
        mButtonInstance(Instance(state.back)),mButtonPosition(state.back_position),mPressed(state.pressed),
        mUnidentifiedCE(state.back_ce),mUnidentifiedB8(state.back_time),mPointerInside(state.status.back_inside)
    {
        for(unsigned i=0;i<4;++i)mPointerInstances[i]=Instance(state.pointers[i]);
        std::array<Node**,8> targets{&mPlusButton,&mMinusButton,&mBackButton,&mBreadcrumbs,&mPlayButton,&mDoneButton,&mLowerDoneButton,&mProgressButton};
        for(unsigned i=0;i<8;++i)*targets[i]=Instance(state.buttons[i]);
        mTransition=Instance(state.transition);mHomeWarning=Instance(state.home);mTimer=Instance(state.timer);
    }
    void ResetButtons(bool enabled){FrontendNavigationResetButtons<Node,Node,Finder>(*this,enabled);}
    void SetPlayButtonText(int v){FrontendNavigationSetPlayButtonText<Node,Node,Finder>(*this,v);}
    void SetBackButtonText(int v){FrontendNavigationSetBackButtonText<Node,Node,Finder>(*this,v);}
    void SetDoneButtonText(int v){FrontendNavigationSetDoneButtonText<Node,Node,Finder>(*this,v);}
    bool HasOtherPointerState(int v,unsigned index)const{return FrontendPointerHasOtherState(state.status.back_states,v,index);}
    void SetPointerState(int v,unsigned index){CheckNav(index<4,"NAV pointer exceeds four");state.status.back_states[index]=v;}
    void SetInstanceBounds(Node* node,bool rotate,float x,float y,float sx,float sy)
    {CheckNav(node&&node->root.id&&node->Value().type==2,"NAV back requires its genuine image bounds");state.binding={node->root.id,rotate,x,y,sx,sy};}
    State Result()
    {
        for(unsigned i=0;i<4;++i){CheckNav(mPointerInstances[i]&&mPointerInstances[i]->root.id&&mPointerInstances[i]->Value().type==4,"NAV requires four authored cursors");state.pointers[i]=mPointerInstances[i]->root.id;}
        const std::array<Node*,8> buttons{mPlusButton,mMinusButton,mBackButton,mBreadcrumbs,mPlayButton,mDoneButton,mLowerDoneButton,mProgressButton};
        for(unsigned i=0;i<8;++i)state.buttons[i]=buttons[i]->root.id;
        CheckNav(mBackButton->root.id&&mBackButton->Value().type==4&&mButtonInstance&&mButtonInstance->root.id&&mButtonInstance->Value().type==4,"NAV back component is absent or mistyped");
        state.back=mButtonInstance->root.id;state.back_position=mButtonPosition;state.transition=mTransition->root.id;state.home=mHomeWarning->root.id;state.timer=mTimer->root.id;
        state.status.transition_playing=mTransitionPlaying;state.status.transition_pending=mTransitionPending;
        state.status.visible_buttons=mVisibleButtons;state.status.widescreen=mIsWidescreen;state.status.back_inside=mPointerInside;
        state.pressed=mPressed;state.back_ce=mUnidentifiedCE;state.back_time=mUnidentifiedB8;return std::move(state);
    }
};
struct Pending{FrontendPointerCallback kind;unsigned index;FrontendSession::Handle frame;};
struct Busy{bool& value;explicit Busy(bool& v):value(v){value=true;}~Busy(){value=false;}};
}
struct FrontendNavigation::Implementation
{
    std::shared_ptr<FrontendSession> session;FrontendInput& input;std::shared_ptr<FrontendAudio> audio;unsigned& seed;
    unsigned controller;bool wide,busy=false,failed=false;std::thread::id thread=std::this_thread::get_id();
    FrontendSession::Handle current;State state;FrontendPointerHost host;std::unique_ptr<FrontendHandler> handler;
    FrontendSession::Handle input_source;
    FrontendNavigation::TransitionCallback transition_callback;
    std::shared_ptr<FrontendPointerRegion> region;std::vector<Pending> pending;std::vector<FrontendAudioHandle> sounds;
    Implementation(std::shared_ptr<FrontendSession> s,FrontendInput& i,std::shared_ptr<FrontendAudio> a,unsigned& r,bool w,unsigned c)
        :session(std::move(s)),input(i),audio(std::move(a)),seed(r),controller(c),wide(w),host(i,c)
    {
        CheckNav(session&&audio&&audio->Loaded()&&controller<4,"NAV requires retained scene, audio and valid controller");
        current=session->Current();CheckNav(current&&current->request.animate&&current->request.image_profile==FrontendImageProfile::Main,"NAV requires animated Main-profile resources");
        pending.reserve(16);handler=std::make_unique<FrontendHandler>(session,input);
    }
    void Ready()const{CheckNav(thread==std::this_thread::get_id()&&session&&!failed,"NAV requires its live nonfailed owner thread");}
    void Mutable()const{Ready();CheckNav(!busy&&!nlGetCurrentAsyncRead(),"NAV recursive or NL-callback mutation is unsupported");}
    void Expected(const FrontendSession::Handle& f)const{Mutable();CheckNav(f&&f==current&&f==session->Current(),"NAV requires its exact current frame");}
    const FrontendSession::Handle& Presented()const{return input_source?input_source:current;}
    void Play(const std::vector<std::uint32_t>& cues)
    {
        const auto live=audio->Handles();std::erase_if(sounds,[&](auto h){return std::find(live.begin(),live.end(),h)==live.end();});
        sounds.reserve(sounds.size()+cues.size());for(auto cue:cues)if(auto h=audio->Play(cue,seed))sounds.push_back(*h);
    }
    void CancelSounds()
    {
        const auto live=audio->Handles();std::exception_ptr error;
        for(auto h:sounds)if(std::find(live.begin(),live.end(),h)!=live.end())try{audio->Cancel(h);}catch(...){if(!error)error=std::current_exception();}
        sounds.clear();if(error)std::rethrow_exception(error);
    }
};
FrontendNavigation::FrontendNavigation(std::shared_ptr<FrontendSession> session,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,unsigned& seed,bool wide,unsigned controller)
    :impl_(std::make_unique<Implementation>(std::move(session),input,std::move(audio),seed,wide,controller))
{
    auto& s=*impl_;State next;
    s.session->HandlerTransaction(s.current,[&](auto& playback){
        Step step(playback,{});
        auto* timer=FrontendNavigationCreated<Step::Node,Step::Node,Step::Finder>(step,[&]{return wide;},
            [](char* name,unsigned size,int index){std::snprintf(name,size,"cursor%d",index);},
            [](int,Step::Node* instance){CheckNav(instance&&instance->root.id,"NAV required cursor is absent");});
        CheckNav(timer&&timer->root.id,"NAV authored timer is absent");
        step.mTimer=Step::Finder<Step::Node,3>::Find(timer->GetActiveSlide(),"Timer");
        CheckNav(step.mTimer&&step.mTimer->Value().type==3,"NAV authored Timer text is absent");step.mTimer->SetVisible(false);
        FrontendNavigationRestoreButtonVisibility(step);
        FrontendBackSetInstance<Step::Node,Step::Node,Step::Finder>(step,step.mBackButton,[&]{return wide;});
        next=step.Result();
    });
    s.state=std::move(next);s.current=s.session->Current();
}
FrontendNavigation::~FrontendNavigation(){try{Release();}catch(...){std::terminate();}}
FrontendSession::Handle FrontendNavigation::Current()const{impl_->Ready();return impl_->current;}
FrontendNavigationStatus FrontendNavigation::Status()const
{const auto& s=*impl_;CheckNav(s.thread==std::this_thread::get_id()&&s.session,"NAV status requires live owner thread");auto out=s.state.status;out.failed=s.failed;return out;}
FrontendPointerBounds FrontendNavigation::Bounds()const{impl_->Ready();CheckNav(bool(impl_->region),"NAV bounds require acknowledged frame");return impl_->region->Bounds();}
void FrontendNavigation::Acknowledge(const FrontendSession::Handle& frame,FrontendPointerViewport viewport)
{
    auto& s=*impl_;s.Expected(frame);CheckNav(!s.input_source,"Cannot acknowledge NAV during input");CheckNav(viewport.widescreen==s.wide,"NAV pointer projection must match its authored aspect");
    try
    {
        if(!s.region)
        {
            Step step(frame->graph,s.state);
            FrontendBackBounds<Step::Node,Position,Step::Finder>(&step);auto next=step.Result();
            auto region=std::make_shared<FrontendPointerRegion>(s.input,frame,next.binding,[&s](auto kind,unsigned index,const auto& source){
                if(kind==FrontendPointerCallback::Enter||kind==FrontendPointerCallback::Leave||kind==FrontendPointerCallback::Inside||kind==FrontendPointerCallback::Press||kind==FrontendPointerCallback::Release)
                {CheckNav(s.pending.size()<16,"NAV callback budget exceeded");s.pending.push_back({kind,index,source});}});
            next.status.back_initialized=true;s.state=std::move(next);s.region=std::move(region);
        }
        else s.region->RebindFrame(frame);
        std::array regions{s.region};s.host.Publish(frame,viewport,regions);
    }
    catch(...){s.failed=true;throw;}
}
void FrontendNavigation::HideButtons(const FrontendSession::Handle& frame)
{
    auto& s=*impl_;s.Expected(frame);State next;
    s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state);FrontendNavigationHideButtons(step);next=step.Result();});s.state=std::move(next);s.current=s.session->Current();
}
void FrontendNavigation::SetButtons(const FrontendSession::Handle& frame,unsigned mask,bool enabled)
{
    auto& s=*impl_;s.Expected(frame);CheckNav(mask<=255,"NAV mask exceeds eight original buttons");State next;
    s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state);FrontendNavigationSetButtons<Step::Node,Step::Node,Step::Finder>(step,int(mask),enabled);next=step.Result();});s.state=std::move(next);s.current=s.session->Current();
}
void FrontendNavigation::SetDoneButtonText(const FrontendSession::Handle& frame,unsigned value)
{
    auto& s=*impl_;s.Expected(frame);CheckNav(value<=1,"NAV Done label is unsupported");
    s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state);
        CheckNav(step.mDoneButton->root.id&&step.mDoneButton->Value().type==4,"NAV Done component is absent");
        for(const char* slide:{"off","over","down"})
        {auto* text=Step::Finder<Step::Node,3>::Find(step.mDoneButton,slide,"Group","done");CheckNav(text&&text->root.id&&text->Value().type==3,"NAV Done label is absent or mistyped");}
        step.SetDoneButtonText(int(value));});s.current=s.session->Current();
}
void FrontendNavigation::SetDoneButtonSlide(const FrontendSession::Handle& frame,FrontendNavigationDoneSlide value)
{
    auto& s=*impl_;s.Expected(frame);
    CheckNav(value==FrontendNavigationDoneSlide::Off||value==FrontendNavigationDoneSlide::Over||value==FrontendNavigationDoneSlide::Down,"NAV Done slide is unsupported");
    const char* name=value==FrontendNavigationDoneSlide::Off?"off":value==FrontendNavigationDoneSlide::Over?"over":"down";
    s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state);
        const auto& node=step.mDoneButton->Value();CheckNav(step.mDoneButton->root.id&&node.type==4&&node.library,"NAV Done component is absent or mistyped");
        CheckNav(playback.SelectComponent(*node.library,name,true,false),"NAV Done feedback slide is absent");});s.current=s.session->Current();
}
void FrontendNavigation::CheckDoneInput(const FrontendSession::Handle& frame)const
{
    auto& s=*impl_;s.Ready();CheckNav(!s.busy&&frame&&s.input_source==frame&&s.host.Current()&&s.host.Current()->Frame()==frame,
        "Done routing requires NAV's exact presented-input window");
}
FrontendNavigationDoneBinding FrontendNavigation::DoneButton(const FrontendSession::Handle& frame)const
{
    auto& s=*impl_;s.Ready();
    CheckNav(frame&&s.host.Current()&&s.host.Current()->Frame()==frame,"NAV Done binding requires its actual acknowledged frame");
    const auto component=s.state.buttons[5];
    const auto it=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& n){return n.offset==component;});
    CheckNav(component&&it!=frame->graph.instances.end()&&it->type==4&&it->library,"NAV Done binding has no authored component");
    struct Bounds
    {
        FrontendPointerBounds value;
        void SetBounds(float x0,float x1,float y1,float y0){value.min_x=x0;value.max_x=x1;value.max_y=y1;value.min_y=y0;}
    } bounds;
    const auto library=std::find_if(frame->graph.library.begin(),frame->graph.library.end(),[&](const auto& n){return n.offset==*it->library;});
    CheckNav(library!=frame->graph.library.end()&&library->type==3,"NAV Done component library is absent");
    FrontendDoneButtonBounds(&bounds,0);return{frame,component,bounds.value,it->visible&&library->attributes.visible};
}
FrontendNavigationBackBinding FrontendNavigation::BackButton(const FrontendSession::Handle& frame)const
{
    auto& s=*impl_;s.Ready();CheckNav(frame&&s.host.Current()&&s.host.Current()->Frame()==frame&&s.region,
        "NAV Back binding requires its actual acknowledged frame");
    const auto component=s.state.buttons[2];
    const auto it=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& n){return n.offset==component;});
    CheckNav(component&&it!=frame->graph.instances.end()&&it->type==4&&it->library,"NAV Back component is absent or mistyped");
    const auto library=std::find_if(frame->graph.library.begin(),frame->graph.library.end(),[&](const auto& n){return n.offset==*it->library;});
    CheckNav(library!=frame->graph.library.end()&&library->type==3,"NAV Back component library is absent");
    // The root component is toggled by original NAV SetButtons. Also require
    // actual published child geometry; invisible/unavailable ancestry cannot
    // make a keyboard shortcut target merely stored source bounds.
    std::vector<std::uint32_t> descendants{component};bool rendered=false;
    for(std::size_t at=0;at<descendants.size();++at)
    {
        CheckNav(descendants.size()<=frame->graph.instances.size(),"NAV Back ancestry exceeds scene bounds");
        const auto node=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& n){return n.offset==descendants[at];});
        CheckNav(node!=frame->graph.instances.end(),"NAV Back ancestry instance absent");
        for(const auto& entry:frame->layout.entries)std::visit([&](const auto& draw){if(draw.instance==node->offset&&draw.colour[3])rendered=true;},entry);
        descendants.insert(descendants.end(),node->children.begin(),node->children.end());
        if(node->type==4&&node->library)
        {
            const auto lib=std::find_if(frame->graph.library.begin(),frame->graph.library.end(),[&](const auto& n){return n.offset==*node->library;});
            CheckNav(lib!=frame->graph.library.end(),"NAV Back child library absent");
            if(lib->active_slide)
            {
                const auto slide=std::find_if(frame->graph.slides.begin(),frame->graph.slides.end(),[&](const auto& n){return n.offset==*lib->active_slide;});
                CheckNav(slide!=frame->graph.slides.end(),"NAV Back active child slide absent");
                descendants.insert(descendants.end(),slide->children.begin(),slide->children.end());
            }
        }
    }
    return{frame,component,s.region->Bounds(),it->visible&&library->attributes.visible&&rendered};
}
void FrontendNavigation::SetPointerSlide(const FrontendSession::Handle& frame,unsigned index,FrontendNavigationPointer value)
{
    auto& s=*impl_;s.Expected(frame);CheckNav(index<4&&(value==FrontendNavigationPointer::Waiting||value==FrontendNavigationPointer::Cursor||value==FrontendNavigationPointer::Accept),"NAV pointer selection is unsupported");
    const char* name=value==FrontendNavigationPointer::Waiting?"waiting":value==FrontendNavigationPointer::Cursor?"cursor":"A";
    s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state);step.mPointerInstances[index]->SetActiveSlide(name,true,false);});s.current=s.session->Current();
}
void FrontendNavigation::SetDesktopPointersEnabled(const FrontendSession::Handle& frame,bool enabled)
{
    auto& s=*impl_;s.Expected(frame);
    UpdatePointers(frame,{},!enabled);s.state.status.pointer_input_enabled=enabled;
}
void FrontendNavigation::UpdatePointers(const FrontendSession::Handle& frame,const std::array<FrontendNavigationPointerSample,4>& samples,bool hidden)
{
    auto& s=*impl_;s.Expected(frame);
    for(const auto& sample:samples)for(float v:sample.position)CheckNav(std::isfinite(v)&&std::abs(v)<=1e7,"NAV pointer position exceeds finite profile");
    s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state);for(unsigned i=0;i<4;++i){const auto& p=samples[i];struct Vec{float x,y;} position{p.position[0],p.position[1]};
        FrontendNavigationPointerPose(step.mPointerInstances[i],position,p.angle,p.valid,hidden,[](unsigned short angle){return AngUnitsToRad_fromUnsignedShort(angle);});}});s.current=s.session->Current();s.state.status.pointer_hidden=hidden;
}
void FrontendNavigation::StartTransition(const FrontendSession::Handle& frame,std::string function,TransitionCallback callback)
{
    auto& s=*impl_;s.Expected(frame);
    CheckNav(!s.state.status.transition_playing&&callback&&!function.empty()&&function.size()<=128&&function.find('\0')==std::string::npos,"NAV transition requires a bounded real callback and no active transition");
    State next;s.session->HandlerTransaction(frame,[&](auto& playback){
        Step step(playback,s.state);CheckNav(step.mTransition->root.id,"NAV transition component is absent");
        // Source Find is unchecked. Validate every mandatory original target
        // before entering the shared body, never substitute an empty component.
        const auto validate=[&](Step::Node* node){CheckNav(node&&node->root.id&&node->Value().type==4&&node->Value().library,"NAV transition requires its authored component");
            const auto& libraries=playback.Scene().library;auto it=std::find_if(libraries.begin(),libraries.end(),[&](const auto& v){return v.offset==*node->Value().library;});CheckNav(it!=libraries.end(),"NAV transition library is absent");
            const auto slide=std::find_if(playback.Scene().slides.begin(),playback.Scene().slides.end(),[&](const auto& v){return v.hash==FrontendLowerHash("Slide1")&&std::find(it->slides.begin(),it->slides.end(),v.offset)!=it->slides.end();});CheckNav(slide!=playback.Scene().slides.end(),"NAV transition Slide1 is absent");};
        validate(step.mTransition);
        for(unsigned i=1;i<=8;++i){char name[16];std::snprintf(name,sizeof(name),"door_%u",i);validate(Step::Finder<Step::Node,4>::Find(step.mTransition,"Slide1","Group",name));}
        FrontendNavigationStartTransition<Step::Node,Step::Node,Step::Finder>(step);step.state.status.transition_function=function;next=step.Result();
    });
    s.state=std::move(next);s.current=s.session->Current();s.transition_callback=std::move(callback);
}
void FrontendNavigation::AdvanceVisual(const FrontendSession::Handle& frame,float delta)
{
    auto& s=*impl_;s.Expected(frame);CheckNav(!s.input_source,"Cannot advance NAV during input");CheckNav(s.pending.empty(),"NAV pointer callbacks must finish before advancement");
    s.handler->Update(frame,delta);s.current=s.session->Current();if(!s.state.status.transition_playing)return;
    Busy guard(s.busy);
    try
    {
        const bool call=s.state.status.transition_pending;
        if(call)
        {
            State next;s.session->HandlerTransaction(s.current,[&](auto& playback){Step step(playback,s.state);FrontendNavigationShowTransition(step);next=step.Result();});
            s.state=std::move(next);s.current=s.session->Current();
        }
        const auto invoke=[&]{if(call){CheckNav(bool(s.transition_callback),"NAV real transition callback is absent");const auto expected=s.current;s.transition_callback(s.state.status.transition_function);CheckNav(s.session->Current()==expected,"NAV callback mutated its retained session");}};
        Step inspect(s.current->graph,s.state);
        if(float(inspect.mTransition->GetActiveSlide()->m_time)>=.6f)
        {
            State next;
            s.session->HandlerTransaction(s.current,[&](auto& playback){Step step(playback,s.state);FrontendNavigationFinishTransition(step,step.state.status.pointer_input_enabled,step.state.status.pointer_hidden);next=step.Result();},invoke);
            s.state=std::move(next);s.current=s.session->Current();s.transition_callback={};
        }
        else invoke();
    }
    catch(...){s.failed=true;throw;}
}
bool FrontendNavigation::ApplyPending(unsigned index)
{
    auto& s=*impl_;s.Expected(s.current);CheckNav(index<4,"NAV pointer exceeds four");const auto frame=s.current,presented=s.Presented();Busy guard(s.busy);bool pressed=false;
    try
    {
        State next;std::vector<std::uint32_t> cues;
        s.session->HandlerTransaction(frame,[&](auto& playback){Step step(playback,s.state);step.mPointerInside[index]=false;step.state.status.speaker_context=index+1;
            const auto play=[&](unsigned long cue,const char* name,void* context,bool restart){CheckNav(cue<=UINT32_MAX&&!name&&!context&&restart,"NAV audio profile is unsupported");cues.push_back(std::uint32_t(cue));};
            const auto empty=[](int,void*){}; // Source base callbacks are genuinely unset for this FEBackButton.
            for(const auto& event:s.pending)
            {
                CheckNav(event.frame==presented&&event.index==index,"NAV callback received a stale pointer/frame");
                switch(event.kind)
                {
                case FrontendPointerCallback::Enter:FrontendBackEnter(step,int(index),nullptr,play,[&](int,void*){++step.state.status.hover_feedback_requests;});break;
                case FrontendPointerCallback::Leave:FrontendBackLeave(step,int(index),nullptr,empty);break;
                case FrontendPointerCallback::Inside:FrontendBackInside(step,int(index));break;
                case FrontendPointerCallback::Press:FrontendBackPress(step,int(index),nullptr,play,empty);break;
                case FrontendPointerCallback::Release:FrontendBackRelease(step,int(index),nullptr,empty);break;
                default:break;
                }
            }
            pressed=FrontendBackFinish<-2>(step,[](int){throw std::logic_error("NAV manager Push is unavailable");},[]{throw std::logic_error("NAV manager Pop is unavailable");});next=step.Result();
        },[&]{s.Play(cues);});
        s.pending.clear();s.state=std::move(next);s.current=s.session->Current();return pressed;
    }
    catch(...){s.failed=true;throw;}
}
bool FrontendNavigation::DeliverPointer(const FrontendSession::Handle& frame,const FrontendPointerEvent& event)
{
    auto& s=*impl_;s.Expected(s.current);CheckNav(frame&&frame==s.Presented()&&s.host.Current()&&s.host.Current()->Frame()==frame,"NAV input requires acknowledged frame");CheckNav(event.index<4,"NAV pointer exceeds four");
    try{s.region->Deliver(frame,event);return ApplyPending(event.index);}catch(...){s.failed=true;throw;}
}
FrontendNavigationDispatch FrontendNavigation::Route(const FrontendPointerDesktopSample& sample)
{
    auto& s=*impl_;s.Expected(s.current);CheckNav(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"NAV input requires acknowledged frame");
    try{auto out=s.host.Route(s.host.Current(),sample);return {out,ApplyPending(out.event.index)};}catch(...){s.failed=true;throw;}
}
FrontendNavigationDispatch FrontendNavigation::Poll(SDL_Window* window,bool capture)
{
    auto& s=*impl_;s.Expected(s.current);CheckNav(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"NAV input requires acknowledged frame");
    try
    {
        CheckNav(window&&SDL_GetWindowID(window)==s.host.Current()->Viewport().window,"NAV input received a different window");
        int w=0,h=0,pw=0,ph=0;CheckNav(SDL_GetWindowSize(window,&w,&h)&&SDL_GetWindowSizeInPixels(window,&pw,&ph),"Cannot query NAV window extent");
        const auto& v=s.host.Current()->Viewport();
        if(w<=0||h<=0||pw<=0||ph<=0||unsigned(w)!=v.window_width||unsigned(h)!=v.window_height||unsigned(pw)!=v.pixel_width||unsigned(ph)!=v.pixel_height)
        {const FrontendPointerEvent leave{s.controller,{-999,-999}};DeliverPointer(s.Presented(),leave);s.host.Reset();return {{leave,false,1},false};}
        auto out=s.host.Poll(s.host.Current(),window,capture);return {out,ApplyPending(out.event.index)};
    }
    catch(...){s.failed=true;throw;}
}
void FrontendNavigation::WithPresentedInput(const FrontendSession::Handle& frame,const std::function<void()>& operation)
{
    auto& s=*impl_;s.Expected(s.current);
    CheckNav(!s.input_source&&operation&&frame&&s.host.Current()&&s.host.Current()->Frame()==frame,
        "NAV input window requires its exact acknowledged presentation");
    CheckNav(frame->visuals==s.current->visuals&&frame->images==s.current->images&&frame->request.path==s.current->request.path,
        "NAV input window cannot cross resource generations");
    s.input_source=frame;
    struct Reset{Implementation& s;~Reset(){s.input_source.reset();}}reset{s};
    try{operation();CheckNav(s.current==s.session->Current(),"NAV input bypassed its retained visual owner");}
    catch(...){s.failed=true;throw;}
}
void FrontendNavigation::Release()
{
    auto& s=*impl_;CheckNav(s.thread==std::this_thread::get_id()&&!s.busy&&!s.input_source&&!nlGetCurrentAsyncRead(),"NAV release requires idle owner thread");if(!s.session)return;Busy guard(s.busy);
    s.transition_callback={};s.host.Release();s.region.reset();s.pending.clear();if(s.handler)s.handler->Release();s.handler.reset();s.CancelSounds();s.current.reset();s.session.reset();s.audio.reset();s.state={};
}
}
