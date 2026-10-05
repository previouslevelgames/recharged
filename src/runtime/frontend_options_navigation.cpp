#include "runtime/frontend_options_navigation.h"
#include "NL/nlFileGC.h"
#include <stdexcept>

namespace mscharged
{
namespace
{
void Require(bool value,const char* message){if(!value)throw std::logic_error(message);}
struct Guard{bool& value;explicit Guard(bool& v):value(v){value=true;}~Guard(){value=false;}};
}
FrontendOptionsNavigation::FrontendOptionsNavigation(FrontendOptions& options,FrontendNavigation& navigation,
    std::function<void(unsigned)> select_music)
    :options_(options),navigation_(navigation),select_music_(std::move(select_music))
{
    Require(bool(select_music_),"Options navigation requires its actual music service");
    ApplyCommands();
}
void FrontendOptionsNavigation::Check()const
{
    Require(thread_==std::this_thread::get_id()&&!busy_&&!failed_&&!nlGetCurrentAsyncRead(),
        "Options navigation requires its idle live owner thread");
    const auto menu=options_.Current(),nav=navigation_.Current();
    Require(menu->visuals==nav->visuals&&menu->images==nav->images,
        "Options and navigation must share their exact resource owners");
}
void FrontendOptionsNavigation::ApplyCommands()
{
    Check();const auto frame=options_.Current();if(frame==commands_frame_)return;
    const auto commands=options_.Status().commands;Guard guard(busy_);
    try
    {
        for(const auto& command:commands)
        {
            switch(command.kind)
            {
            case FrontendOptionsCommandKind::HideNavigation:
                navigation_.HideButtons(navigation_.Current());break;
            case FrontendOptionsCommandKind::BindNavigationBack:
                Require(command.argument==4,"Options requested an unsupported navigation binding");back_bound_=true;break;
            case FrontendOptionsCommandKind::ShowNavigationBack:
                navigation_.SetButtons(navigation_.Current(),command.argument,true);break;
            case FrontendOptionsCommandKind::PointerWaiting:
            case FrontendOptionsCommandKind::PointerCursor:
                navigation_.SetPointerSlide(navigation_.Current(),command.argument,
                    command.kind==FrontendOptionsCommandKind::PointerWaiting
                        ?FrontendNavigationPointer::Waiting:FrontendNavigationPointer::Cursor);break;
            case FrontendOptionsCommandKind::SelectMusic:select_music_(command.argument);break;
            case FrontendOptionsCommandKind::PushScene:
            case FrontendOptionsCommandKind::TransitionOptionsToMainMenu:
            case FrontendOptionsCommandKind::PopScene:
                // The exact original request stays in Options.Status().transition.
                // Destination/script admission belongs to its actual services.
                break;
            }
        }
        commands_frame_=frame;
    }
    catch(...){failed_=true;throw;}
}
FrontendNavigationDispatch FrontendOptionsNavigation::Dispatch(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& nav,const std::function<FrontendNavigationDispatch()>& route)
{
    Check();Require(menu&&nav,"Options input requires both presented snapshots");
    const auto status=options_.Status();
    Require(status.initialized&&status.state==1&&!status.transition,"Options navigation input requires its interactive source stage");
    FrontendNavigationDispatch result;Guard guard(busy_);
    try
    {
        navigation_.WithPresentedInput(nav,[&]{
            result=route();
            if(result.back_pressed)
            {
                Require(back_bound_,"Options navigation Back callback is unavailable");
                options_.NotifyBackButton(menu);
            }
            else options_.DeliverPointer(menu,result.pointer.event);
        });
    }
    catch(...){failed_=true;throw;}
    return result;
}
FrontendNavigationDispatch FrontendOptionsNavigation::Route(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& nav,const FrontendPointerDesktopSample& sample)
{return Dispatch(menu,nav,[&]{return navigation_.Route(sample);});}
FrontendNavigationDispatch FrontendOptionsNavigation::Poll(const FrontendSession::Handle& menu,
    const FrontendSession::Handle& nav,SDL_Window* window,bool capture)
{return Dispatch(menu,nav,[&]{return navigation_.Poll(window,capture);});}
bool FrontendOptionsNavigation::BackBound()const{Check();return back_bound_;}
bool FrontendOptionsNavigation::Failed()const
{Require(thread_==std::this_thread::get_id(),"Options navigation status requires its owner thread");return failed_;}
}
