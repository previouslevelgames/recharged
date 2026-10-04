#include "runtime/frontend_input.h"
#include "runtime/frontend_input_sdl.h"
#include "Game/FE/feInput.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace mscharged;
using A = FrontendAction;
using Q = FrontendButtonQuery;
namespace
{
unsigned checks = 0;
void Check(bool good, const char* message) { ++checks; if (!good) throw std::runtime_error(message); }
template<class F> void Reject(F fn)
{ ++checks; try { fn(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid input operation accepted"); }
void Core()
{
    FrontendInput input;
    Reject([] { FrontendInput duplicate; });
    std::array<FrontendPadSample,4> samples{}; for (auto& s : samples) s.connected = true;
    input.Update(samples,0); Check(input.Connected(3),"Fourth pad missing");
    for (const auto [action,mask] : std::array<std::pair<A,unsigned>,7>{{{A::Left,1},{A::Right,2},{A::Up,8},{A::Down,4},{A::Accept,0x100},{A::Back,0x200},{A::Start,0x1000}}})
    {
        for (int pad=0;pad<4;++pad)
        {
            samples[pad].buttons=mask;input.Update(samples,0);
            int found=-2;Check(input.Button(action,Q::Pressed,-1,&found)&&found==pad,"Original action remap/first pad differs");
            Check(input.Button(action,Q::Held,pad)&&input.Button(action,Q::Repeat,pad),"First press was not immediate");
            input.SetRepeat(action,.3f,.1f,pad);
            input.Update(samples,.1f);Check(!input.Button(action,Q::Pressed,pad)&&!input.Button(action,Q::Repeat,pad),"Repeat fired early");
            input.Update(samples,.2f);Check(input.Button(action,Q::Repeat,pad),"Original delay boundary missed");
            input.Update(samples,.05f);Check(!input.Button(action,Q::Repeat,pad),"Original repeat interval ignored");
            input.Update(samples,.05f);Check(input.Button(action,Q::Repeat,pad),"Original repeat interval boundary missed");
            samples[pad].buttons=0;input.Update(samples,0);Check(input.Button(action,Q::Released,pad)&&!input.Button(action,Q::Held,pad),"Original release differs");
            input.Update(samples,0);Check(!input.Button(action,Q::Released,pad),"Release repeated");
        }
    }
    samples[1].buttons=samples[2].buttons=0x100; input.Update(samples,0);
    int found=-1;Check(input.Button(A::Accept,Q::Pressed,-1,&found)&&found==1,"Pad iteration order differs");
    input.EnablePad(1,false);Check(input.Button(A::Accept,Q::Pressed,-1,&found)&&found==2,"Disabled pad was used");
    int first=1,second=2;
    input.PushFocus(&first); input.Focus(&second); Check(!input.Button(A::Accept,Q::Held),"Unfocused scene consumed input");
    input.Focus(&first); Check(input.Button(A::Accept,Q::Held),"Focused scene lost input");
    input.PushFocus(&second);Reject([&]{input.PopFocus(&first);});
    Check(input.HasFocusLock(&second),"Focus lock owner differs");input.PopFocus(&second);input.PopFocus(&first);
    Reject([&]{input.PopFocus(&first);});Reject([&]{input.PushFocus(nullptr);});
    for(int i=0;i<4;++i)input.PushFocus(&first);Reject([&]{input.PushFocus(&first);});
    input.Reset();Check(!input.HasFocusLock(&first)&&!input.Button(A::Accept,Q::Held),"Reset retained locks/buttons");
    samples={};samples[0].connected=true;samples[0].left_x=-1;samples[0].left_y=1;
    input.EnableAnalogDirections(true);input.Update(samples,0);
    Check(input.Button(A::Left,Q::Pressed)&&input.Button(A::Up,Q::Pressed),"Analog directions not mapped");
    Check(g_pPadManager->GetPad(0)->m_polarAnalogLeft.a>0,"Original polar update missing");
    samples[0].left_y=-1;input.Update(samples,.1f);Check(g_pPadManager->GetPad(0)->m_polarAnalogLeft.a>32768,"Negative original angle did not wrap");
    samples[0].connected=false;input.Update(samples,0);Check(!input.Connected(0)&&!input.Button(A::Left,Q::Released),"Disconnect leaked an edge");
    const auto valid=samples;
    samples[0].buttons=0x2000;Reject([&]{input.Update(samples,0);});samples=valid;
    samples[0].left_x=std::numeric_limits<float>::quiet_NaN();Reject([&]{input.Update(samples,0);});samples=valid;
    Reject([&]{input.Update(samples,-1);});Reject([&]{input.Update(samples,std::numeric_limits<float>::infinity());});
    Reject([&]{input.Button(A::Accept,Q::Held,4);});Reject([&]{input.Button(A::Accept,Q::Held,-2);});
    Reject([&]{input.Button(static_cast<A>(0),Q::Held);});Reject([&]{input.Connected(4);});
    Reject([&]{input.SetRepeat(A::Accept,-1,.1f);});
    Reject([&]{g_pPadManager->GetPad(0)->StartRumble(1,1,1);});
    Reject([&]{g_pPadManager->GetPad(0)->StopRumble();});
    bool rejected=false;std::thread foreign([&]{try{input.Update(samples,0);}catch(const std::logic_error&){rejected=true;}});foreign.join();Check(rejected,"Foreign thread changed original input");
}
void Mapping()
{
    FrontendInput input;FrontendInputMap map;FrontendInputDevices devices;
    auto sample=[&](FrontendInputCapture capture=FrontendInputCapture{}){map.Update(input,devices,capture,.016f);};
    devices.keys[SDL_SCANCODE_RETURN]=true;sample();Check(!input.Button(A::Accept,Q::Pressed),"Initially held keyboard was admitted");
    devices.keys[SDL_SCANCODE_RETURN]=false;sample();devices.keys[SDL_SCANCODE_RETURN]=true;sample();Check(input.Button(A::Accept,Q::Pressed),"Keyboard accept not connected to original FEInput");
    devices.pads[0].id=12;sample();
    sample({true,true,false});Check(!input.Button(A::Accept,Q::Released)&&!input.Button(A::Accept,Q::Held),"Keyboard capture leaked release into neutral gamepad");
    sample();Check(!input.Button(A::Accept,Q::Pressed),"Capture exit admitted held keyboard");
    devices.keys[SDL_SCANCODE_RETURN]=false;sample();
    devices.pads[0].buttons[SDL_GAMEPAD_BUTTON_SOUTH]=true;sample();Check(input.Button(A::Accept,Q::Pressed),"Gamepad accept mapping differs");
    sample({true,false,true});Check(!input.Button(A::Accept,Q::Released),"Gamepad capture leaked release into keyboard");
    devices.keys[SDL_SCANCODE_ESCAPE]=true;sample({true,false,true});Check(input.Button(A::Back,Q::Pressed),"Captured gamepad disabled keyboard");
    sample({false,false,false});Check(!input.Button(A::Back,Q::Released),"Focus loss leaked an action");
    sample();Check(!input.Button(A::Back,Q::Pressed),"Held focus-return input was admitted");
    devices={};sample();
    devices.pads[3].id=44;devices.pads[3].buttons[SDL_GAMEPAD_BUTTON_START]=true;sample();Check(!input.Button(A::Start,Q::Pressed),"New pad admitted held input");
    devices.pads[3].buttons[SDL_GAMEPAD_BUTTON_START]=false;sample();devices.pads[3].buttons[SDL_GAMEPAD_BUTTON_START]=true;sample();
    int found=-1;Check(input.Button(A::Start,Q::Pressed,-1,&found)&&found==3,"Fourth desktop pad not routed");
    devices.pads[3].id=0;sample();Check(!input.Button(A::Start,Q::Released),"Hot unplug leaked release");
    input.EnableAnalogDirections(true);devices.pads[1].id=8;sample();devices.pads[1].left_y=-32768;sample();Check(input.Button(A::Up,Q::Pressed,1),"SDL axis sign differs");
    sample({true,false,true});Check(!input.Button(A::Up,Q::Released,1),"Captured analog direction leaked release");
    // A rejected update with neutral input cannot unlock held keys on the next
    // valid update. This exercises the same transaction used by production Poll.
    FrontendInputMap transactional;
    devices={};devices.keys[SDL_SCANCODE_RETURN]=true;
    transactional.Update(input,devices,{},.016f);
    devices.keys[SDL_SCANCODE_RETURN]=false;
    Reject([&]{transactional.Update(input,devices,{},-1);});
    devices.keys[SDL_SCANCODE_RETURN]=true;
    transactional.Update(input,devices,{},.016f);
    Check(!input.Button(A::Accept,Q::Held),"Rejected time consumed keyboard neutral gate");
    devices.keys[SDL_SCANCODE_RETURN]=false;transactional.Update(input,devices,{},.016f);
    devices.keys[SDL_SCANCODE_RETURN]=true;transactional.Update(input,devices,{},.016f);
    Check(input.Button(A::Accept,Q::Pressed),"Valid neutral gate did not admit later press");
}
void SDLRead()
{
    {FrontendInputSDL uninitialized;Reject([&]{uninitialized.ReadDevices();});}
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0xFFFF/0xFE01");
    Check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD),SDL_GetError());
    {
        FrontendInput input;FrontendInputSDL source;
        auto* window=SDL_CreateWindow("Frontend input test",160,120,SDL_WINDOW_HIDDEN);Check(window,SDL_GetError());
        source.Poll(input,window,.016f);Check(!input.Button(A::Accept,Q::Pressed),"Hidden window consumed input");
        Reject([&]{source.Poll(input,nullptr,0);});
        SDL_VirtualJoystickDesc desc{};desc.version=sizeof(desc);desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id=0xffff;desc.product_id=0xfe01;
        desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;desc.name="Charged frontend fixture";
        const auto id=SDL_AttachVirtualJoystick(&desc);Check(id!=0,SDL_GetError());
        auto* joystick=SDL_OpenJoystick(id);Check(joystick,SDL_GetError());
        SDL_UpdateJoysticks();SDL_PumpEvents();
        auto devices=source.ReadDevices();auto slot=std::find_if(devices.pads.begin(),devices.pads.end(),[&](auto p){return p.id==id;});
        Check(slot!=devices.pads.end(),"Real SDL virtual gamepad was not discovered");
        FrontendInputMap mapping;
        mapping.Update(input,devices,{},.016f);
        Check(!input.Button(A::Accept,Q::Pressed),"Neutral SDL pad generated a press");
        Check(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true),SDL_GetError());
        SDL_UpdateJoysticks();SDL_PumpEvents();devices=source.ReadDevices();slot=std::find_if(devices.pads.begin(),devices.pads.end(),[&](auto p){return p.id==id;});
        Check(slot!=devices.pads.end()&&slot->buttons[SDL_GAMEPAD_BUTTON_SOUTH],"SDL button did not reach device snapshot");
        mapping.Update(input,devices,{},.016f);
        Check(input.Button(A::Accept,Q::Pressed),"Real SDL virtual button did not reach original FEInput");
        SDL_CloseJoystick(joystick);Check(SDL_DetachVirtualJoystick(id),SDL_GetError());SDL_PumpEvents();
        devices=source.ReadDevices();Check(std::none_of(devices.pads.begin(),devices.pads.end(),[&](auto p){return p.id==id;}),"Detached SDL gamepad retained its slot");
        mapping.Update(input,devices,{},.016f);
        Check(!input.Button(A::Accept,Q::Released),"Real SDL detach leaked a frontend release");
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}
}
int main()
{
    try
    {
        for(int i=0;i<3;++i){Core();Mapping();Check(!g_pPadManager&&!g_pFEInput,"Original input globals survived teardown");}
        SDLRead();std::cout<<checks<<" original frontend input and SDL checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
