#include "runtime/frontend_handler.h"
#include "Game/FE/FrontendHandlerSteps.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool okay,const char* message){if(!okay)throw std::logic_error(message);}
std::atomic<std::uint64_t> next_screen{1};
struct DispatchGuard
{
    bool& busy;
    explicit DispatchGuard(bool& value):busy(value){busy=true;}
    ~DispatchGuard(){busy=false;}
};
}
struct FrontendHandler::Implementation
{
    FrontendHandler& owner;
    std::shared_ptr<FrontendSession> session;
    FrontendInput& input;
    const std::thread::id thread=std::this_thread::get_id();
    bool busy=false,exclusive=false;
    struct Scene
    {
        Implementation& handler;
        void Update(float delta)
        {
            if(handler.notification)
            {
                if(!handler.session->AdvanceLoadingNotification(delta,*handler.notification))handler.ClearNotification();
            }
            else handler.session->Advance(delta);
        }
    } scene{*this};
    struct Screen
    {
        Screen* m_next=nullptr;Screen* m_prev=nullptr;Scene* mFEScene=nullptr;
        ScreenId id;Activation activation;Implementation& handler;
        void OnActivate(){handler.edits=activation(handler.activation_frame,handler.owner);}
    };
    Scene* mFEScene=&scene;
    Screen* mScreenHandlerList=nullptr;
    Screen* mActiveScreenHandler=nullptr;
    std::map<ScreenId,std::unique_ptr<Screen>> screens;
    Frame activation_frame;
    std::vector<resources::FrontendInstanceChange> edits;
    std::optional<std::uint32_t> notification;
    std::shared_ptr<const FrontendVisualAssets> notification_visuals;
    resources::FrontendImageCatalog::Handle notification_images;
    Implementation(FrontendHandler& h,std::shared_ptr<FrontendSession> retained,FrontendInput& i)
        :owner(h),session(std::move(retained)),input(i)
    {
        Require(bool(session),"Frontend handler requires a retained session");
        session->Current();input.HasFocusLock(this);FrontendBaseHandlerSceneCreated();
    }
    void Thread() const{Require(thread==std::this_thread::get_id(),"Frontend handler requires its creating thread");}
    void Ready() const{Thread();Require(bool(session),"Frontend handler has been released");}
    void Mutable() const
    {
        Ready();Require(!busy,"Recursive frontend handler mutation is unsupported");
        Require(nlGetCurrentAsyncRead()==nullptr,"Frontend handler mutation inside an NL callback is unsupported");
    }
    void Expected(const Frame& expected) const
    {Ready();Require(expected&&session->Current()==expected,"Frontend handler requires the visible current session snapshot");}
    Screen& Find(ScreenId id) const
    {const auto found=screens.find(id);Require(found!=screens.end(),"Native screen adapter is absent or belongs to another handler");return *found->second;}
    void Update(float delta){FrontendBaseHandlerUpdate(*this,delta);}
    void EnableInputIfSceneHasFocus(Implementation* target){input.Focus(target);}
    void ClearNotification(){notification.reset();notification_visuals.reset();notification_images.reset();}
    void Release()
    {
        Thread();if(!session)return;Mutable();
        if(exclusive){Require(input.HasFocusLock(this),"Release frontend exclusive focus in stack order");input.PopFocus(this);exclusive=false;}
        DispatchGuard guard(busy);
        mActiveScreenHandler=nullptr;mScreenHandlerList=nullptr;screens.clear();
        activation_frame.reset();edits.clear();ClearNotification();session.reset();
    }
    ~Implementation(){try{Release();}catch(...){std::terminate();}}
};
FrontendHandler::FrontendHandler(std::shared_ptr<FrontendSession> session,FrontendInput& input)
    :impl_(std::make_unique<Implementation>(*this,std::move(session),input)){}
