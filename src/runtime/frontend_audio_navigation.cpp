#include "runtime/frontend_audio_navigation.h"
#include "NL/nlFileGC.h"
#include <thread>
#include <stdexcept>

namespace mscharged
{
namespace
{
void Require(bool value,const char* text){if(!value)throw std::logic_error(text);}
struct Guard{bool& busy;explicit Guard(bool& b):busy(b){busy=true;}~Guard(){busy=false;}};
}
struct FrontendAudioNavigation::Implementation
{
    std::shared_ptr<FrontendAudioOptions> menu;
    std::shared_ptr<FrontendNavigation> navigation;
    std::shared_ptr<NativePreferences> preferences;
    FrontendInput& input;
    std::unique_ptr<FrontendNavigationDone> done;
    FrontendSession::Handle commands_frame;
    const std::thread::id thread=std::this_thread::get_id();
    bool back_bound=false,done_bound=false,busy=false,failed=false;
    Implementation(std::shared_ptr<FrontendAudioOptions> m,std::shared_ptr<FrontendNavigation> n,
        FrontendInput& input,std::shared_ptr<FrontendAudio> audio,unsigned& seed,std::shared_ptr<NativePreferences> p)
        :menu(std::move(m)),navigation(std::move(n)),preferences(std::move(p)),input(input)
    {
        Require(menu&&navigation&&preferences,"Audio navigation requires actual retained menu, NAV and native preferences");
        Check();
        done=std::make_unique<FrontendNavigationDone>(navigation,input,std::move(audio),seed,
            [m=menu,p=preferences](const auto& shown){m->SaveNativePreferences(shown,p);});
    }
    void Check()const
    {
        Require(thread==std::this_thread::get_id()&&menu&&!busy&&!failed&&!nlGetCurrentAsyncRead(),"Audio navigation requires its idle live owner thread");
        const auto m=menu->Current(),n=navigation->Current();
        Require(m->visuals==n->visuals&&m->images==n->images,"Audio and NAV must share their exact resource owners");
    }
    FrontendNavigationDispatch Dispatch(const FrontendSession::Handle& m,const FrontendSession::Handle& n,
        const std::function<FrontendNavigationDispatch()>& route)
    {
        Check();if(input.InputLocked())return {};Require(m&&n&&back_bound&&done_bound,"Audio input requires retained presented frames and original NAV bindings");
        const auto state=menu->Status();Require(state.initialized&&state.state==1&&!state.native_save_admitted,"Audio navigation requires its interactive source stage");
        FrontendNavigationDispatch result;Guard guard(busy);
        try
        {
            navigation->WithPresentedInput(n,[&]{
                result=route();
                if(result.back_pressed)menu->NotifyBackButton(m);
                else
                {
                    menu->DeliverPointer(m,result.pointer.event);
                    done->Route(n,m,result.pointer.event);
                }
            });
        }
        catch(...){failed=true;throw;}
        return result;
    }
};
FrontendAudioNavigation::FrontendAudioNavigation(std::shared_ptr<FrontendAudioOptions> menu,
    std::shared_ptr<FrontendNavigation> navigation,FrontendInput& input,std::shared_ptr<FrontendAudio> audio,
    unsigned& seed,std::shared_ptr<NativePreferences> preferences)
    :impl_(std::make_unique<Implementation>(std::move(menu),std::move(navigation),input,std::move(audio),seed,std::move(preferences)))
{ApplyCommands();}
FrontendAudioNavigation::~FrontendAudioNavigation(){try{Release();}catch(...){std::terminate();}}
void FrontendAudioNavigation::ApplyCommands()
{
    auto& s=*impl_;s.Check();const auto frame=s.menu->Current();if(frame==s.commands_frame)return;
    const auto commands=s.menu->Status().commands;Guard guard(s.busy);
    try
    {
        for(const auto& command:commands)
        {
            switch(command.kind)
            {
            case FrontendAudioOptionsCommandKind::HideNavigation:s.navigation->HideButtons(s.navigation->Current());break;
            case FrontendAudioOptionsCommandKind::ResetNavigation:s.navigation->SetButtons(s.navigation->Current(),0,true);break;
            case FrontendAudioOptionsCommandKind::BindBack:
                Require(command.argument==4,"Audio requested unsupported NAV Back binding");s.back_bound=true;break;
            case FrontendAudioOptionsCommandKind::BindDone:
                Require(command.argument==0x20,"Audio requested unsupported NAV Done binding");s.done_bound=true;break;
            case FrontendAudioOptionsCommandKind::ShowBackAndDone:Require(command.argument==0x24,"Audio NAV button mask differs");s.navigation->SetButtons(s.navigation->Current(),command.argument,true);break;
            case FrontendAudioOptionsCommandKind::DoneText:Require(command.argument==1,"Audio Done label differs");s.navigation->SetDoneButtonText(s.navigation->Current(),command.argument);break;
            case FrontendAudioOptionsCommandKind::DoneDown:s.navigation->SetDoneButtonSlide(s.navigation->Current(),FrontendNavigationDoneSlide::Down);break;
            case FrontendAudioOptionsCommandKind::PointerWaiting:
            case FrontendAudioOptionsCommandKind::PointerCursor:
                s.navigation->SetPointerSlide(s.navigation->Current(),command.argument,command.kind==FrontendAudioOptionsCommandKind::PointerWaiting?FrontendNavigationPointer::Waiting:FrontendNavigationPointer::Cursor);break;
            case FrontendAudioOptionsCommandKind::PushOptions:
            case FrontendAudioOptionsCommandKind::HoverRumble:
                // Kept in the selected menu status for actual scene/haptic
                // admission by the caller; never mark a manager request done.
                break;
            }
        }
        s.commands_frame=frame;
    }
    catch(...){s.failed=true;throw;}
}
void FrontendAudioNavigation::Acknowledge(const FrontendSession::Handle& frame)
{
    auto& s=*impl_;s.Check();Require(s.done_bound,"Audio Done binding has not been requested");Guard guard(s.busy);
    try{s.done->Acknowledge(frame);}catch(...){s.failed=true;throw;}
}
FrontendNavigationDispatch FrontendAudioNavigation::Deliver(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& navigation,const FrontendPointerEvent& event)
{
    auto& s=*impl_;return s.Dispatch(menu,navigation,[&]{return FrontendNavigationDispatch{{event,false,1},s.navigation->DeliverPointer(navigation,event)};});
}
FrontendNavigationDispatch FrontendAudioNavigation::Route(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& navigation,const FrontendPointerDesktopSample& sample)
{auto& s=*impl_;return s.Dispatch(menu,navigation,[&]{return s.navigation->Route(sample);});}
FrontendNavigationDispatch FrontendAudioNavigation::Poll(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& navigation,SDL_Window* window,bool capture)
{auto& s=*impl_;return s.Dispatch(menu,navigation,[&]{return s.navigation->Poll(window,capture);});}
FrontendNavigationDoneStatus FrontendAudioNavigation::DoneStatus()const{impl_->Check();return impl_->done->Status();}
FrontendPointerBounds FrontendAudioNavigation::DoneBounds()const{impl_->Check();return impl_->done->Bounds();}
bool FrontendAudioNavigation::Failed()const
{Require(impl_->thread==std::this_thread::get_id(),"Audio navigation status requires owner thread");return impl_->failed;}
void FrontendAudioNavigation::Release()
{
    auto& s=*impl_;Require(s.thread==std::this_thread::get_id()&&!s.busy&&!nlGetCurrentAsyncRead(),"Audio navigation release requires its idle owner thread");if(!s.menu)return;
    Guard guard(s.busy);s.done->Release();s.done.reset();s.commands_frame.reset();s.preferences.reset();s.menu.reset();s.navigation.reset();
}
}
