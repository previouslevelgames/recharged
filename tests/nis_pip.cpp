#include "runtime/nis_pip.h"
#include "runtime/startup.h"
#include "runtime/graphics_memory.h"
#include "Game/Camera/CameraMan.h"
#include "nis_pip_fixture.h"
#include <bit>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <source_location>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool valid, const char* message) { ++checks; if (!valid) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid PIP operation accepted"); }
void Bits(float a, float b, std::source_location source=std::source_location::current())
{
    ++checks;
    if (std::bit_cast<std::uint32_t>(a) != std::bit_cast<std::uint32_t>(b))
        throw std::runtime_error("PIP float bits differ at line "+std::to_string(source.line())+": "
            +std::to_string(a)+" vs "+std::to_string(b));
}
void Rectangle(const NisPip& pip, float x, float y, float width, float height)
{
    const auto rectangle = pip.Rectangle(); Check(bool(rectangle), "Expected PIP rectangle is missing");
    Bits(rectangle->x,x); Bits(rectangle->y,y); Bits(rectangle->width,width); Bits(rectangle->height,height);
}
void Position(float value, float expected)
{
    // Original GetLocalPoint/GetWorldPoint round through their trigonometric
    // helpers even for zero facing. Geometry has a small finite error; the
    // extracted PIP rectangle/timer arithmetic above is compared bit for bit.
    Check(std::isfinite(value)&&std::abs(value-expected)<.0001f,"Original camera position differs from analytic track");
}
struct Session
{
    OriginalCameras core;
    NisCameraAssets assets{nis_pip_fixture::Fixture(), "pip"};
    NisCameraBinding a{assets,0}, b{assets,1};
    NisCameras cameras{core};
    NisPlayback playback{cameras};
    Session() { cameras.Select(0,a,0); cameras.Select(1,b,1); cameras.Activate(); }
};
void Rectangles()
{
    Session session;
    for (float duration : {.125f,.3f,1.f,3.f})
    {
        NisPip pip(session.playback,duration);
        Check(pip.Mode()==NisPipMode::Pip,"PIP initial overlay differs from original NisPlayer");
        Rectangle(pip,360,270,240,150);
        pip.SetMode(NisPipMode::Expand);
        const auto primary=session.a.Camera();
        for (unsigned i=0;i<=16;++i)
        {
            pip.ResetExpansion(); pip.Update(duration*(i/16.f));
            const float t = float(double(pip.Time())/duration), u = float(1.-double(t));
            Rectangle(pip,float(360.*u),float(270.*u),float(double(float(640.*t))+float(240.*u)),
                float(double(float(480.*t))+float(150.*u)));
            Check(pip.Mode()==NisPipMode::Expand&&session.a.Camera()==primary,"PIP changed cameras at or before duration");
        }
        Rectangle(pip,0,0,640,480); pip.Update(0);
        Check(pip.Mode()==NisPipMode::Expand,"Exact expansion endpoint must render once before swap");
        pip.Update(std::nextafter(duration,INFINITY)-duration);
        Check(pip.Mode()==NisPipMode::None&&!pip.Rectangle()&&session.a.Camera()!=primary,"PIP did not swap strictly after duration");
        const auto swapped=session.a.Camera(); pip.Update(1);pip.Update(1);
        Check(session.a.Camera()==swapped,"Completed expansion swapped repeatedly");
        pip.SetMode(NisPipMode::Expand); Bits(pip.Time(),0);
        pip.Update(duration*.25f);pip.SetMode(NisPipMode::Expand);Bits(pip.Time(),duration*.25f);
        pip.ResetExpansion();Bits(pip.Time(),0);
        Check(session.a.Camera()==swapped,"Expansion reset replaced camera payload");
    }
}
void PlaybackAndSwap()
{
    Session session; NisPip pip(session.playback);
    session.core.Advance(0,0);
    const auto* identity=session.cameras.Camera(0); Position(identity->GetCameraPosition().x,10);
    std::vector<std::pair<std::size_t,unsigned>> events;
    session.playback.BindService(2,[&](const NisTriggerDispatch& event){events.emplace_back(event.table,event.render_mode);});
    for(unsigned mode:{0u,1u,2u})session.playback.AddTable({mode,{{2,0}}});
    pip.SetMode(NisPipMode::Swap); Check(!pip.Rectangle(),"Transient camera-swap overlay rendered");
    pip.Update(0);
    Check(pip.Mode()==NisPipMode::Pip&&session.a.RenderMode()==1&&session.b.RenderMode()==0,"Instant swap did not return to PIP or update bindings");
    session.core.Advance(0,0);
    Check(session.cameras.Camera(0)==identity&&cCameraManager::PeekCamera()==identity,"PIP replaced stable manager identity");
    Position(identity->GetCameraPosition().x,20); Rectangle(pip,360,270,240,150);
    auto step=session.playback.Advance({.125f,1,0x10}); if(step.active)pip.Update(step.delta);
    session.core.Advance(0,0);
    Bits(session.cameras.Time(0),.0625f); Bits(session.cameras.Time(1),.125f);
    Check(events==std::vector<std::pair<std::size_t,unsigned>>{{0,1},{1,0},{2,2}},"PIP bypassed original trigger render-mode swap");
    Position(identity->GetCameraPosition().x,20.f+59.f*.0625f*.01f);
    Bits(cCameraManager::m_cameraPosition.x,identity->GetCameraPosition().x);
    pip.SetMode(NisPipMode::Expand); step=session.playback.Advance({.1f,1,0x20}); if(step.active)pip.Update(step.delta);
    Bits(pip.Time(),0); // Callers preserve the original task-state gate.
    pip.SetMode(NisPipMode::None); Check(!pip.Rectangle(),"NoOverlay rendered a rectangle");
}
void Validation()
{
    Session session;
    for(float value:{0.f,-1.f,INFINITY,NAN})Reject([&]{NisPip invalid(session.playback,value);});
    NisPip pip(session.playback);
    for(float value:{-1.f,INFINITY,NAN})Reject([&]{pip.Update(value);});
    for(unsigned mode:{4u,5u,UINT32_MAX})Reject([&]{pip.SetMode(static_cast<NisPipMode>(mode));});
    Check(!pip.Failed()&&pip.Mode()==NisPipMode::Pip,"Argument rejection changed owner state");
    bool rejected=false; std::thread worker([&]{try{pip.Update(0);}catch(const std::logic_error&){rejected=true;}});worker.join();
    Check(rejected&&!pip.Failed(),"PIP accepted wrong-thread mutation or poisoned validation");
    NisPip huge(session.playback,std::numeric_limits<float>::max());huge.SetMode(NisPipMode::Expand);
    huge.Update(std::numeric_limits<float>::max()); Rectangle(huge,0,0,640,480);
    Reject([&]{huge.Update(std::numeric_limits<float>::max());});
    Check(!huge.Failed()&&huge.Mode()==NisPipMode::Expand,"Overflow mutated expansion state");
    NisPip tiny(session.playback,std::numeric_limits<float>::denorm_min());tiny.SetMode(NisPipMode::Expand);
    tiny.Update(std::numeric_limits<float>::denorm_min());Rectangle(tiny,0,0,640,480);
}
std::function<void()> on_transition;
unsigned transition_count=0;
void Transition(eCameraMessage) { ++transition_count; if(on_transition)on_transition(); }
void Callbacks(bool throw_error)
{
    Session session; CameraPoseInput extra; NisPip pip(session.playback);
    cCameraManager::PushCameraWithTransition(&extra,1,eCT_EASE_IN,Transition,false);
    on_transition=[&]{
        Reject([&]{pip.Update(0);});Reject([&]{pip.SetMode(NisPipMode::None);});Reject([&]{pip.ResetExpansion();});
        Reject([&]{pip.Rectangle();});Reject([&]{session.cameras.Release();});Reject([&]{session.core.Release();});
        if(throw_error)throw std::runtime_error("Injected PIP swap transition failure");
    };
    const auto before=transition_count; pip.SetMode(NisPipMode::Expand);
    if(throw_error)
    {
        Reject([&]{pip.Update(2);});Check(pip.Failed()&&session.playback.Failed(),"Escaping swap failure was not retained");
        Reject([&]{pip.Update(0);});Reject([&]{pip.Rectangle();});
    }
    else {pip.Update(2);Check(!pip.Failed()&&pip.Mode()==NisPipMode::None,"Caught reentry rejection poisoned PIP");}
    Check(transition_count==before+1,"Swap did not invoke original manager transition abort exactly once");
    on_transition={}; session.cameras.Release();
    Check(transition_count==before+1&&!cCameraManager::m_pCallback,"PIP cleanup invoked callbacks or retained transition");
}
void Lifetimes()
{
    Session session;
    auto pip=std::make_unique<NisPip>(session.playback);pip->SetMode(NisPipMode::Expand);pip->Update(.5f);
    session.core.Release(); OriginalCameras next;CameraPoseInput camera;cCameraManager::PushCamera(&camera);
    Reject([&]{pip->Update(0);});Reject([&]{pip->Rectangle();});pip.reset();session.cameras.Release();
    Check(cCameraManager::PeekCamera()==&camera,"Old PIP/camera owner mutated the new session");
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            Rectangles();PlaybackAndSwap();Validation();Callbacks(false);Callbacks(true);Lifetimes();
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8&&VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"PIP camera ownership leaked game arenas");
        }
        ResetStartupMemory();std::cout<<checks<<" original NIS PIP timing, rectangle, swap and ownership checks passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}
