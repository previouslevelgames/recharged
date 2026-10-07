#include "platform/desktop_wpad.h"
#include "platform/desktop_dpd.h"
#include "platform/wpad_sdl.h"
#include "platform/interrupts.h"
#include <revolution/kpad.h>
#include "NL/plat/DPDData.h"
#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
unsigned checks{}, queries{}, controls{}, cancelled{}, immediate{}, rejected{}, unsupported{};
SDL_Window* window{};
std::thread::id owner;
DesktopDpdProjection projection{1,20,16,600,448};
bool projected = true;
KPADStatus last{};
DPDData data;

void Check(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void Reject(F fn,const char* message) {
    bool rejected=false;
    try {fn();}catch(const std::exception&){rejected=true;}
    Check(rejected,message);
}
bool Project(void*,SDL_Window* actual,DesktopDpdProjection* output) {
    Check(owner==std::this_thread::get_id()&&actual==window,
          "Projection queried foreign or retired window/owner");
    ++queries;
    *output=projection;
    // Generated display-hardware fixture, not a real successful GPU Present.
    return projected;
}
void Event(Uint32 type) {
    SDL_Event event{};event.type=type;event.window.windowID=SDL_GetWindowID(window);
    Check(SDL_PushEvent(&event),"Cannot publish actual SDL window event");
}
void Mouse(float x,float y,Uint32 type=SDL_EVENT_MOUSE_MOTION,Uint8 button=SDL_BUTTON_LEFT) {
    SDL_Event event{};event.type=type;
    if(type==SDL_EVENT_MOUSE_MOTION){event.motion.windowID=SDL_GetWindowID(window);event.motion.x=x;event.motion.y=y;event.motion.which=1;}
    else{event.button.windowID=SDL_GetWindowID(window);event.button.x=x;event.button.y=y;event.button.which=1;event.button.button=button;event.button.down=type==SDL_EVENT_MOUSE_BUTTON_DOWN;}
    Check(SDL_PushEvent(&event),"Cannot publish actual SDL mouse event");
}
void Service() {
    ServiceDesktopWpad();
    ServiceWpadSDL();
    KPADStatus sample{};
    if(KPADRead(0,&sample,1)>0){last=sample;data.Update(&last);}
}
template<class F> void Until(F fn,const char* message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!fn()){
        Service();
        if(std::chrono::steady_clock::now()>deadline){
            std::fprintf(stderr,"last DPDvalid=%d pos=%f/%f raw=%d/%d angle=%u\n",int(last.dpd_valid_fg),double(last.pos.x),double(last.pos.y),int(last.wpad_err),int(last.data_format),unsigned(data.mAngle));
            throw std::runtime_error(message);
        }
        SDL_Delay(2);
    }
    ++checks;
}
void Control(s32,WPADResult result){
    Check(owner==std::this_thread::get_id()&&!NativeInterruptsEnabled(),
          "Camera control completion crossed source owner/interrupt exclusion");
    Check(result==WPAD_ERR_OK,"Camera register request did not complete");
    ++controls;
}
void Immediate(s32,WPADResult result) {
    Check(owner==std::this_thread::get_id()&&!NativeInterruptsEnabled(),
          "Repeated pending camera request changed its caller/mask");
    Check(result==WPAD_ERR_OK,"Repeated source command did not retain original immediate acceptance");
    ++immediate;
}
void Rejected(s32,WPADResult result) {
    Check(owner==std::this_thread::get_id()&&!NativeInterruptsEnabled(),
          "Rejected camera request changed its masked caller");
    Check(result==WPAD_ERR_COMMUNICATION_ERROR,
          "Occupied actual native register slot did not report communication failure");
    ++rejected;
}
void Unsupported(s32,WPADResult result) {
    Check(owner==std::this_thread::get_id()&&NativeInterruptsEnabled(),
          "Unsupported command callback was delayed or changed its caller mask");
    Check(result==WPAD_ERR_INVALID,"Unsupported camera mode invented success");
    ++unsupported;
}

void Cancelled(s32,WPADResult result) {
    Check(owner==std::this_thread::get_id()&&!NativeInterruptsEnabled(),
          "Disconnected camera completion crossed source owner/IRQ boundary");
    Check(result==WPAD_ERR_NO_CONTROLLER,"Disconnected pending camera command falsely succeeded");
    ++cancelled;
}

// Independent raw word oracle: integer pixel centres and pair endpoints,
// chosen ahead of the adapter; these are not expected values from its helper.
void RawOracles() {
    const auto bottom=MakeDesktopDpdObservation(.5f,.5f,0,true);
    Check(bottom[0].x==378&&bottom[1].x==645,"Bottom centre camera pair width/half-pixel origin changed");
    Check(bottom[0].y==486&&bottom[1].y==486,"Bottom source sensor-height sign changed");
    const auto top=MakeDesktopDpdObservation(.5f,.5f,1,true);
    Check(top[0].y==281&&top[1].y==281,"Top source sensor-height sign changed");
    Check(top[0].size==12&&top[1].size==12&&!top[2].size&&!top[3].size,
          "Virtual raw camera fabricated more observed objects");
    const auto lost=MakeDesktopDpdObservation(0,0,1,false);
    for(const auto& point:lost)Check(!point.size,"Lost raw observation has a visible object");
    Reject([]{(void)MakeDesktopDpdObservation(0,0,2,true);},"Unknown camera calibration accepted");
    Reject([]{(void)MakeDesktopDpdObservation(-.01f,0,0,true);},"Out-of-content mouse was clamped into scene");
}

