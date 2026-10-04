#include "runtime/nis_playback.h"
#include "runtime/startup.h"
#include "runtime/graphics_memory.h"
#include "runtime/tasks.h"
#include "resources/chunk_reader.h"
#include "Game/NisPlaybackTiming.h"
#include "Game/Camera/CameraMan.h"
#include "NL/nlTask.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <thread>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool good,const char* message){++checks;if(!good)throw std::runtime_error(message);}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.0001f,"NIS timing differs");}
void Bits(float a,float b){Check(std::bit_cast<unsigned>(a)==std::bit_cast<unsigned>(b),"Original NIS arithmetic bits differ");}
template<class F>void Reject(F action)
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid NIS scheduling operation accepted");}
using Blob = std::vector<std::uint8_t>;
void Word(Blob& data, std::uint32_t word) { for (int n : {24,16,8,0}) data.push_back(word>>n); }
void Set(Blob& data, std::size_t at, std::uint32_t word) { for (int n : {24,16,8,0}) data.at(at++)=word>>n; }
void CameraChunk(Blob& data, unsigned count, float base, float target=0)
{
    const auto begin=data.size(); Word(data,0x8002500b);Word(data,0);
    std::map<unsigned,Blob> channels;
    channels[0x25000]={'n','i','s',0};Word(channels[0x2500c],count);
    for(unsigned i=0;i<count;++i)
    {
        for(float v:{base+i*.01f,2.f,3.f})Word(channels[0x25003],std::bit_cast<unsigned>(v));
        for(float v:{target,0.f,0.f})Word(channels[0x25006],std::bit_cast<unsigned>(v));
        for(float v:{0.f,0.f,0.f,1.f})Word(channels[0x25004],std::bit_cast<unsigned>(v));
        Word(channels[0x25009],std::bit_cast<unsigned>(45.f+i));Word(channels[0x2500a],std::bit_cast<unsigned>(4.f+i));
    }
    for(const auto& [id,payload]:channels)
    {
        Word(data,id|0x05000000);const auto size=data.size();Word(data,0);data.resize(resources::Align(data.size(),32));
        data.insert(data.end(),payload.begin(),payload.end());Set(data,size,data.size()-size-4);
    }
    Set(data,begin+4,data.size()-begin-8);
}
Blob Fixture(float target=0)
{
    Blob data;CameraChunk(data,30,10,target);CameraChunk(data,60,20,target);return data;
}
NisPlaybackTrigger Trigger(float frame,unsigned type=2)
{
    NisPlaybackTrigger result;result.type=type;result.frame=frame;result.name="event";result.target="target";
    result.value=.25f;result.params={0,1,0x80000000,0xffffffff};return result;
}
struct Session
{
    OriginalCameras core;
    NisCameraAssets assets{Fixture(),"timing"};
    NisCameraBinding a{assets,0},b{assets,1};
    NisCameras cameras{core};
    NisPlayback playback{cameras};
    Session(){cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Activate();}
};
void Arithmetic()
{
    for(float real:{0.f,.0001f,.25f,.5f,1.f,4.f})
        for(float dilation:{0.f,.1f,.5f,1.f,4.f})
            for(float carry:{0.f,.001f,.25f,.5f,2.f})
            {
                float originalCarry=carry;
                const float product=float(double(real)*dilation);
                const float expected=float(double(std::min(product,.5f))+carry);
                Bits(NisPlaybackTiming::FrameDelta(real,dilation,originalCarry),expected);Bits(originalCarry,0);
            }
    for(float overrun:{-2.f,-0.f,0.f,.1f,1.f})
    {
        float carry=.25f;NisPlaybackTiming::KeepPrimaryOverrun(overrun,carry);Bits(carry,overrun>0?overrun:.25f);
    }
    for(float duration:{0.f,2.f/30,1.f,2.f,65536.f/30})
        for(float frame:{-1.f,0.f,.5f,1.f,30.f,60.f,std::numeric_limits<float>::max()})
            for(auto [oldTime,newTime]:{std::pair{0.f,0.f},std::pair{0.f,.5f},std::pair{.5f,1.f},std::pair{1.f,.5f}})
            {
                const float boundary=duration==0?0:float(double(float(double(frame)/30.))/duration);
                Check(NisPlaybackTiming::Crossed(oldTime,newTime,duration,frame)==
                    (duration!=0&&oldTime<=boundary&&newTime>boundary),"Trigger crossing differs from independently rounded division");
            }
}
void Clock()
{
    Session s;
    Near(s.cameras.Duration(0),1);Near(s.cameras.Duration(1),2);
    s.cameras.Seek(0,.9f);s.cameras.Seek(1,.99f);
    auto frame=s.playback.Advance({2,2,0x10});Near(frame.delta,.5f);Near(frame.primary_overrun,.4f);Near(s.playback.Carry(),.4f);
    Near(frame.old_time[0],.9f);Near(frame.new_time[0],1);Check(s.playback.WorldIsFrozen(0x10),"Original end predicate did not freeze");
    Check(!s.playback.WorldIsFrozen(0x20),"Inactive task state reported frozen NIS");
    // Secondary overruns do not replace primary carry; clamp precedes carry.
    s.cameras.Select(0,s.a,0);s.cameras.Select(1,s.b,1);
    frame=s.playback.Advance({4,1,0x10});Near(frame.delta,.9f);Near(frame.new_time[0],.9f);Near(s.playback.Carry(),0);
    frame=s.playback.Advance({.5f,1,0x10});Near(s.playback.Carry(),.4f);
    const float before=s.cameras.Time(0);
    frame=s.playback.Advance({2,2,0x20});Near(frame.delta,.9f);Near(s.playback.Carry(),0);Near(s.cameras.Time(0),before);
    Check(!frame.active&&frame.dispatched==0,"Inactive NIS dispatched work");
    s.playback.Reset();Near(s.playback.Carry(),0);Near(s.playback.LastStep().delta,0);Near(s.cameras.Time(0),before);
    s.cameras.Seek(0,0);s.cameras.Seek(1,.99f);frame=s.playback.Advance({.1f,1,0x10});
    Near(frame.primary_overrun,0);Near(s.playback.Carry(),0);Near(s.cameras.Time(1),1);
    s.cameras.Seek(0,0);frame=s.playback.Advance({.04f,.25f,0x10});Near(frame.delta,.01f);Near(s.cameras.Time(0),.01f);
}
void BoundariesAndOrder()
{
    Session s;std::vector<std::pair<std::size_t,float>> events;
    s.playback.BindService(2,[&](const NisTriggerDispatch& event){
        events.emplace_back(event.table,event.trigger.frame);
        Check(event.trigger.name=="event"&&event.trigger.target=="target"&&event.trigger.params[2]==0x80000000,
            "Owned trigger data changed");
    });
    NisPlaybackTable first{0,{Trigger(0),Trigger(15),Trigger(15),Trigger(30),Trigger(-1)}};
    s.playback.AddTable(first);first.triggers[0].name="changed";
    s.playback.AddTable({1,{Trigger(0),Trigger(15)}});s.playback.AddTable({2,{Trigger(0)}});
    Check(s.playback.Advance({0,1,0x10}).dispatched==0,"Zero advance fired frame-zero triggers");
    auto frame=s.playback.Advance({.5f,1,0x10});
    Check(events==std::vector<std::pair<std::size_t,float>>{{0,0},{1,0},{2,0}}&&frame.dispatched==3,
        "Trigger ordering or exact-boundary condition changed");
    frame=s.playback.Advance({.001f,1,0x10});
    Check(events.size()==6&&events[3]==std::pair<std::size_t,float>{0,15}&&events[4]==events[3]&&events[5].first==1,
        "Duplicate/insertion-order or shared-secondary triggers changed");
    s.playback.Advance({.5f,1,0x10});Check(events.size()==6,"Exact endpoint or negative trigger fired");
    Check(s.playback.LastStep().dispatched==0,"Recorded dispatch count did not match frame");
    s.playback.Reset();events.clear();s.cameras.Seek(0,0);s.cameras.Seek(1,0);
    s.playback.AddTable({0,{Trigger(3),Trigger(0),Trigger(1)}});
    s.playback.Advance({.2f,1,0x10});
    Check(events==std::vector<std::pair<std::size_t,float>>{{0,3},{0,0},{0,1}},"Triggers were incorrectly sorted by time");
    s.cameras.Seek(0,0);s.playback.Advance({.2f,1,0x10});Check(events.size()==6,"Rewind-and-recross incorrectly suppressed original trigger repetition");
}
void Swaps()
{
    Session s;std::vector<std::pair<std::size_t,unsigned>> events;
    s.playback.BindService(2,[&](const NisTriggerDispatch& event){events.emplace_back(event.table,event.render_mode);});
    for(unsigned mode:{0u,1u,2u})s.playback.AddTable({mode,{Trigger(20)}});
    s.cameras.Seek(0,.5f);s.cameras.Seek(1,.5f);s.playback.Swap();s.playback.Advance({.4f,1,0x10});
    Check(events==std::vector<std::pair<std::size_t,unsigned>>{{0,1},{2,2}},"Mode swap or mode2 secondary routing differs");
    s.playback.Reset();events.clear();s.cameras.Seek(0,0);s.cameras.Seek(1,0);
    for(unsigned i=0;i<8;++i)s.playback.AddTable({i%3,{Trigger(0)}});
    s.playback.Swap();s.playback.Advance({.01f,1,0x10});Check(events.size()==8,"Not all eight original source slots dispatched");
    for(unsigned i=0;i<8;++i)Check(events[i]==std::pair<std::size_t,unsigned>{i,i%3==2?2:1-i%3},"All-source render-mode swap differs");
}
void Validation()
{
    Session s;
    for(float value:{-1.f,INFINITY,NAN})
    {
        Reject([&]{s.playback.Advance({value,1,0x10});});Reject([&]{s.playback.Advance({1,value,0x10});});
    }
    Reject([&]{s.playback.Advance({std::numeric_limits<float>::max(),2,0x10});});
    Reject([&]{s.playback.AddTable({3,{}});});Reject([&]{s.playback.AddTable({0,std::vector<NisPlaybackTrigger>(49)});});
    Reject([&]{s.playback.AddTable({0,{Trigger(0,11)}});});Reject([&]{s.playback.AddTable({0,{Trigger(NAN)}});});
    auto bad=Trigger(0);bad.value=INFINITY;Reject([&]{s.playback.AddTable({0,{bad}});});
    bad=Trigger(0);bad.name=std::string("bad\0name",8);Reject([&]{s.playback.AddTable({0,{bad}});});
    Reject([&]{s.playback.BindService(11,[](const auto&){});});Reject([&]{s.playback.BindService(0,{});});
    for(unsigned i=0;i<8;++i)s.playback.AddTable({0,{}});Reject([&]{s.playback.AddTable({0,{}});});
    bool rejected=false;std::thread other([&]{try{s.playback.Reset();}catch(const std::logic_error&){rejected=true;}});other.join();
    Check(rejected&&!s.playback.Failed(),"Validation poisoned owner or allowed wrong thread");Near(s.cameras.Time(0),0);
    s.playback.Reset();s.playback.AddTable({0,{Trigger(std::numeric_limits<float>::max())}});
    s.playback.Advance({.1f,1,0x10}); // Finite far-future frame is valid and unconsumed.
}
void Services()
{
    Session s;unsigned calls=0;
    s.playback.AddTable({0,{Trigger(0,1)}});
    s.playback.BindService(1,[&](const NisTriggerDispatch& event){
        ++calls;nlTaskManager::SetTimeDilation(event.trigger.value); // Actual selected task service.
        Reject([&]{s.playback.Reset();});Reject([&]{s.playback.Advance({});});Reject([&]{s.playback.Swap();});
        Reject([&]{s.playback.AddTable({0,{}});});Reject([&]{s.playback.BindService(1,[](const auto&){});});
        Reject([&]{s.cameras.Swap();});Reject([&]{s.cameras.Release();});Reject([&]{s.a.Release();});Reject([&]{s.core.Release();});
    });
    auto frame=s.playback.Advance({.01f,1,0x10});
    Check(calls==1&&!s.playback.Failed(),"Real service or caught mutation guard failed");Near(frame.delta,.01f);
    Near(nlTaskManager::m_pInstance->mTimeDilation,.25f);
    frame=s.playback.Advance({.04f,nlTaskManager::m_pInstance->mTimeDilation,0x10});Near(frame.delta,.01f);
    nlTaskManager::SetTimeDilation(1);
}
void Failures()
{
    {
        Session s;s.core.Advance(0,0);const auto published=cCameraManager::m_matView;
        s.playback.AddTable({0,{Trigger(0)}});Reject([&]{s.playback.Advance({.01f,1,0x10});});
        Check(s.playback.Failed()&&std::memcmp(&published,&cCameraManager::m_matView,sizeof(published))==0,
            "Unavailable trigger service silently succeeded or published a manager pose");
        Reject([&]{s.playback.Advance({});});Reject([&]{s.playback.Swap();});
        const float advanced=s.cameras.Time(0);s.playback.Reset();Near(s.cameras.Time(0),advanced);
        Check(!s.playback.Failed()&&s.playback.LastStep().dispatched==0,"Timeline reset retained failure/dispatch state");
        unsigned count=0;s.playback.BindService(2,[&](const auto&){++count;});s.playback.AddTable({0,{Trigger(0)}});
        s.cameras.Seek(0,0);s.playback.Advance({.01f,1,0x10});Check(count==1,"Explicit reset/reselection recovery failed");
    }
    {
        Session s;s.playback.AddTable({0,{Trigger(0)}});
        s.playback.BindService(2,[](const auto&){throw std::runtime_error("injected service failure");});
        Reject([&]{s.playback.Advance({.01f,1,0x10});});Check(s.playback.Failed(),"Escaping service failure did not poison timeline");
        Reject([&]{s.playback.Reset();});s.cameras.Release();
    }
    {
        Session s;s.playback.AddTable({1,{Trigger(0)}});s.b.Release();
        Check(s.playback.Advance({.1f,1,0x10}).dispatched==0,"Zero-duration absent slot dispatched a trigger");
        s.a.Release();Check(s.playback.WorldIsFrozen(0x10),"Original camera-free time-left predicate changed");
    }
}
void Owned(const char* path)
{
    std::ifstream file(path,std::ios::binary);Check(bool(file),"Cannot read owned NIS");Blob bytes{std::istreambuf_iterator<char>(file),{}};
    OriginalCameras core;NisCameraAssets assets(bytes,"owned");Check(assets.Layout().cameras.size()>=2,"Owned clock needs two tracks");
    NisCameraBinding a(assets,0),b(assets,1);NisCameras cameras(core);cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Activate();
    NisPlayback playback(cameras);std::array<unsigned,2> fired{};
    playback.BindService(2,[&](const NisTriggerDispatch& event){++fired[event.table];});
    const float duration0=cameras.Duration(0),duration1=cameras.Duration(1);
    const float frames0=assets.Camera(0)->Data().m_uKeyCount,frames1=assets.Camera(1)->Data().m_uKeyCount;
    playback.AddTable({0,{Trigger(0),Trigger(frames0/2),Trigger(frames0)}});
    playback.AddTable({1,{Trigger(0),Trigger(frames1/2),Trigger(frames1)}});
    bytes.clear();bytes.shrink_to_fit();
    const unsigned frames=unsigned(std::ceil(std::max(duration0,duration1)*60))+2;
    for(unsigned i=0;i<frames;++i){playback.Advance({1.f/60,1,0x10});core.Advance(0,0);CheckCameraPose(*cameras.Camera(0));}
    Check(fired[0]==2&&fired[1]==2,"Owned camera-clock trigger boundary differs");
    Check(playback.WorldIsFrozen(0x10),"Owned primary did not reach original endpoint");
    playback.Reset();Near(playback.Carry(),0);Check(playback.LastStep().dispatched==0,"Owned reset retained dispatch state");
}
}
int main(int argc,char** argv)
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        {
            struct Tasks{Tasks(){nlTaskManager::Startup(4);}~Tasks(){ShutdownNativeTaskManager();}}tasks;
            if(argc==2)Owned(argv[1]);else for(unsigned i=0;i<3;++i){Arithmetic();Clock();BoundariesAndOrder();Swaps();Validation();Services();Failures();}
        }
        Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8&&VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"NIS scheduling leaked game arenas");
        ResetStartupMemory();std::cout<<checks<<" NIS scheduling checks passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}
