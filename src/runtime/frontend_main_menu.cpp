#include "runtime/frontend_main_menu.h"
#include "runtime/frontend_handler.h"
#include "resources/frontend_animation.h"
#include "Game/FE/FrontendMainMenuSteps.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <map>
#include <thread>

namespace mscharged
{
namespace
{
using namespace resources;
void CheckMenu(bool okay,const char* message){if(!okay)throw std::logic_error(message);}
struct Position { struct { float x,y,z; } f; };
struct MenuState
{
    std::array<std::uint32_t,7> items{},highlights{},arrows{},groups{},hits{};
    std::uint32_t description=0;
    std::array<FrontendPointerBinding,7> bindings{};
    FrontendMainMenuStatus status;
};
struct MenuStep
{
    FrontendAnimationPlayback& playback;
    MenuState state;
    bool default_arrow_lookup=false;
    struct EmptySlide {float m_time=0;void Update(float){throw std::logic_error("Original default arrow must have no slides");}};
    struct EmptyComponent {EmptySlide* pChildren=nullptr;EmptySlide* m_pActiveSlide=nullptr;} default_arrow;
    struct Slide
    {
        MenuStep& owner;std::uint32_t id;
        FrontendNode Root()const{return {FrontendNodeKind::Slide,id};}
    };
    struct Instance
    {
        MenuStep& owner;std::uint32_t id;
        FrontendNode Root()const{return {FrontendNodeKind::Instance,id};}
        const FrontendInstance& Value()const
        {
            const auto& values=owner.playback.Scene().instances;
            const auto found=std::find_if(values.begin(),values.end(),[&](const auto& v){return v.offset==id;});
            CheckMenu(found!=values.end(),"Main Menu instance is absent");return *found;
        }
        const FrontendLibraryObject& Component()const
        {
            const auto& value=Value();CheckMenu(value.type==4&&value.library,"Main Menu component is absent");
            const auto& values=owner.playback.Scene().library;
            const auto found=std::find_if(values.begin(),values.end(),[&](const auto& v){return v.offset==*value.library;});
            CheckMenu(found!=values.end()&&found->type==3,"Main Menu component library is absent");return *found;
        }
        Slide* GetActiveSlide(){const auto id=Component().active_slide;CheckMenu(bool(id),"Main Menu component has no active slide");return owner.SlideAt(*id);}
        Position GetAssetPosition()const{const auto& p=Value().attributes.position;return {{p[0],p[1],p[2]}};}
        void SetVisible(bool flag)
        {FrontendInstanceChange change;change.instance=id;change.flag=flag;owner.playback.Apply(std::span(&change,1));}
        void SetActiveSlide(const char* name,bool reset,bool preserve)
        {
            if(!id)
            {
                // Original TLDefault's static component is zero-initialized
                // with an empty slide ring. Execute the shared selection body:
                // missing name clears active and performs no slide Update.
                FrontendMainComponentSelect<EmptySlide>(owner.default_arrow,FrontendLowerHash(name),reset,preserve,
                    [](EmptySlide* list,unsigned long)->EmptySlide*{CheckMenu(!list,"Default arrow has an unexpected slide ring");return nullptr;});
                return;
            }
            CheckMenu(owner.playback.SelectComponent(Component().offset,name,reset,preserve),"Main Menu authored feedback slide is absent");
        }
    };
    struct Presentation
    {
        MenuStep& owner;
        FrontendNode Root()const{return {};}
        void SetActiveSlide(const char* name,bool reset)
        {CheckMenu(owner.playback.SelectPresentation(name,reset),"Main Menu authored MAIN slide is absent");}
        Slide* GetActiveSlide(){const auto id=owner.playback.Scene().active_slide;CheckMenu(bool(id),"Main Menu presentation has no active slide");return owner.SlideAt(*id);}
    } presentation{*this};
    struct Button
    {
        std::array<int,4> states{};FrontendPointerBinding binding;
        bool HasOtherPointerState(int value,unsigned which)const{return FrontendPointerHasOtherState(states,value,which);}
        void SetPointerState(int value,int which){CheckMenu(which>=0&&which<4,"Main pointer index exceeds four");states[which]=value;}
        void SetInstanceBounds(Instance* image,bool rotation,float x,float y,float sx,float sy)
        {binding={image->id,rotation,x,y,sx,sy};}
    };
    static constexpr int NUM_ITEMS=7;
    std::map<std::uint32_t,Instance> instances;
    std::map<std::uint32_t,Slide> slides;
    std::array<Instance*,7> mMenuItemInstances{},mMenuHighlights{},mMenuArrows{},mMenuGroups{},mMenuHitImages{};
    Instance* mItemDescription=nullptr;
    std::array<Button,7> mMenuItems;
    std::array<int,4> mHighlightedItemCounts;
    MenuStep(FrontendAnimationPlayback& p,MenuState value):playback(p),state(std::move(value)),mHighlightedItemCounts(state.status.highlighted)
    {
        for(unsigned i=0;i<7;++i)
        {
            mMenuItems[i].states=state.status.pointer_states[i];mMenuItems[i].binding=state.bindings[i];
            if(state.items[i])
            {
                mMenuItemInstances[i]=InstanceAt(state.items[i]);mMenuHighlights[i]=InstanceAt(state.highlights[i]);
                mMenuArrows[i]=InstanceAt(state.arrows[i]);mMenuGroups[i]=InstanceAt(state.groups[i]);mMenuHitImages[i]=InstanceAt(state.hits[i]);
            }
        }
        if(state.description)mItemDescription=InstanceAt(state.description);
    }
    Instance* InstanceAt(std::uint32_t id){return &instances.try_emplace(id,Instance{*this,id}).first->second;}
    Slide* SlideAt(std::uint32_t id){return &slides.try_emplace(id,Slide{*this,id}).first->second;}
    Presentation* GetPresentation(){return &presentation;}
    template<class T,int Type> struct Finder
    {
        template<class U> static T* Find(U* root,const char* first,const char* second=nullptr)
        {
            CheckMenu(root,"Main Menu lookup root is absent");
            const std::array<std::string_view,2> names{first,second?second:""};
            const auto count=second?2:1;
            const auto node=FindFrontendNode(root->owner.playback.Scene(),root->Root(),FrontendNamedPath(std::span(names.data(),count)),static_cast<FrontendNodeType>(Type));
            if(!node||node->kind!=FrontendNodeKind::Instance)
                throw std::logic_error(std::string("Main Menu authored component path is absent: ")+first
                    +(second?std::string("/")+second:std::string{})+" at "+std::to_string(root->Root().id)
                    +" (type "+std::to_string(Type)+"); default substitutes are unavailable");
            return root->owner.InstanceAt(node->id);
        }
        template<class U> static T* FindOrDefault(U* root,const char* a,const char* b=nullptr)
        {
            if constexpr(Type==4)
            {
                if(root->owner.default_arrow_lookup&&std::string_view(a)=="arrows"&&!b)
                {
                    const std::array<std::string_view,1> names{a};
                    const auto node=FindFrontendNode(root->owner.playback.Scene(),root->Root(),FrontendNamedPath(names),FrontendNodeType::Component);
                    return root->owner.InstanceAt(node?node->id:0);
                }
            }
            return Find(root,a,b);
        }
    };
    void Created()
    {
        // Current native language profile covers USA En/Fr/Es; Japanese remains
        // outside it. Original hidden Japanese item and source lookup still run.
        FrontendMainMenuVisualCreated<Instance,Instance,Instance,Instance,Finder>(*this,false);
        default_arrow_lookup=true;
        FrontendMainMenuFindArrows<Instance,Finder>(*this);
        default_arrow_lookup=false;
    }
    void Bind(){for(int i=0;i<7;++i)FrontendMainMenuBind<Position>(*this,i);state.status.interactive=true;}
    MenuState Result()
    {
        for(unsigned i=0;i<7;++i)
        {
            state.items[i]=mMenuItemInstances[i]->id;state.highlights[i]=mMenuHighlights[i]->id;state.arrows[i]=mMenuArrows[i]->id;
            state.groups[i]=mMenuGroups[i]->id;state.hits[i]=mMenuHitImages[i]->id;
            state.status.pointer_states[i]=mMenuItems[i].states;state.bindings[i]=mMenuItems[i].binding;
            state.status.default_arrows[i]=mMenuArrows[i]->id==0;
        }
        state.description=mItemDescription->id;state.status.highlighted=mHighlightedItemCounts;return state;
    }
};
struct PendingEvent{FrontendPointerCallback kind;unsigned item,index;FrontendSession::Handle frame;};
struct Mutation
{bool& value;explicit Mutation(bool& v):value(v){value=true;}~Mutation(){value=false;}};
}
struct FrontendMainMenu::Implementation
{
    std::shared_ptr<FrontendSession> session;
    FrontendInput& input;
    std::shared_ptr<FrontendAudio> audio;
    unsigned& seed;bool media;
    std::thread::id thread=std::this_thread::get_id();
    FrontendSession::Handle current;
    MenuState state;
    std::array<FrontendPointerBounds,7> bounds{};
    FrontendPointerHost host;
    std::shared_ptr<FrontendHandler> handler;
    std::array<std::shared_ptr<FrontendPointerRegion>,7> regions{};
    std::vector<PendingEvent> pending;
    std::vector<FrontendAudioHandle> sounds;
    bool failed=false,busy=false,stack_attached=false,stack_update=false,input_window=false;
    FrontendSession::Handle input_source;
    Implementation(std::shared_ptr<FrontendSession> s,FrontendInput& i,std::shared_ptr<FrontendAudio> a,unsigned& rng,bool m)
        :session(std::move(s)),input(i),audio(std::move(a)),seed(rng),media(m),host(i)
    {
        CheckMenu(session&&audio&&audio->Loaded(),"Main Menu requires retained scene and resident frontend audio");
        current=session->Current();CheckMenu(current&&current->request.animate&&current->request.image_profile==FrontendImageProfile::Main,"Main Menu requires animated Main resource profile");
        pending.reserve(32);
    }
    void Ready()const
    {CheckMenu(thread==std::this_thread::get_id(),"Main Menu requires its creating thread");CheckMenu(session&&!failed,"Main Menu is unavailable after failure/release");}
    void Mutable()const{Ready();CheckMenu(!busy&&nlGetCurrentAsyncRead()==nullptr&&(!stack_attached||stack_update),"Main Menu mutation requires its idle owner or controlled stack update");}
    void Expected(const FrontendSession::Handle& value,bool acknowledge=false)const
    {if(acknowledge){Ready();CheckMenu(!busy&&!stack_update&&!nlGetCurrentAsyncRead(),"Cannot acknowledge Main during update");}else Mutable();CheckMenu(value&&value==current&&session->Current()==current,"Main Menu requires its exact current frame");}
    void Queue(FrontendPointerCallback kind,unsigned item,unsigned index,const FrontendSession::Handle& frame)
    {
        if(kind!=FrontendPointerCallback::Enter&&kind!=FrontendPointerCallback::Leave&&kind!=FrontendPointerCallback::Press)return;
        CheckMenu(pending.size()<32,"Main Menu pointer callback budget exceeded");pending.push_back({kind,item,index,frame});
    }
    FrontendSession::Handle Presented()const{return input_window?input_source:current;}
    void InputExpected(const FrontendSession::Handle& frame)const
    {Expected(current);CheckMenu(frame&&frame==Presented(),"Pointer input requires the last acknowledged stack frame");}
    bool RegionsPresented()const{return std::all_of(regions.begin(),regions.end(),[&](const auto& region){return region&&region->Current()==Presented();});}
    bool CanRoute()const{return state.status.interactive&&!state.status.selection&&RegionsPresented();}
    void CancelSounds()
    {
        std::exception_ptr error;const auto live=audio->Handles();
        for(auto handle:sounds)if(std::find(live.begin(),live.end(),handle)!=live.end())
            try{audio->Cancel(handle);}catch(...){if(!error)error=std::current_exception();}
        sounds.clear();if(error)std::rethrow_exception(error);
    }
};
FrontendMainMenu::FrontendMainMenu(std::shared_ptr<FrontendSession> session,FrontendInput& input,
    std::shared_ptr<FrontendAudio> audio,unsigned& seed,bool media)
    :FrontendMainMenu(std::move(session),input,std::move(audio),seed,{},media){}
FrontendMainMenu::FrontendMainMenu(std::shared_ptr<FrontendSession> session,FrontendInput& input,
    std::shared_ptr<FrontendAudio> audio,unsigned& seed,std::shared_ptr<FrontendHandler> base,bool media)
    :impl_(std::make_unique<Implementation>(std::move(session),input,std::move(audio),seed,media))
{
    auto& s=*impl_;CheckMenu(!base||base->Binds(s.session),"Main shared handler belongs to another session");
    s.handler=base?std::move(base):std::make_shared<FrontendHandler>(s.session,input);MenuState next;
    s.session->HandlerTransaction(s.current,[&](auto& playback){MenuStep step(playback,{});step.Created();next=step.Result();});
    s.state=std::move(next);s.current=s.session->Current();
}
FrontendMainMenu::~FrontendMainMenu(){try{Release();}catch(...){std::terminate();}}
FrontendSession::Handle FrontendMainMenu::Current()const{impl_->Ready();return impl_->current;}
FrontendMainMenuStatus FrontendMainMenu::Status()const
{CheckMenu(impl_->thread==std::this_thread::get_id()&&bool(impl_->session),"Main Menu status requires a live owner thread");auto value=impl_->state.status;value.failed=impl_->failed;return value;}
std::array<FrontendPointerBounds,7> FrontendMainMenu::Bounds()const
{auto& s=*impl_;s.Ready();CheckMenu(s.state.status.interactive,"Main Menu pointer initialization waits for authored intro completion");return s.bounds;}
void FrontendMainMenu::Acknowledge(const FrontendSession::Handle& frame,FrontendPointerViewport viewport)
{
    auto& s=*impl_;s.Expected(frame,true);
    try
    {
        if(s.state.status.interactive)
        {
            for(unsigned i=0;i<7;++i)
                s.regions[i]->RebindFrame(frame);
            s.host.Publish(frame,viewport,s.regions);
        }
        else s.host.Publish(frame,viewport,{});
    }
    catch(...){s.failed=true;throw;}
}
// Listener callbacks only collect commands. Process source mutations once the
// host has completed every listener against the same acknowledged generation.
void FrontendMainMenu::ApplyPending()
{
    auto& s=*impl_;s.Expected(s.current);if(s.pending.empty())return;
    const auto frame=s.current,presented=s.Presented();
    Mutation mutation(s.busy);
    try
    {
        std::vector<PendingEvent> events;events.swap(s.pending);s.pending.reserve(32);
        MenuState next=s.state;std::vector<std::uint32_t> cues;cues.reserve(events.size());
        s.session->HandlerTransaction(frame,[&](auto& playback){
            MenuStep step(playback,next);
            const auto play=[&](unsigned long cue,const void* name,void* context,bool restartable){CheckMenu(!name&&!context&&restartable,"Main Menu audio request exceeds null-context profile");CheckMenu(cue<=UINT32_MAX,"Main Menu audio cue exceeds console width");cues.push_back(std::uint32_t(cue));};
            for(const auto& pending:events)
            {
                CheckMenu(pending.frame==presented,"Main Menu callback targets a different presented frame");
                if(step.state.status.selection)break;
                if(pending.kind==FrontendPointerCallback::Enter)FrontendMainMenuOpen(step,pending.index,pending.item,play);
                else if(pending.kind==FrontendPointerCallback::Leave)FrontendMainMenuClose(step,pending.index,pending.item);
                else FrontendMainMenuSelect(pending.item,[&]{return s.media;},play,
                    [&]{step.state.status.selection=FrontendMainSelection{pending.item,pending.index,FrontendMainSelectionService::OnlineSaveMii,presented};},
                    [&](unsigned item){step.state.status.selection=FrontendMainSelection{item,pending.index,FrontendMainSelectionService::ApplyItem,presented};});
            }
            FrontendMainMenuDescription(step.state.status.selection&&step.state.status.selection->service==FrontendMainSelectionService::ApplyItem,
                step.mHighlightedItemCounts[events.back().index],[&](bool visible){step.mItemDescription->SetVisible(visible);});
            next=step.Result();
        },[&]{
            const auto live=s.audio->Handles();std::erase_if(s.sounds,[&](auto h){return std::find(live.begin(),live.end(),h)==live.end();});
            s.sounds.reserve(s.sounds.size()+cues.size());
            for(auto cue:cues)if(auto handle=s.audio->Play(cue,s.seed))s.sounds.push_back(*handle);
        });
        s.state=std::move(next);s.current=s.session->Current();
    }
    catch(...){s.failed=true;throw;}
}
void FrontendMainMenu::DeliverPointer(const FrontendSession::Handle& frame,const FrontendPointerEvent& event)
{
    auto& s=*impl_;s.InputExpected(frame);CheckMenu(s.host.Current()&&s.host.Current()->Frame()==frame,"Main Menu input has no matching acknowledged presentation");
    if(!s.CanRoute())return;
    CheckMenu(event.index<4,"Main Menu pointer index exceeds four");
    try{for(const auto& region:s.regions)region->Deliver(frame,event);ApplyPending();}
    catch(...){s.failed=true;throw;}
}
FrontendPointerDispatch FrontendMainMenu::Route(const FrontendPointerDesktopSample& sample)
{
    auto& s=*impl_;s.Expected(s.current);CheckMenu(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Main Menu input has no matching acknowledged presentation");
    if(!s.CanRoute())return {};
    try
    {
        auto result=s.host.Route(s.host.Current(),sample);
        ApplyPending();
        return result;
    }
    catch(...){s.failed=true;throw;}
}
FrontendPointerDispatch FrontendMainMenu::Poll(SDL_Window* window,bool capture)
{
    auto& s=*impl_;s.Expected(s.current);CheckMenu(s.host.Current()&&s.host.Current()->Frame()==s.Presented(),"Main Menu input has no matching acknowledged presentation");
    if(!s.CanRoute())return {};
    try
    {
        CheckMenu(window&&SDL_GetWindowID(window)==s.host.Current()->Viewport().window,"Main Menu input received a different window");
        int width=0,height=0,pixel_width=0,pixel_height=0;
        CheckMenu(SDL_GetWindowSize(window,&width,&height)&&SDL_GetWindowSizeInPixels(window,&pixel_width,&pixel_height),"Cannot query Main Menu window extent");
        const auto& v=s.host.Current()->Viewport();
        if(width<=0||height<=0||pixel_width<=0||pixel_height<=0||unsigned(width)!=v.window_width
            ||unsigned(height)!=v.window_height||unsigned(pixel_width)!=v.pixel_width||unsigned(pixel_height)!=v.pixel_height)
        {
            // Complete genuine Leave callbacks against the old visible frame;
            // the replacement viewport cannot accept input before its renderer
            // acknowledgement and a neutral observation.
            DeliverPointer(s.Presented(),{0,{-999,-999}});s.host.Reset();return {};
        }
        auto result=s.host.Poll(s.host.Current(),window,capture);ApplyPending();return result;
    }
    catch(...){s.failed=true;throw;}
}
void FrontendMainMenu::AdvanceVisual(const FrontendSession::Handle& frame,float delta)
{
    auto& s=*impl_;CheckMenu(!s.stack_attached,"Stack owns the only base update");s.Expected(frame);CheckMenu(s.pending.empty(),"Main Menu must apply pointer callbacks before advancing visuals");
    AfterBaseUpdate(s.handler->UpdateOnce(frame,delta));
}
void FrontendMainMenu::AfterBaseUpdate(FrontendHandler::UpdateProof&& proof)
{
    auto& s=*impl_;s.Mutable();s.current=s.handler->ConsumeUpdate(std::move(proof),s.current);
    if(!s.state.status.interactive)
    {
        const auto& graph=s.current->graph;const auto slide=std::find_if(graph.slides.begin(),graph.slides.end(),[&](const auto& value){return value.offset==graph.active_slide;});
        CheckMenu(slide!=graph.slides.end(),"Main Menu intro slide is absent");
        if(slide->time>=slide->start+slide->duration)
        {
            MenuState next;
            s.session->HandlerTransaction(s.current,[&](auto& playback){MenuStep step(playback,s.state);step.Bind();next=step.Result();});
            s.state=std::move(next);s.current=s.session->Current();
            try
            {
                // Original InitializeMenuItems measures each listener once;
                // later feedback slides and animation only retag its frame.
                std::array<FrontendPointerBounds,7> bounds;
                std::array<std::shared_ptr<FrontendPointerRegion>,7> regions;
                for(unsigned i=0;i<7;++i)
                {
                    regions[i]=std::make_shared<FrontendPointerRegion>(s.input,s.current,s.state.bindings[i],
                        [&s,i](auto kind,unsigned index,const auto& source){s.Queue(kind,i,index,source);});
                    bounds[i]=regions[i]->Bounds();
                }
                s.bounds=bounds;
                s.regions=std::move(regions);
            }
            catch(...){s.failed=true;throw;}
        }
    }
}
std::shared_ptr<FrontendSession> FrontendMainMenu::StackSession()const{impl_->Ready();return impl_->session;}
std::shared_ptr<FrontendHandler> FrontendMainMenu::StackHandler()const{impl_->Ready();return impl_->handler;}
unsigned FrontendMainMenu::StackScene()const{return 1;}
void FrontendMainMenu::AttachStack()
{auto& s=*impl_;s.Mutable();CheckMenu(!s.stack_attached,"Visual owner already belongs to a stack");s.stack_attached=true;}
void FrontendMainMenu::UpdateStack(FrontendHandler::UpdateProof&& proof,const FrontendSession::Handle& presented,const std::function<void()>& input)
{
    auto& s=*impl_;s.Ready();CheckMenu(s.stack_attached&&!s.stack_update&&!s.busy&&!nlGetCurrentAsyncRead(),"Visual stack update requires its idle retained owner");
    CheckMenu(presented&&presented==s.current&&proof.Before()==presented&&proof.After()==s.session->Current(),"Visual stack proof/presentation mismatch");
    s.stack_update=true;s.input_source=presented;
    struct Reset{Implementation& s;~Reset(){s.input_window=false;s.input_source.reset();s.stack_update=false;}}reset{s};
    try{AfterBaseUpdate(std::move(proof));s.input_window=true;if(input)input();CheckMenu(s.current==s.session->Current(),"Stack input mutated the session outside its selected visual owner");}
    catch(...){s.failed=true;throw;}
}
void FrontendMainMenu::ReleaseStack()
{auto& s=*impl_;CheckMenu(!s.stack_update&&!s.busy,"Cannot remove active visual update");s.stack_attached=false;Release();}
void FrontendMainMenu::Release()
{
    auto& s=*impl_;CheckMenu(s.thread==std::this_thread::get_id()&&!s.busy,"Main Menu release requires idle owner thread");if(!s.session)return;CheckMenu(!s.stack_attached,"Stack owns selected visual teardown");Mutation guard(s.busy);
    s.host.Release();s.regions={};s.pending.clear();if(s.handler&&s.handler.use_count()==1)s.handler->Release();s.handler.reset();
    s.CancelSounds();s.current.reset();s.state={};s.session.reset();s.audio.reset();
}
}