void CheckPosition(float nx,float ny) {
    Mouse(projection.left+nx*projection.width,projection.top+ny*projection.height);
    const float wanted_x=2*nx-1,wanted_y=2*ny-1;
    Until([&]{return last.dpd_valid_fg==2&&std::fabs(last.pos.x-wanted_x)<.006f&&std::fabs(last.pos.y-wanted_y)<.006f;},
          "Actual original KPAD did not resolve observed mouse camera pair");
    nlVector2 position{};unsigned short angle{};
    Check(data.GetPosition(&position,&angle)==2,"Actual DPDData GetPosition changed validity category");
    Check(std::fabs(position.x+wanted_x)<.006f&&std::fabs(position.y+wanted_y)<.006f,
          "Actual DPDData inversion changed");
    Check(angle==0,"Upright observed pair changed original source roll angle");
}

void SourceAndMouse() {
    ConfigureWpadSDL({0,3});
    DesktopWpadSettings bad{true,false,true};
    Reject([&]{InitializeDesktopWpad(window,bad);},"Mouse activated without a presented content provider");
    DesktopWpadSettings settings{true,false,true,Project,nullptr};
    InitializeDesktopWpad(window,settings);
    KPADInit();
    // Literal original pad initialization parameter requests.
    KPADSetPosParam(0,.02f,.95f);KPADSetHoriParam(0,0,1);KPADSetDistParam(0,0,1);KPADSetAccParam(0,0,1);
    (void)KPADRead(0,&last,1);
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    Event(SDL_EVENT_WINDOW_MOUSE_ENTER);
    Until([]{return WpadSDLConnectedChannels()==1&&last.wpad_err==WPAD_ERR_OK;},
          "Virtual camera did not reach real SDL/WPAD/source KPAD");
    Check(last.dpd_valid_fg==0,"Host manufactured initial mouse coordinates");
    const auto masked=OSDisableInterrupts();
    Check(!WPADIsDpdEnabled(0),"Unrequested virtual camera was enabled");
    Check(WPADControlDpd(0,WPAD_DPD_STANDARD,nullptr)==WPAD_ERR_OK,
          "Original NULL callback camera request was not accepted");
    Check(!WPADIsDpdEnabled(0),"Camera became enabled before its actual owner commit");
    Check(WPADControlDpd(0,WPAD_DPD_STANDARD,Immediate)==WPAD_ERR_OK&&immediate==1,
          "Repeated pending source command did not invoke original immediate callback");
    Check(WPADControlDpd(0,WPAD_DPD_BASIC,Rejected)==WPAD_ERR_COMMUNICATION_ERROR&&rejected==1,
          "NULL callback work disappeared or rejected callback was omitted");
    Check(WPADControlDpd(0,WPAD_DPD_DISABLE,nullptr)==WPAD_ERR_OK,
          "Original disabled-with-enable-pending quirk was rewritten");
    ServiceWpadSDL();
    Check(!WPADIsDpdEnabled(0),"Masked hardware servicing committed a NULL callback request");
    OSRestoreInterrupts(masked);
    Until([]{return WPADIsDpdEnabled(0);},"Actual NULL callback hardware work was never committed");
    Check(controls==0,"NULL callback request manufactured a game completion callback");
    Check(WPADControlDpd(0,WPAD_DPD_DISABLE,Control)==WPAD_ERR_OK,
          "Committed source camera could not request stop");
    Check(WPADIsDpdEnabled(0),"Stop request changed committed state before owner delivery");
    Until([]{return controls==1&&!WPADIsDpdEnabled(0);},"True owner did not commit the camera stop");
    const auto enable_mask=OSDisableInterrupts();
    Check(WPADControlDpd(0,WPAD_DPD_STANDARD,Control)==WPAD_ERR_OK,"Native source camera rejected real register mode");
    ServiceWpadSDL();Check(controls==1&&!WPADIsDpdEnabled(0),"Masked camera request delivered source completion");
    OSRestoreInterrupts(enable_mask);
    Until([]{return controls==2;},"Camera completion did not reach actual owner");
    Check(WPADIsDpdEnabled(0),"Actual camera register mode is unavailable");
    Check(WPADControlDpd(0,WPAD_DPD_EXTENDED,Unsupported)==WPAD_ERR_INVALID&&unsupported==1,
          "Unsupported extended camera mode became false-ready");
    CheckPosition(.5f,.5f);CheckPosition(.2f,.25f);CheckPosition(.8f,.75f);
    // Source button and source DPD validity remain independent, including the
    // real source's prior-position behavior when the raw camera loses objects.
    Mouse(projection.left+.3f*projection.width,projection.top+.6f*projection.height,SDL_EVENT_MOUSE_BUTTON_DOWN);
    Until([]{return last.hold&WPAD_BUTTON_A;},"Actual SDL mouse button did not reach source KPAD Wii A");
    Mouse(projection.left+.3f*projection.width,projection.top+.6f*projection.height,SDL_EVENT_MOUSE_BUTTON_UP);
    Until([]{return !(last.hold&WPAD_BUTTON_A);},"Actual SDL mouse release did not reach source KPAD");
    CheckPosition(.3f,.6f);
    const auto retained=last.pos;
    Mouse(5,5);
    Until([]{return last.dpd_valid_fg==0;},"Black-bar raw observation remained visible");
    Check(last.pos.x==retained.x&&last.pos.y==retained.y,
          "Host repaired original KPAD retained-position invalidity behavior");
    projected=false;Mouse(320,240);Service();
    Check(last.dpd_valid_fg==0,"Unknown output projection fabricated source pointer readiness");
    projected=true;projection={2,80,60,400,300};CheckPosition(.25f,.8f);
    std::thread worker([]{Mouse(380,135);});worker.join();
    Until([]{return last.dpd_valid_fg==2&&last.pos.x>.45f;},"Worker-latched raw SDL observation failed owner delivery");
    Event(SDL_EVENT_WINDOW_FOCUS_LOST);
    Until([]{return last.dpd_valid_fg==0&&!last.hold;},"Focus loss retained mouse raw objects/buttons");
    const auto retire_mask=OSDisableInterrupts();
    Check(WPADControlDpd(0,WPAD_DPD_DISABLE,Cancelled)==WPAD_ERR_OK,
          "Camera stop request was not accepted before device retirement");
    Check(WPADControlDpd(0,WPAD_DPD_STANDARD,Rejected)==WPAD_ERR_BUSY&&rejected==2,
          "Pending raw camera command was overwritten");
    ShutdownDesktopWpad();
    ServiceWpadSDL(); Check(cancelled==0,"Masked disconnect completed a source camera request");
    OSRestoreInterrupts(retire_mask);
    ServiceWpadSDL();
    Check(cancelled==1&&WpadSDLConnectedChannels()==0,
          "Pending command did not fail before real disconnected channel retirement");
    WPADShutdown();
    Check(WPADGetStatus()==WPAD_LIB_STATUS_0,"Camera shutdown left source callbacks active");
}

