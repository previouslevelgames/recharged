#include "runtime/frontend_visual_navigation.h"
#include "NL/nlFileGC.h"
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool value,const char* message){if(!value)throw std::logic_error(message);}
struct Guard{bool& value;explicit Guard(bool& v):value(v){value=true;}~Guard(){value=false;}};
}
struct FrontendVisualNavigation::Implementation
{
    std::shared_ptr<FrontendVisualOptions> visual;
    std::shared_ptr<FrontendNavigation> navigation;
    FrontendInput& input;
    std::unique_ptr<FrontendNavigationDone> done;
    FrontendSession::Handle commands_frame;
    const std::thread::id thread=std::this_thread::get_id();
    bool back_bound=false,done_bound=false,busy=false,failed=false;
    Implementation(std::shared_ptr<FrontendVisualOptions> v,std::shared_ptr<FrontendNavigation> n,FrontendInput& i)
        :visual(std::move(v)),navigation(std::move(n)),input(i){}
    void Check()const
    {
        Require(thread==std::this_thread::get_id()&&!busy&&!failed&&!nlGetCurrentAsyncRead()&&visual&&navigation,
            "Visual navigation requires its idle live owner thread");
        const auto menu=visual->Current(),nav=navigation->Current();
        Require(menu&&nav&&menu->visuals==nav->visuals&&menu->images==nav->images,
            "Visual and NAV must share their exact retained resource owners");
    }
};
FrontendVisualNavigation::FrontendVisualNavigation(std::shared_ptr<FrontendVisualOptions> visual,
    std::shared_ptr<FrontendNavigation> navigation,FrontendInput& input,
    std::shared_ptr<FrontendAudio> audio,unsigned& seed,std::shared_ptr<NativePreferences> preferences)
    :impl_(std::make_unique<Implementation>(std::move(visual),std::move(navigation),input))
{
    auto& s=*impl_;s.Check();Require(bool(preferences),"Visual navigation requires an explicit native preferences service");
    // These owners outlive the retained callback. No fallback to a successful
    // original SaveLoad service is installed.
    s.done=std::make_unique<FrontendNavigationDone>(s.navigation,input,std::move(audio),seed,
        [owner=s.visual,preferences=std::move(preferences)](const auto& frame){owner->SaveNativePreferences(frame,preferences);});
    ApplyCommands();
}
FrontendVisualNavigation::~FrontendVisualNavigation(){try{Release();}catch(...){std::terminate();}}
void FrontendVisualNavigation::ApplyCommands()
{
    auto& s=*impl_;s.Check();const auto frame=s.visual->Current();if(frame==s.commands_frame)return;
    const auto commands=s.visual->Status().commands;Guard guard(s.busy);
    try
    {
        for(const auto& command:commands)
        {
            switch(command.kind)
            {
            case FrontendVisualOptionsCommandKind::HideNavigation:
                s.navigation->HideButtons(s.navigation->Current());break;
            case FrontendVisualOptionsCommandKind::ResetNavigation:
                s.navigation->SetButtons(s.navigation->Current(),0,true);break;
            case FrontendVisualOptionsCommandKind::BindBack:
                Require(command.argument==4,"Visual requested an unsupported NAV Back binding");s.back_bound=true;break;
            case FrontendVisualOptionsCommandKind::BindDone:
                Require(command.argument==0x20,"Visual requested an unsupported NAV Done binding");s.done_bound=true;break;
            case FrontendVisualOptionsCommandKind::ShowBackAndDone:
                Require(command.argument==0x24,"Visual requested an unsupported NAV button mask");
                s.navigation->SetButtons(s.navigation->Current(),command.argument,true);break;
            case FrontendVisualOptionsCommandKind::DoneText:
                Require(command.argument==1,"Visual requested an unsupported Done label");
                s.navigation->SetDoneButtonText(s.navigation->Current(),command.argument);break;
            case FrontendVisualOptionsCommandKind::DoneDown:
                s.navigation->SetDoneButtonSlide(s.navigation->Current(),FrontendNavigationDoneSlide::Down);break;
            case FrontendVisualOptionsCommandKind::PointerWaiting:
            case FrontendVisualOptionsCommandKind::PointerCursor:
                s.navigation->SetPointerSlide(s.navigation->Current(),command.argument,
                    command.kind==FrontendVisualOptionsCommandKind::PointerWaiting?FrontendNavigationPointer::Waiting:FrontendNavigationPointer::Cursor);break;
            case FrontendVisualOptionsCommandKind::PushOptions:
                Require(command.argument==13,"Visual requested an unsupported destination");
                // Source request stays observable in Visual.Status().commands.
                // Actual manager replacement belongs to the caller's queue.
                break;
            case FrontendVisualOptionsCommandKind::HoverRumble:
                // Original haptic request remains visible on Visual.Status().
                // No successful speaker/rumble service is claimed here.
                break;
            }
        }
        s.commands_frame=frame;
    }
    catch(...){s.failed=true;throw;}
}
void FrontendVisualNavigation::Acknowledge(const FrontendSession::Handle& nav)
{
    auto& s=*impl_;s.Check();Require(s.done_bound,"Visual Done listener is not source-bound");Guard guard(s.busy);
    try{s.done->Acknowledge(nav);}catch(...){s.failed=true;throw;}
}
FrontendNavigationDispatch FrontendVisualNavigation::Dispatch(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& nav,const std::function<FrontendNavigationDispatch()>& sample)
{
    auto& s=*impl_;s.Check();if(s.input.InputLocked())return{};Require(menu&&nav,"Visual navigation requires both presented snapshots");
    const auto status=s.visual->Status();Require(status.initialized&&status.state==1&&!status.native_save_admitted,
        "Visual navigation input requires its interactive source state");
    Require(s.back_bound&&s.done_bound,"Visual navigation source bindings are unavailable");
    FrontendNavigationDispatch result;Guard guard(s.busy);
    try
    {
        s.navigation->WithPresentedInput(nav,[&]{
            result=sample();
            if(result.back_pressed){s.visual->NotifyBackButton(menu);return;}
            s.visual->DeliverPointer(menu,result.pointer.event);
            s.done->Route(nav,menu,result.pointer.event);
        });
    }
    catch(...){s.failed=true;throw;}
    return result;
}
FrontendNavigationDispatch FrontendVisualNavigation::Route(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& nav,const FrontendPointerDesktopSample& sample)
{return Dispatch(menu,nav,[&]{return impl_->navigation->Route(sample);});}
FrontendNavigationDispatch FrontendVisualNavigation::Poll(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& nav,SDL_Window* window,bool capture)
{return Dispatch(menu,nav,[&]{return impl_->navigation->Poll(window,capture);});}
FrontendNavigationDispatch FrontendVisualNavigation::Deliver(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& nav,const FrontendPointerEvent& event)
{
    return Dispatch(menu,nav,[&]{FrontendNavigationDispatch result;result.pointer.event=event;
        result.pointer.listeners=1;result.back_pressed=impl_->navigation->DeliverPointer(nav,event);return result;});
}
FrontendNavigationDoneStatus FrontendVisualNavigation::DoneStatus()const{impl_->Check();return impl_->done->Status();}
FrontendPointerBounds FrontendVisualNavigation::DoneBounds()const{impl_->Check();return impl_->done->Bounds();}
bool FrontendVisualNavigation::Failed()const
{Require(impl_->thread==std::this_thread::get_id(),"Visual navigation status requires its owner thread");return impl_->failed;}
void FrontendVisualNavigation::Release()
{
    auto& s=*impl_;Require(s.thread==std::this_thread::get_id()&&!s.busy&&!nlGetCurrentAsyncRead(),"Visual navigation teardown requires its idle owner thread");
    if(!s.visual)return;Guard guard(s.busy);if(s.done)s.done->Release();s.done.reset();s.commands_frame.reset();s.visual.reset();s.navigation.reset();
}
}