FrontendHandler::~FrontendHandler()=default;
FrontendHandler::Frame FrontendHandler::Current() const{impl_->Ready();return impl_->session->Current();}
void FrontendHandler::Update(const Frame& expected,float delta)
{
    auto& s=*impl_;s.Mutable();s.Expected(expected);
    Require(std::isfinite(delta)&&delta>=0&&delta<=60,"Frontend handler time exceeds its bounded profile");
    Require(expected->request.animate,"Frontend base handler update requires an animated native session");
    if(s.notification&&(expected->visuals!=s.notification_visuals||expected->images!=s.notification_images))s.ClearNotification();
    DispatchGuard guard(s.busy);
    FrontendFocusedHandlerUpdate(&s,&s,delta);
}
bool FrontendHandler::Button(const Frame& expected,FrontendAction action,FrontendButtonQuery query,int pad,int* found)
{
    auto& s=*impl_;s.Expected(expected);s.input.Focus(&s);
    return s.input.Button(action,query,pad,found);
}
FrontendHandler::ScreenId FrontendHandler::AddScreen(Activation activation)
{
    auto& s=*impl_;s.Mutable();Require(bool(activation)&&s.screens.size()<32,"Native screen adapter is empty or exceeds its 32-entry limit");
    const auto id=next_screen.fetch_add(1);Require(id&&id!=std::numeric_limits<ScreenId>::max(),"Native screen identity range exhausted");
    auto screen=std::make_unique<Implementation::Screen>(Implementation::Screen{nullptr,nullptr,nullptr,id,std::move(activation),s});
    auto* pointer=screen.get();s.screens.emplace(id,std::move(screen));FrontendBaseHandlerAdd(s,pointer);return id;
}
void FrontendHandler::Activate(const Frame& expected,ScreenId id)
{
    auto& s=*impl_;s.Mutable();s.Expected(expected);auto& screen=s.Find(id);
    auto* previous=s.mActiveScreenHandler;s.mActiveScreenHandler=&screen;s.activation_frame=expected;s.edits.clear();
    DispatchGuard guard(s.busy);
    try
    {
        FrontendBaseHandlerActivate(s);
        s.Expected(expected);Require(s.edits.size()<=128,"Native activation exceeds its 128-edit budget");
        if(!s.edits.empty())s.session->Apply(expected,s.edits);
        s.edits.clear();s.activation_frame.reset();
    }
    catch(...){s.mActiveScreenHandler=previous;s.edits.clear();s.activation_frame.reset();throw;}
}
void FrontendHandler::OriginalRemove(ScreenId id)
{auto& s=*impl_;s.Mutable();FrontendBaseHandlerRemove(&s.Find(id));}
void FrontendHandler::Detach(ScreenId id)
{
    auto& s=*impl_;s.Mutable();auto& screen=s.Find(id);
    DispatchGuard guard(s.busy);
    if(s.mActiveScreenHandler==&screen)s.mActiveScreenHandler=nullptr;
    nlDLRingRemove(&s.mScreenHandlerList,&screen);s.screens.erase(id);
}
std::vector<FrontendHandler::ScreenId> FrontendHandler::Screens() const
{
    const auto& s=*impl_;s.Ready();std::vector<ScreenId> result;
    auto* node=nlDLRingGetStart(s.mScreenHandlerList);
    while(node){result.push_back(node->id);if(nlDLRingIsEnd(s.mScreenHandlerList,node))break;node=node->m_next;}
    return result;
}
FrontendHandler::ScreenId FrontendHandler::ActiveScreen() const
{impl_->Ready();return impl_->mActiveScreenHandler?impl_->mActiveScreenHandler->id:0;}
void FrontendHandler::SetExclusiveInput(bool value)
{
    auto& s=*impl_;s.Mutable();if(value==s.exclusive)return;
    if(value)s.input.PushFocus(&s);
    else{Require(s.input.HasFocusLock(&s),"Release frontend exclusive focus in stack order");s.input.PopFocus(&s);}
    s.exclusive=value;
}
void FrontendHandler::WatchLoadingNotification(const Frame& expected,std::uint32_t id)
{
    auto& s=*impl_;s.Mutable();s.Expected(expected);
    Require(expected->request.animate,"Loading notification requires an animated session");
    const auto& graph=expected->graph;
    const auto instance=std::find_if(graph.instances.begin(),graph.instances.end(),[&](const auto& p){return p.offset==id;});
    Require(instance!=graph.instances.end()&&instance->type==4&&instance->visible&&instance->library,"Loading notification requires a visible authored component");
    const auto library=std::find_if(graph.library.begin(),graph.library.end(),[&](const auto& p){return p.offset==*instance->library;});
    Require(library!=graph.library.end()&&library->type==3&&library->active_slide,"Loading notification requires an active authored component slide");
    s.notification=id;s.notification_visuals=expected->visuals;s.notification_images=expected->images;
}
bool FrontendHandler::LoadingNotificationActive() const{impl_->Ready();return impl_->notification.has_value();}
void FrontendHandler::Release(){impl_->Release();}
}