void ProducerLifetime() {
    // Test the actual device registration owner with an independently attached
    // real SDL virtual joystick; no game manager or callback substitute.
    SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);desc.name="Raw DPD lifetime fixture";
    const auto id=SDL_AttachVirtualJoystick(&desc);Check(id!=0,"Cannot attach actual SDL raw fixture");
    auto* joystick=SDL_OpenJoystick(id);Check(joystick,"Cannot own actual raw producer backing");
    const auto first=AttachNativeWpadDpdSource(id);
    const auto observed=MakeDesktopDpdObservation(.5f,.5f,1,true);
    SubmitNativeWpadDpdObservation(first,observed);
    Reject([&]{(void)AttachNativeWpadDpdSource(id);},"Duplicate live raw producer accepted");
    bool rejected=false;
    std::thread worker([&]{try{SubmitNativeWpadDpdObservation(first,observed);}catch(const std::logic_error&){rejected=true;}});worker.join();
    Check(rejected,"Foreign thread published camera registers");
    DetachNativeWpadDpdSource(first);
    const auto second=AttachNativeWpadDpdSource(id);Check(second.generation!=first.generation,"Reattachment reused retired producer incarnation");
    Reject([&]{SubmitNativeWpadDpdObservation(first,observed);},"Retired raw producer published into a new incarnation");
    Reject([&]{DetachNativeWpadDpdSource(first);},"Retired source released a current producer");
    Check(SDL_DetachVirtualJoystick(id),"Cannot detach actual producer device");
    Reject([&]{SubmitNativeWpadDpdObservation(second,observed);},"Disconnected raw backing remained usable");
    DetachNativeWpadDpdSource(second);SDL_CloseJoystick(joystick);
}
}
int main(){
    try{
        owner=std::this_thread::get_id();RawOracles();
        Check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD),"Cannot initialize actual SDL devices");
        window=SDL_CreateWindow("Original raw camera projection qualifier",640,480,SDL_WINDOW_HIDDEN);
        Check(window,"Cannot create owned actual SDL window");
        SourceAndMouse();ProducerLifetime();
        SDL_DestroyWindow(window);window=nullptr;SDL_Quit();
        std::printf("Original KPAD/DPDData and native SDL raw camera: %u checks, %u projection queries; generated viewport only, physical/presented IR and full FE lifecycle unqualified.\n",checks,queries);
        return 0;
    }catch(const std::exception& e){
        std::fprintf(stderr,"Raw DPD qualifier: %s\n",e.what());
        try{WPADShutdown();ShutdownDesktopWpad();}catch(...){}
        if(window)SDL_DestroyWindow(window);SDL_Quit();return 1;
    }
}
