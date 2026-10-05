#include "runtime/frontend_navigation_done.h"
#include "Game/FE/FrontendDoneButtonSteps.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <thread>
#include <stdexcept>

namespace mscharged
{
namespace
{
void Require(bool value,const char* text){if(!value)throw std::logic_error(text);}
struct Guard{bool& busy;explicit Guard(bool& b):busy(b){busy=true;}~Guard(){busy=false;}};
}
struct FrontendNavigationDone::Implementation
{
    std::shared_ptr<FrontendNavigation> navigation;
    FrontendInput& input;
    std::shared_ptr<FrontendAudio> audio;
    unsigned& seed;
    SaveCallback save;
    FrontendNavigationDoneStatus status;
    std::unique_ptr<FrontendPointerRegion> region;
    FrontendSession::Handle menu_input;
    std::vector<FrontendAudioHandle> sounds;
    const std::thread::id thread=std::this_thread::get_id();
    bool busy=false;
    Implementation(std::shared_ptr<FrontendNavigation> n,FrontendInput& i,std::shared_ptr<FrontendAudio> a,unsigned& r,SaveCallback cb)
        :navigation(std::move(n)),input(i),audio(std::move(a)),seed(r),save(std::move(cb))
    {
        Require(navigation&&audio&&audio->Loaded()&&save,"Done pointer requires actual retained NAV, audio and save callback");
        (void)navigation->Current();(void)input.InputLocked();
    }
    void Ready()const{Require(thread==std::this_thread::get_id()&&navigation&&!status.failed,"Done pointer requires its live nonfailed owner thread");}
    void Mutable()const{Ready();Require(!busy&&!nlGetCurrentAsyncRead(),"Done pointer is busy or in an NL callback");}
    void Play(unsigned long cue,const char* name,void* context,bool restart)
    {
        Require(cue<=UINT32_MAX&&!name&&!context&&restart,"Done hover audio contract differs");
        const auto live=audio->Handles();std::erase_if(sounds,[&](auto h){return std::find(live.begin(),live.end(),h)==live.end();});
        sounds.reserve(sounds.size()+1);const auto h=audio->Play(std::uint32_t(cue),seed);
        Require(bool(h),"Done authored hover cue is unavailable");sounds.push_back(*h);
    }
    struct Step
    {
        Implementation& owner;
        struct Button
        {
            Implementation& owner;
            void SetPointerState(int state,int index){Require(index>=0&&index<4,"Done pointer index exceeds four");owner.status.pointer_states[index]=state;}
            bool HasOtherPointerState(int state,int index)const
            {for(unsigned i=0;i<4;++i)if(int(i)!=index&&owner.status.pointer_states[i]==state)return true;return false;}
        } mSaveButtonComponent{owner};
        struct Component
        {
            Implementation& owner;
            void SetActiveSlide(const char* name,bool reset,bool preserve)
            {
                Require(reset&&!preserve,"Done feedback reset contract differs");
                const auto n=std::string_view(name);
                Require(n=="off"||n=="over","Done hover requested unsupported feedback");
                owner.navigation->SetDoneButtonSlide(owner.navigation->Current(),n=="off"?FrontendNavigationDoneSlide::Off:FrontendNavigationDoneSlide::Over);
            }
        } component{owner};Component* mSaveButton=&component;
    };
    void Callback(FrontendPointerCallback kind,unsigned index,const FrontendSession::Handle& frame)
    {
        Require(busy&&region&&frame==region->Current()&&menu_input,"Done callback lost its retained route");
        Step step{*this};
        if(kind==FrontendPointerCallback::Enter)
        {
            FrontendDoneButtonEnter(step,int(index),[&](auto cue,const char* name,void* context,bool restart){Play(cue,name,context,restart);});
            // Original default mSpeakerEnabled=true requests hover rumble after
            // the menu callback. No Wii rumble-active query is invented here.
            ++status.hover_feedback_requests;
        }
        else if(kind==FrontendPointerCallback::Leave)FrontendDoneButtonLeave(step,int(index));
        else if(kind==FrontendPointerCallback::Press)
        {
            Require(!status.pressed,"Done save callback was already admitted");
            save(menu_input);status.pressed=true;
        }
    }
};
FrontendNavigationDone::FrontendNavigationDone(std::shared_ptr<FrontendNavigation> n,FrontendInput& input,
    std::shared_ptr<FrontendAudio> audio,unsigned& seed,SaveCallback save)
    :impl_(std::make_unique<Implementation>(std::move(n),input,std::move(audio),seed,std::move(save))){}
FrontendNavigationDone::~FrontendNavigationDone(){try{Release();}catch(...){std::terminate();}}
void FrontendNavigationDone::Acknowledge(const FrontendSession::Handle& frame)
{
    auto& s=*impl_;s.Mutable();const auto binding=s.navigation->DoneButton(frame);
    if(s.region)s.region->RebindFrame(frame);
    else s.region=std::make_unique<FrontendPointerRegion>(s.input,frame,binding.component,binding.bounds,
        [&s](auto kind,unsigned index,const auto& shown){s.Callback(kind,index,shown);});
    s.status.bound=true;
}
bool FrontendNavigationDone::Route(const FrontendSession::Handle& navigation,const FrontendSession::Handle& menu,const FrontendPointerEvent& event)
{
    auto& s=*impl_;s.Mutable();Require(menu&&s.region&&navigation==s.region->Current(),"Done input requires its actually acknowledged NAV frame");
    s.navigation->CheckDoneInput(navigation);
    const auto binding=s.navigation->DoneButton(navigation);
    Require(menu->visuals==navigation->visuals&&menu->images==navigation->images,"Done menu and NAV must share exact resource owners");
    // Native input waits for the actual visible NAV generation, even when the
    // source gate has just enabled the next unpublished candidate this frame.
    if(!binding.visible)return false;
    Require((s.navigation->Status().visible_buttons&0x20)&&!s.status.pressed,"Done input requires its visible unpressed button");
    Require(event.index<4,"Done pointer index exceeds four");
    Guard guard(s.busy);s.menu_input=menu;
    struct Reset{Implementation& s;~Reset(){s.menu_input.reset();}}reset{s};
    // Original HandlePointerEvent sets ControllerSpeaker context before the
    // listener algorithm. This is metadata, not desktop speaker emulation.
    s.status.speaker_context=event.index+1;
    try{s.region->Deliver(navigation,event);return s.status.pressed;}
    catch(...){s.status.failed=true;throw;}
}
FrontendPointerBounds FrontendNavigationDone::Bounds()const
{auto& s=*impl_;s.Ready();Require(bool(s.region),"Done bounds await actual publication");return s.region->Bounds();}
FrontendNavigationDoneStatus FrontendNavigationDone::Status()const
{auto& s=*impl_;Require(s.thread==std::this_thread::get_id()&&s.navigation,"Done status requires its live owner thread");return s.status;}
void FrontendNavigationDone::Release()
{
    auto& s=*impl_;Require(s.thread==std::this_thread::get_id()&&!s.busy&&!nlGetCurrentAsyncRead(),"Done teardown requires its idle owner thread");
    if(!s.navigation)return;Guard guard(s.busy);s.region.reset();s.menu_input.reset();s.save={};
    std::exception_ptr error;
    if(s.audio->Loaded()){const auto live=s.audio->Handles();for(auto h:s.sounds)if(std::find(live.begin(),live.end(),h)!=live.end())try{s.audio->Cancel(h);}catch(...){if(!error)error=std::current_exception();}}
    s.sounds.clear();s.audio.reset();s.navigation.reset();if(error)std::rethrow_exception(error);
}
}
