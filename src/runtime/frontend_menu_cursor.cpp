#include "runtime/frontend_menu_cursor.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool value,const char* reason){if(!value)throw std::logic_error(reason);}
bool Neutral(const FrontendInputSnapshot& s)
{
    const auto& k=s.devices.keys;const auto& p=s.devices.pads[0];
    const bool keyboard=s.capture.keyboard || !(k[SDL_SCANCODE_LEFT]||k[SDL_SCANCODE_RIGHT]||k[SDL_SCANCODE_UP]||k[SDL_SCANCODE_DOWN]||k[SDL_SCANCODE_RETURN]||k[SDL_SCANCODE_SPACE]);
    const bool pad=s.capture.gamepad || !p.id || !(p.buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT]||p.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]||p.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP]||p.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN]||p.buttons[SDL_GAMEPAD_BUTTON_SOUTH]||std::abs(int(p.left_x))>5898||std::abs(int(p.left_y))>5898);
    return keyboard&&pad;
}
void Validate(const FrontendPointerViewport& v)
{
    Require(v.window&&v.window_width&&v.window_height&&v.pixel_width&&v.pixel_height,"Menu cursor requires a live acknowledged viewport");
    for(auto n:{v.window_width,v.window_height,v.pixel_width,v.pixel_height,v.screen_width,v.screen_height})Require(n<=65535,"Menu cursor viewport exceeds bounds");
    Require(v.screen_width>=40&&v.screen_height>=10,"Menu cursor logical viewport is too small");
    Require(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.width)&&std::isfinite(v.height)&&v.x>=0&&v.y>=0&&v.width>0&&v.height>0&&v.x+v.width<=v.pixel_width&&v.y+v.height<=v.pixel_height,"Menu cursor content rectangle is invalid");
}
}
struct FrontendMenuCursor::Implementation
{
    const std::thread::id thread=std::this_thread::get_id();
    FrontendSession::Handle frame;FrontendPointerViewport view{};std::uint64_t scene=0,sequence=0,epoch=1,mouse_id=0;SDL_JoystickID pad=0;
    FrontendMenuCursorStatus status;FrontendCursorMouse previous_mouse;bool observed=false,virtual_blocked=true,suspended=false;
    FrontendInputCapture previous_capture{};
    std::vector<SDL_MouseID> mice;std::uint64_t mouse_epoch=1;
    void Ready()const{Require(thread==std::this_thread::get_id(),"Menu cursor requires its owner thread");}
    void Epoch(){Require(epoch<std::numeric_limits<std::uint64_t>::max(),"Menu cursor device epoch exhausted");++epoch;}
    void Clamp(){const int width=view.widescreen?854:view.screen_width,height=view.screen_height;status.position[0]=std::clamp(status.position[0],float(-width/2+20),float(width/2-20));status.position[1]=std::clamp(status.position[1],float(-height/2+5),float(height/2-5));}
};
FrontendMenuCursor::FrontendMenuCursor():impl_(std::make_unique<Implementation>()){}
FrontendMenuCursor::~FrontendMenuCursor(){if(impl_->thread!=std::this_thread::get_id())std::terminate();}
void FrontendMenuCursor::Acknowledge(std::uint64_t scene,FrontendSession::Handle frame,FrontendPointerViewport view)
{
    auto& s=*impl_;s.Ready();Validate(view);Require(scene&&frame,"Menu cursor requires an actual retained scene publication");
    const bool change=s.scene!=scene||s.view!=view||!s.frame||s.frame->visuals!=frame->visuals||s.frame->images!=frame->images||s.frame->request.path!=frame->request.path;
    if(change){s.Epoch();s.virtual_blocked=true;s.observed=false;s.status.position={};}s.frame=std::move(frame);s.scene=scene;s.view=view;s.suspended=false;
}
FrontendPointerDesktopSample FrontendMenuCursor::Sample(const FrontendInputSnapshot& input,const FrontendCursorMouse& mouse)
{
    auto& s=*impl_;s.Ready();Require(s.frame&&!s.suspended,"Menu cursor has no matching acknowledged viewport");
    Require(input.window==s.view.window&&input.sequence>s.sequence,"Menu cursor input snapshot is stale or from another window");
    Require(std::isfinite(input.delta)&&input.delta>=0&&input.delta<=.1f,"Menu cursor delta exceeds bounded host policy");
    Require(std::isfinite(mouse.x)&&std::isfinite(mouse.y)&&std::abs(double(mouse.x))<=1e9&&std::abs(double(mouse.y))<=1e9&&mouse.connected==(mouse.device!=0),"Menu cursor mouse identity/coordinates invalid");
    const auto& mapped=input.mapped[0];Require(std::isfinite(mapped.left_x)&&std::isfinite(mapped.left_y)&&std::abs(mapped.left_x)<=1&&std::abs(mapped.left_y)<=1,"Menu cursor analog sample invalid");
    auto next=s; // Validate/compute before committing pointer policy state.
    const bool capture_changed=input.capture.focused!=s.previous_capture.focused||input.capture.keyboard!=s.previous_capture.keyboard||input.capture.gamepad!=s.previous_capture.gamepad;
    if(capture_changed||input.devices.pads[0].id!=s.pad){next.Epoch();next.virtual_blocked=true;}
    if(mouse.device!=s.mouse_id)next.Epoch();
    if(!input.capture.focused)next.virtual_blocked=true;
    if(next.virtual_blocked&&input.capture.focused&&Neutral(input))next.virtual_blocked=false;
    float x=float(bool(mapped.buttons&2))-float(bool(mapped.buttons&1));float y=float(bool(mapped.buttons&8))-float(bool(mapped.buttons&4));
    if(x==0)x=mapped.left_x;if(y==0)y=mapped.left_y;
    if(mapped.suppress_edges||next.virtual_blocked||!input.capture.focused||(input.capture.keyboard&&input.capture.gamepad))x=y=0;
    const bool mouse_active=mouse.connected&&mouse.focused&&!mouse.captured;
    const bool mouse_moved=s.observed&&(std::abs(mouse.x-s.previous_mouse.x)>.5f||std::abs(mouse.y-s.previous_mouse.y)>.5f);
    auto source=next.status.source;
    if(mouse_active&&(mouse_moved||(mouse.down&&!s.previous_mouse.down)))source=FrontendCursorSource::Mouse;
    else if(x!=0||y!=0)source=FrontendCursorSource::KeyboardGamepad;
    else if(!s.observed&&!mouse.connected)source=FrontendCursorSource::KeyboardGamepad;
    if(source!=next.status.source){next.Epoch();next.status.source=source;}
    const int width=s.view.widescreen?854:s.view.screen_width;
    FrontendPointerDesktopSample out{s.view.window,s.view.window_width,s.view.window_height,s.view.pixel_width,s.view.pixel_height,input.sequence,next.epoch};
    if(source==FrontendCursorSource::Mouse)
    {
        out.x=mouse.x;out.y=mouse.y;out.connected=mouse.connected;out.focused=mouse.focused;out.captured=mouse.captured;out.primary_down=mouse.down;
        if(!out.connected)out.device=0;
        if(mouse_active){next.status.position={float((double(mouse.x)*s.view.pixel_width/s.view.window_width-s.view.x)/s.view.width*width-width/2.),float(s.view.screen_height/2.-(double(mouse.y)*s.view.pixel_height/s.view.window_height-s.view.y)/s.view.height*s.view.screen_height)};next.Clamp();}
    }
    else
    {
        const float length=std::hypot(x,y);if(length>1){x/=length;y/=length;}
        next.status.position[0]+=x*480.f*input.delta;next.status.position[1]+=y*480.f*input.delta;next.Clamp();
        out.x=float((s.view.x+(double(next.status.position[0])/width+.5)*s.view.width)*s.view.window_width/s.view.pixel_width);
        out.y=float((s.view.y+(.5-double(next.status.position[1])/s.view.screen_height)*s.view.height)*s.view.window_height/s.view.pixel_height);
        out.connected=true;out.focused=input.capture.focused;out.captured=next.virtual_blocked||(input.capture.keyboard&&input.capture.gamepad);
    }
    next.sequence=input.sequence;next.previous_mouse=mouse;next.observed=true;next.pad=input.devices.pads[0].id;next.mouse_id=mouse.device;next.previous_capture=input.capture;
    // The creating thread is immutable; publish only mutable state.
    s.frame=next.frame;s.view=next.view;s.scene=next.scene;s.sequence=next.sequence;s.epoch=next.epoch;s.mouse_id=next.mouse_id;s.pad=next.pad;s.status=next.status;s.previous_mouse=next.previous_mouse;s.observed=next.observed;s.virtual_blocked=next.virtual_blocked;s.previous_capture=next.previous_capture;
    return out;
}
std::optional<FrontendPointerDesktopSample> FrontendMenuCursor::Poll(const FrontendInputSnapshot& input,SDL_Window* window,bool capture)
{
    auto& s=*impl_;s.Ready();Require(s.frame&&window&&SDL_IsMainThread()&&SDL_GetWindowID(window)==s.view.window,"Menu cursor polling requires its rendered SDL window");
    Require(!SDL_GetWindowRelativeMouseMode(window),"Menu cursor requires absolute SDL mouse mode");
    int ww=0,wh=0,pw=0,ph=0;Require(SDL_GetWindowSize(window,&ww,&wh)&&SDL_GetWindowSizeInPixels(window,&pw,&ph),"Cannot read menu cursor viewport");
    if(ww<=0||wh<=0||pw<=0||ph<=0||unsigned(ww)!=s.view.window_width||unsigned(wh)!=s.view.window_height||unsigned(pw)!=s.view.pixel_width||unsigned(ph)!=s.view.pixel_height){s.suspended=true;s.virtual_blocked=true;return {};}
    if(s.suspended)return {};
    int count=0;std::unique_ptr<SDL_MouseID,decltype(&SDL_free)> ids(SDL_GetMice(&count),SDL_free);
    Require(bool(ids)&&count>=0&&count<=1024,"Cannot enumerate menu cursor mice");
    std::vector<SDL_MouseID> mice(ids.get(),ids.get()+count);std::sort(mice.begin(),mice.end());
    const bool changed=mice!=s.mice;Require(!changed||s.mouse_epoch<std::numeric_limits<std::uint64_t>::max(),"Menu cursor mouse identity exhausted");
    const auto identity=s.mouse_epoch+unsigned(changed);
    FrontendCursorMouse mouse;mouse.connected=SDL_HasMouse();mouse.device=mouse.connected?identity:0;
    mouse.down=(SDL_GetMouseState(&mouse.x,&mouse.y)&SDL_BUTTON_LMASK)!=0;
    const auto flags=SDL_GetWindowFlags(window);mouse.focused=SDL_GetMouseFocus()==window&&(flags&SDL_WINDOW_INPUT_FOCUS)&&!(flags&(SDL_WINDOW_HIDDEN|SDL_WINDOW_MINIMIZED));mouse.captured=capture;
    auto result=Sample(input,mouse);s.mice=std::move(mice);s.mouse_epoch=identity;return result;
}
FrontendMenuCursorStatus FrontendMenuCursor::Status()const{impl_->Ready();return impl_->status;}
}
