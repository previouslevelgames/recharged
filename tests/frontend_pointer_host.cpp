#include "runtime/frontend_pointer_host.h"
#include "Game/FE/FrontendPointerHostSteps.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>
#include <thread>

thread_local long allocation_budget = -1;
void* operator new(std::size_t n)
{
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool b, const char* message) { ++checks; if (!b) throw std::runtime_error(message); }
template<class F> void Reject(F f, std::source_location at = std::source_location::current())
{
    ++checks; try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid pointer host operation accepted at " + std::to_string(at.line()));
}
void Near(float a, double b, double tolerance = .0001, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!std::isfinite(a) || std::abs(double(a) - b) > tolerance)
        throw std::runtime_error("Pointer coordinate oracle differs at " + std::to_string(at.line()) + ": " + std::to_string(a) + " != " + std::to_string(b));
}
FrontendSession::Handle Frame()
{
    auto f = std::make_shared<FrontendSessionFrame>();
    resources::FrontendLibraryObject library{}; library.offset = 10; library.type = 1; library.attributes.scale = {2,1,1};
    f->graph.library.push_back(library);
    resources::FrontendInstance image{}; image.offset = 100; image.type = 2; image.library = 10;
    f->graph.instances.push_back(image);
    return f;
}
FrontendPointerViewport View()
{ return {7, 800, 600, 1600, 1200, 160, 120, 1280, 960}; }
FrontendPointerDesktopSample Sample(const FrontendPointerViewport& v, std::uint64_t seq = 1)
{ return {v.window, v.window_width, v.window_height, v.pixel_width, v.pixel_height, seq, 11, 400, 300, true, true}; }
void Update(FrontendInput& in, bool down = false, unsigned index = 0)
{
    std::array<FrontendPadSample,4> sample{}; sample[index].connected = true;
    sample[index].buttons = down ? 0x100 : 0; in.Update(sample, .016f);
}
void SharedSource()
{
    struct E { int mIndex = -1; nlVector2 mPosition{}; bool mPressed = false, mReleased = true, mAuxiliaryTriggered = true; } event;
    struct S
    {
        E& e; int calls = 0;
        nlVector2 Position(int i) { Check(i == 3 && e.mIndex == 3 && calls++ == 0, "Original index/position order differs"); return {12,34}; }
        bool JustPressed(int i) { Check(i == 3 && e.mPosition.x == 12 && e.mPosition.y == 34 && calls++ == 1, "Original position/action30 order differs"); return true; }
    } source{event};
    FrontendOptionsPointerEvent(event, 3, source);
    Check(event.mPressed && event.mReleased && event.mAuxiliaryTriggered && source.calls == 2, "Producer overwrote untouched event defaults");
    for (int width : {40, 641, 854, 65535}) for (int height : {10, 481, 65535})
        for (float x : {-2.f, -1.f, -.321f, -0.f, 0.f, .987f, 1.f, 2.f})
            for (float y : {-2.f, -1.f, -.125f, 0.f, .75f, 1.f, 2.f})
            {
                nlVector2 v{x,y}; FrontendDPDToAssetPosition(v, width, height);
                // Exact source operation order, independently retained expected
                // float products; wide host-coordinate oracle is checked below.
                float ex = -1.f * (x * float(width) / 2.f), ey = y * float(height) / 2.f;
                Check(std::bit_cast<std::uint32_t>(v.x) == std::bit_cast<std::uint32_t>(ex)
                    && std::bit_cast<std::uint32_t>(v.y) == std::bit_cast<std::uint32_t>(ey), "Original normalized float bits differ");
                v = FrontendClampPointerPosition(v, width, height);
                Near(v.x, std::clamp(ex, float(-width/2+20), float(width/2-20)));
                Near(v.y, std::clamp(ey, float(-height/2+5), float(height/2-5)));
            }
}
void Coordinates()
{
    FrontendInput input; Update(input); FrontendPointerHost host(input);
    auto frame = Frame(); std::uint64_t seq = 0;
    for (auto v : {View(), FrontendPointerViewport{7,853,641,1706,1282,13.5,9.25,1657,1250,641,481,false},
        FrontendPointerViewport{7,1200,800,1800,1200,0,94.5,1800,1011,640,480,true}})
    {
        auto token = host.Publish(frame,v,{}); auto sample = Sample(v,++seq);
        sample.x = float((v.x+v.width/2)*v.window_width/v.pixel_width);
        sample.y = float((v.y+v.height/2)*v.window_height/v.pixel_height);
        Check(!host.Route(token,sample).active,"New viewport did not require neutral");
        const unsigned logical_width = v.widescreen ? 854 : v.screen_width;
        for (int ix = 0; ix < 13; ++ix) for (int iy = 0; iy < 11; ++iy)
        {
            sample.x = float((v.x+v.width*ix/13.)*v.window_width/v.pixel_width);
            sample.y = float((v.y+v.height*iy/11.)*v.window_height/v.pixel_height); sample.sequence=++seq;
            auto result=host.Route(token,sample);
            const double px=double(sample.x)*v.pixel_width/v.window_width;
            const double py=double(sample.y)*v.pixel_height/v.window_height;
            const bool inside=px>=v.x&&py>=v.y&&px<v.x+v.width&&py<v.y+v.height;
            if(!inside){Check(!result.active,"Rounded outside coordinate accepted");continue;}
            if(!result.active){sample.sequence=++seq;result=host.Route(token,sample);}
            Check(result.active&&!result.event.pressed&&!result.event.released&&!result.event.unidentified,"Neutral coordinate event differs");
            Near(result.event.position[0],std::clamp(((px-v.x)/v.width-.5)*logical_width,
                double(-int(logical_width)/2+20),double(int(logical_width)/2-20)));
            Near(result.event.position[1],std::clamp((.5-(py-v.y)/v.height)*v.screen_height,
                double(-int(v.screen_height)/2+5),double(int(v.screen_height)/2-5)));
        }
    }
}
void Routing()
{
    FrontendInput input;Update(input);auto frame=Frame();auto v=View();
    std::vector<FrontendPointerCallback> log;FrontendPointerHost host(input);
    auto region=std::make_shared<FrontendPointerRegion>(input,frame,FrontendPointerBinding{100},
        [&](auto kind,unsigned index,const auto& f){Check(index==0&&f==frame,"Routed listener identity differs");log.push_back(kind);});
    const std::array listeners{region};auto token=host.Publish(frame,v,listeners);auto sample=Sample(v);
    host.Route(token,sample);sample.sequence++;
    auto result=host.Route(token,sample);
    Check(result.active&&result.listeners==1&&log==std::vector{FrontendPointerCallback::Enter,FrontendPointerCallback::Update,FrontendPointerCallback::Inside},"Original entry order differs");
    log.clear();sample.primary_down=true;sample.sequence++;
    result=host.Route(token,sample);Check(result.event.pressed&&log.back()==FrontendPointerCallback::Press,"Mouse down did not route press");
    sample.sequence++;Check(!host.Route(token,sample).event.pressed,"Held mouse repeated a press");
    sample.primary_down=false;sample.sequence++;Check(!host.Route(token,sample).event.released,"Options profile invented a release");
    Update(input,true);sample.sequence++;Check(host.Route(token,sample).event.pressed,"Original action30 press did not route");
    sample.sequence++;Check(!host.Route(token,sample).event.pressed,"Duplicate original input observation repeated press");
    Update(input,false);sample.sequence++;host.Route(token,sample);
    for(unsigned mode=0;mode<4;++mode)
    {
        sample.primary_down=true;sample.sequence++;host.Route(token,sample);log.clear();
        if(mode==0)sample.focused=false;
        if(mode==1)sample.captured=true;
        if(mode==2){sample.connected=false;sample.device=0;}
        if(mode==3)sample.x=79; // left letterbox edge is80 window units.
        sample.sequence++;result=host.Route(token,sample);
        Check(!result.active&&!result.event.pressed&&!result.event.released,"Inactive mouse leaked an edge");
        Check(log==std::vector{FrontendPointerCallback::Leave},"Inactive pointer did not use original leave order");
        sample.focused=true;sample.captured=false;sample.connected=true;sample.device=11;sample.x=400;
        sample.sequence++;Check(!host.Route(token,sample).active,"Held activation escaped neutral suppression");
        sample.primary_down=false;sample.sequence++;Check(!host.Route(token,sample).active,"Neutral observation activated early");
        sample.sequence++;Check(host.Route(token,sample).active,"Neutral pointer failed to recover");
    }
    // Device replacement within one connected desktop sample requires neutral.
    sample.device=12;sample.primary_down=true;sample.sequence++;Check(!host.Route(token,sample).active,"Hotplug held button escaped gate");
    sample.primary_down=false;sample.sequence++;host.Route(token,sample);sample.sequence++;host.Route(token,sample);
    int focus=0;input.PushFocus(&focus);log.clear();sample.sequence++;host.Route(token,sample);
    Check(log.empty(),"Original listener input lock bypassed");
    region->IgnoreInputLock(true);sample.sequence++;host.Route(token,sample);Check(!log.empty(),"Original explicit ignore lock was lost");
    region->IgnoreInputLock(false);input.PopFocus(&focus);
    auto old=token;token=host.Publish(frame,v,listeners);sample.sequence++;
    Reject([&]{host.Route(old,sample);});Check(host.Route(token,sample).active,"Frame refresh unnecessarily reset neutral state");
    auto resized=v;resized.window_width=801;old=token;token=host.Publish(frame,resized,listeners);
    sample.sequence++;Reject([&]{host.Route(token,sample);});sample.window_width=801;
    sample.primary_down=true;Check(!host.Route(token,sample).active,"Resize leaked held edge");
    sample.primary_down=false;sample.sequence++;host.Route(token,sample);
    auto next=Frame();region->Rebind(next,{100});sample.sequence++;Reject([&]{host.Route(token,sample);});
    token=host.Publish(next,resized,listeners);frame=next;host.Route(token,sample);
    std::weak_ptr<const FrontendSessionFrame> retained=frame;frame.reset();next.reset();old.reset();region.reset();
    Check(!retained.expired(),"Host lost retained scene/listener ownership");
    host.Release();Reject([&]{host.Current();});host.Release();
    token.reset();Check(!retained.expired(),"External listener array did not retain its scene");
}
void Failures()
{
    FrontendInput input;Update(input);auto frame=Frame();auto v=View();FrontendPointerHost host(input);
    unsigned calls=0;bool fail=false,reenter=false;
    auto region=std::make_shared<FrontendPointerRegion>(input,frame,FrontendPointerBinding{100},
        [&](auto kind,unsigned,const auto&){++calls;if(reenter){Reject([&]{host.Release();});Reject([&]{host.Reset();});Reject([&]{host.Publish(frame,v,{});});Check(bool(host.Current()),"Callback lost query access");}if(fail&&kind==FrontendPointerCallback::Enter)throw std::runtime_error("injected callback failure");});
    std::array listeners{region};auto token=host.Publish(frame,v,listeners);auto sample=Sample(v);
    host.Route(token,sample);sample.sequence++;host.Route(token,sample);reenter=true;
    sample.sequence++;host.Route(token,sample);reenter=false;
    region->Disable();region->Enable();fail=true;sample.primary_down=true;sample.sequence++;Reject([&]{host.Route(token,sample);});Check(host.Failed(),"Callback failure did not poison host");
    Reject([&]{host.Route(token,sample);});Reject([&]{host.Publish(frame,v,listeners);});
    // Failed source event does not commit previous history. Direct checked
    // listener retry re-enters, while host intentionally requires reset.
    fail=false;const auto previous=calls;region->Deliver(frame,{0,{0,0},true});Check(calls==previous+4,"Failed source event history changed");
    host.Reset();Check(!host.Failed()&&region->Enabled(),"Reset failed to restore explicit enabled state");
    sample.primary_down=false;sample.sequence++;host.Route(token,sample);sample.sequence++;host.Route(token,sample);
    region->Disable();host.Reset();Check(!region->Enabled(),"Reset enabled a disabled listener");region->Enable();
    auto wrong=Frame();Reject([&]{host.Publish(wrong,v,listeners);});
    const std::array duplicate{region,region};Reject([&]{host.Publish(frame,v,duplicate);});
    for(long n=0;n<3;++n)
    {
        allocation_budget=n;bool rejected=false;try{host.Publish(frame,v,listeners);}catch(const std::bad_alloc&){rejected=true;}allocation_budget=-1;
        Check(rejected&&host.Current()==token,"Failed candidate publication replaced visible token");
    }
    auto forged=std::make_shared<FrontendPointerPresentation>(*token);sample.sequence++;
    Reject([&]{host.Route(forged,sample);});
    auto invalid=sample;invalid.sequence=0;Reject([&]{host.Route(token,invalid);});
    invalid=sample;invalid.window++;Reject([&]{host.Route(token,invalid);});
    invalid=sample;invalid.pixel_width++;Reject([&]{host.Route(token,invalid);});
    invalid=sample;invalid.x=NAN;Reject([&]{host.Route(token,invalid);});
    invalid=sample;invalid.y=INFINITY;Reject([&]{host.Route(token,invalid);});
    invalid=sample;invalid.connected=false;Reject([&]{host.Route(token,invalid);});
    for(unsigned bad=0;bad<8;++bad)
    {
        auto extent=v;
        switch(bad){case 0:extent.window=0;break;case 1:extent.window_width=0;break;case 2:extent.pixel_width=70000;break;case 3:extent.x=-1;break;case 4:extent.width=INFINITY;break;case 5:extent.height=0;break;case 6:extent.screen_width=39;break;case 7:extent.screen_height=9;break;}
        Reject([&]{host.Publish(frame,extent,listeners);});Check(host.Current()==token,"Invalid viewport changed publication");
    }
    bool rejected=false;std::thread thread([&]{try{host.Current();}catch(const std::exception&){rejected=true;}});thread.join();Check(rejected,"Wrong-thread pointer query accepted");
    Reject([&]{FrontendPointerHost invalid_host(input,4);});
    region->Release();Reject([&]{host.Route(token,sample);});host.Release();
}
void RetentionAndDispatchFailure()
{
    FrontendInput input;Update(input);auto frame=Frame();auto view=View();
    FrontendPointerHost host(input);bool mutate=false;
    auto second=std::make_shared<FrontendPointerRegion>(input,frame,FrontendPointerBinding{100});
    auto first=std::make_shared<FrontendPointerRegion>(input,frame,FrontendPointerBinding{100},
        [&](auto kind,unsigned,const auto&){if(mutate&&kind==FrontendPointerCallback::Enter)second->Release();});
    const std::array listeners{first,second};auto token=host.Publish(frame,view,listeners);auto sample=Sample(view);
    host.Route(token,sample);sample.sequence++;mutate=true;Reject([&]{host.Route(token,sample);});
    Check(host.Failed(),"Mutation of a later listener did not poison partial dispatch");
    Reject([&]{host.Reset();});host.Release();
    struct Probe
    {
        FrontendPointerHost* host=nullptr;
        bool* armed=nullptr;
        ~Probe(){if(*armed){Reject([&]{host->Release();});Reject([&]{host->Reset();});}}
    };
    // Destroying removed listener captures must not mutate the host while its
    // old listener list is being retired, even though no event is dispatching.
    FrontendPointerHost owner(input);bool armed=false;
    auto probe=std::make_shared<Probe>();probe->host=&owner;probe->armed=&armed;
    auto held=std::make_shared<FrontendPointerRegion>(input,frame,FrontendPointerBinding{100},
        [probe](auto,unsigned,const auto&){});
    auto old=owner.Publish(frame,view,std::array{held});held.reset();probe.reset();armed=true;
    auto next=owner.Publish(frame,view,{});Check(next!=old,"Replacing listener list lost new publication");
    armed=false;
    for(unsigned index=0;index<4;++index)
    {
        FrontendPointerHost pointer(input,index);Update(input,false,index);
        auto selected=pointer.Publish(frame,view,{});auto sample4=Sample(view);pointer.Route(selected,sample4);
        Update(input,true,index);sample4.sequence++;
        const auto result=pointer.Route(selected,sample4);
        Check(result.active&&result.event.index==index&&result.event.pressed,"Original action30 routed to wrong pointer index");
        Update(input,false,index);
    }
}
void SDLWindow()
{
    Check(SDL_Init(SDL_INIT_VIDEO),SDL_GetError());
    struct Quit{~Quit(){SDL_Quit();}} quit;
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> window(SDL_CreateWindow("frontend pointer fixture",640,480,SDL_WINDOW_HIDDEN),SDL_DestroyWindow);
    Check(bool(window),SDL_GetError());
    FrontendInput input;Update(input);FrontendPointerHost host(input);auto frame=Frame();
    int ww,wh,pw,ph;Check(SDL_GetWindowSize(window.get(),&ww,&wh)&&SDL_GetWindowSizeInPixels(window.get(),&pw,&ph),"SDL extent read failed");
    FrontendPointerViewport v{SDL_GetWindowID(window.get()),unsigned(ww),unsigned(wh),unsigned(pw),unsigned(ph),0,0,double(pw),double(ph)};
    unsigned leaves=0;
    auto region=std::make_shared<FrontendPointerRegion>(input,frame,FrontendPointerBinding{100},
        [&](auto kind,unsigned,const auto& source){Check(source==frame,"Resize callback lost its retained frame");if(kind==FrontendPointerCallback::Leave)++leaves;});
    auto token=host.Publish(frame,v,std::array{region});SDL_WarpMouseInWindow(window.get(),127.5f,230.25f);SDL_PumpEvents();
    const auto result=host.Poll(token,window.get());
    Check(!result.active&&!result.event.pressed,"Hidden SDL window admitted pointer input");
    Reject([&]{host.Poll(token,nullptr);});
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> other(SDL_CreateWindow("other fixture",640,480,SDL_WINDOW_HIDDEN),SDL_DestroyWindow);
    Check(bool(other),SDL_GetError());Reject([&]{host.Poll(token,other.get());});
    // Establish original listener history independently of this hidden window's
    // physical focus, then resize the actual SDL window before presentation.
    region->Deliver(frame,{0,{0,0}});
    Check(SDL_SetWindowSize(window.get(),800,600),SDL_GetError());SDL_PumpEvents();
    const auto resized=host.Poll(token,window.get());
    Check(!resized.active&&!resized.event.pressed&&resized.listeners==1&&leaves==1&&!host.Failed()
        &&host.Current()==token,"SDL resize did not leave the retained frame or poisoned the host");
    Check(!host.Poll(token,window.get()).active&&leaves==1,"Repeated resize invented another Leave");
    Check(SDL_GetWindowSize(window.get(),&ww,&wh)&&SDL_GetWindowSizeInPixels(window.get(),&pw,&ph),"Resized SDL extent read failed");
    v.window_width=ww;v.window_height=wh;v.pixel_width=pw;v.pixel_height=ph;v.width=pw;v.height=ph;
    token=host.Publish(frame,v,std::array{region});auto sample=Sample(v,100);sample.x=ww/2.f;sample.y=wh/2.f;sample.primary_down=true;
    Check(!host.Route(token,sample).active,"Held input crossed the resized presentation gate");
    sample.sequence++;sample.primary_down=false;Check(!host.Route(token,sample).active,"Resized presentation skipped neutral recovery");
    sample.sequence++;Check(host.Route(token,sample).active,"Resized presentation did not recover on neutral input");
    host.Release();
}
void Owned(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),{});
    Check(!bytes.empty(),"Owned FEN is missing");
    auto frame=std::make_shared<FrontendSessionFrame>();frame->graph=resources::ReadFrontendScene(bytes);
    // Actual exported image instances, tested only as pointer bounds. This does
    // not claim rendered resources, original Title/Main handler or menu actions.
    FrontendInput input;Update(input);FrontendPointerHost host(input);auto view=View();std::uint64_t seq=0;unsigned measured=0;
    for(const auto& instance:frame->graph.instances)
    {
        if(instance.type!=2||!instance.library)continue;
        auto region=std::make_shared<FrontendPointerRegion>(input,frame,FrontendPointerBinding{instance.offset});
        auto bounds=region->Bounds();const float x=(bounds.min_x+bounds.max_x)*.5f,y=(bounds.min_y+bounds.max_y)*.5f;
        if(x<=-300||x>=300||y<=-235||y>=235||bounds.min_x>bounds.max_x||bounds.min_y>bounds.max_y)continue;
        auto token=host.Publish(frame,view,std::array{region});auto sample=Sample(view,++seq);
        sample.x=400+x;sample.y=300-y;host.Route(token,sample);sample.sequence=++seq;
        auto result=host.Route(token,sample);Near(result.event.position[0],x);Near(result.event.position[1],y);
        Check(result.active&&region->Contains(result.event.position),"Owned pointer center did not route into original bounds");++measured;
    }
    Check(measured>0,"Owned FEN had no eligible image bounds");
    std::cout<<"Owned image centers "<<measured<<" in "<<path.filename()<<'\n';
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc>1){for(int i=1;i<argc;++i)Owned(argv[i]);}
        else{SharedSource();Coordinates();Routing();Failures();RetentionAndDispatchFailure();SDLWindow();}
        std::cout<<"Frontend host pointer checks "<<checks<<'\n';return 0;
    }
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}
}
