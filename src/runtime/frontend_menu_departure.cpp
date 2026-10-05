#include "runtime/frontend_menu_departure.h"
#include "Game/FE/FrontendMainDepartureSteps.h"
#include "NL/nlFileGC.h"
#include <stdexcept>

namespace mscharged
{
namespace
{
void Require(bool value,const char* message){if(!value)throw std::logic_error(message);}
}
void BeginMainOptions(FrontendMainMenu& main,FrontendSceneStack& stack,FrontendSceneStack::Token token,
    FrontendNavigation& navigation,FrontendAudio& audio,unsigned& seed,FrontendMenuTransition& flow,
    FrontendMenuTransitionServices services)
{
    Require(!nlGetCurrentAsyncRead(),"Main departure cannot run inside an NL callback");
    const auto selected=main.Status().selection;
    const auto entry=stack.Entry(token);
    Require(selected&&selected->item==6&&selected->service==FrontendMainSelectionService::ApplyItem,
        "Main departure requires the actual source Options selection");
    Require(entry.scene==1&&entry.handler_scope==FrontendStackHandlerScope::SelectedVisual&&!entry.queued_pop
        &&entry.published==selected->source,"Main departure requires its exact published selected visual");
    const auto current=main.Current(),nav=navigation.Current();
    Require(current->visuals==selected->source->visuals&&current->images==selected->source->images
        &&current->visuals==nav->visuals&&current->images==nav->images,
        "Main departure cannot cross retained resource owners");
    Require(audio.Loaded(),"Main departure requires live resident audio");
    const auto state=flow.Status().state;
    Require(state==FrontendMenuTransitionState::Idle||state==FrontendMenuTransitionState::Cancelled
        ||state==FrontendMenuTransitionState::SceneQueued,"Main departure requires an idle transition owner");
    std::string_view next;
    FrontendMainApplyOptions(
        [&]{stack.QueuePop(token);},
        [&]{navigation.HideButtons(navigation.Current());},
        [&](unsigned long cue,const void* name,void* context,bool restartable){
            Require(cue==0xB19DBC20&&!name&&!context&&restartable,"Unqualified Main Options departure cue");
            audio.Play(std::uint32_t(cue),seed);
        },
        [&](const char* function){next=function;},
        [&](const char* function){
            Require(next=="TransitionMainMenuToOptions"&&std::string_view(function)=="TransitionFromMainMenu",
                "Unqualified Main Options departure script");
            flow.Begin(FrontendMenuTransitionKind::MainDeparture,token,selected->source,std::move(services));
        });
}
}
