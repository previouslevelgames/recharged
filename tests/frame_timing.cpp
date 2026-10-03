#include "runtime/frame_timing.h"
#include "runtime/startup.h"
#include "runtime/tasks.h"
#include "Game/Debug/TimeRegions.h"
#include "NL/MemAlloc.h"
#include "NL/nlTask.h"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace { std::uint32_t tick=0; bool enabled=true; }
extern "C" std::uint32_t ChargedFixtureGetTick() { return tick; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
namespace
{
unsigned checks=0;
void Check(bool c,const char* m) { ++checks; if(!c) throw std::runtime_error(m); }
bool Close(float a,float b) { return std::abs(a-b)<0.0001f; }
template<class F> void Reject(F f)
{
    try { f(); } catch(const std::exception&) { ++checks;return; }
    throw std::runtime_error("Invalid timing operation accepted");
}
struct Task: nlTask
{
    std::function<void()> run;
    void Run(float) override { run(); }
    const char* GetName() override { return "Frame timing fixture"; }
};
void Run()
{
    FrameCounter counter("frame","send");
    auto initial=mscharged::ReadFrameTiming(counter);
    Check(initial.average_ms[0]==0 && initial.average_ms[1]==0 && initial.pending_frames==0,
          "Scoped timing fields were not initialized");
    TimeRegion region("fixture",[] { return enabled; });
    nlTaskManager::Startup(1);
    Task begin,work,end;
    begin.run=[&] { counter.StartTimer(0); };
    work.run=[] { tick+=60750; };
    end.run=[&] { counter.StartTimer(1);tick+=121500;counter.FinishTiming(); };
    nlTaskManager::AddTask(&begin,0,1);nlTaskManager::AddTask(&work,1,1);nlTaskManager::AddTask(&end,2,1);
    tick=0xffff0000u;
    for(unsigned i=0;i<650;++i)
    {
        enabled=(i%2)==0;
        nlTaskManager::RunAllTasks();
        auto s=mscharged::ReadFrameTiming(counter);
        Check(Close(s.last_ms[0],1) && Close(s.last_ms[1],2),"Original ticker rollover or phase accumulation changed");
    }
    mscharged::ShutdownNativeTaskManager();
    auto s=mscharged::ReadFrameTiming(counter);
    Check(Close(s.average_ms[0],1) && Close(s.average_ms[1],2) && s.pending_frames==20,"Original averaging window changed");
    Check(s.next_history==10 && s.next_continuous==50,"Original history wrap changed");
    Check(region.m_unk10==325 && region.m_Histogram.m_NumSamples==325 && Close(region.m_fThreshold,975),"Conditional time region sampling changed");
    Reject([&] { counter.StartTimer(-1); });Reject([&] { counter.StartTimer(2); });
    FrameCounter::NUM_FRAMES_TO_AVERAGE_OVER=0;
    Reject([&] { counter.FinishTiming(); });FrameCounter::NUM_FRAMES_TO_AVERAGE_OVER=30;
    Histogram bins("bounds",3,0,1);
    for(float v:{-1.f,0.f,.5f,1.f,100.f}) bins.AddSample(v);
    Check(bins.GetCumulativePercentage(0)==20 && bins.GetCumulativePercentage(1)==60 && bins.GetCumulativePercentage(2)==100,
          "Original histogram boundaries changed");
    Reject([&] { bins.GetCumulativePercentage(3); });
    Reject([&] { bins.AddSample(std::numeric_limits<float>::quiet_NaN()); });
    Reject([] { Histogram invalid("invalid",2,0,1); });
    Reject([] { TimeRegion invalid("invalid",nullptr); });
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        mscharged::ResetStartupMemory();StandardAllocator.Initialize(mem1.data(),mem1.size()*8);
        VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        for(int i=0;i<3;++i)
        {
            Run();
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,
                  "Original timing/history leaked game arenas");
        }
        mscharged::ResetStartupMemory();
        std::cout<<checks<<" original timing/history/scheduler checks and three arena recoveries passed\n";
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
