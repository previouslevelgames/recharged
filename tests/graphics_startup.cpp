#include "runtime/graphics_startup.h"
#include "runtime/graphics_memory.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/startup.h"
#include "runtime/tasks.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/glx/glxMemory.h"
#include "NL/nlTask.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

static std::uint32_t tick = 60750;
extern "C" std::uint32_t ChargedFixtureGetTick() { return tick; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
extern float g_fTaskTimeLowerBound, g_fTaskTimeUpperBound;
using namespace mscharged;
namespace
{
unsigned checks = 0;
std::vector<char> trace;
void Check(bool yes, const char* what) { ++checks; if (!yes) throw std::runtime_error(what); }
template<class Error=std::logic_error, class Fn> void Reject(Fn fn)
{
    try { fn(); } catch (const Error&) { ++checks; return; }
    throw std::runtime_error("Invalid graphics startup/task operation accepted");
}
void Drain() {}
void Invalidate() { trace.push_back('I'); }
struct Backend : FrameBackend
{
    bool open=false, available=true, fail=false;
    unsigned acquired=0, closed=0, packets=0;
    bool Acquire() override { trace.push_back('A'); if (available) {open=true; ++acquired;} return available; }
    void Render() override
    {
        Check(open && !glIsFrameActive(),"Render before end"); trace.push_back('V');
        if (fail) throw std::runtime_error("Backend render fault");
    }
    void Finish(bool present) override { Check(open,"Double close"); trace.push_back(present?'P':'X'); open=false; ++closed; }
    void Drain() override { trace.push_back('D'); }
    void WaitIdle() override {}
    void Cancel() noexcept override { if(open) {open=false;++closed;} }
};
void Video()
{
    Check(glGetResourcePools() && glGetMaterialProgram(0x21db4385),"Video setup preceded memory/material registration");
    Check(glGetIdentityMatrix()==GL_INVALID_MATRIX && !OriginalViewsReady(),"Video setup followed state/view creation");
    Reject([] { GraphicsStartup duplicate(640,480,Video,Drain); });
}
void RunSession()
{
    GraphicsStartup graphics(640,480,Video,Drain);
    Check(OriginalViewsReady() && glGetIdentityMatrix()!=GL_INVALID_MATRIX,"Qualified startup missing views/matrices");
    const auto* bundle=gl_GetCurrentStateBundle();
    Check(bundle->matrix==GL_INVALID_MATRIX && bundle->texconfig==0,"Original startup altered current matrix or LP64 texture sentinel");
    Check(glGetCurrentRasterState()==glHandleizeRasterState() && glGetCurrentTextureState()==glHandleizeTextureState(),"Original default handles not published");
    for(unsigned i=0;i<GLTT_Num;++i) Check(bundle->texture[i]==0xffffffffu,"Original texture sentinel changed");
    Reject([] { GraphicsStartup duplicate(640,480,Video,Drain); });
    bool wrong=false;
    std::thread other([&] {try {graphics.Release();}catch(const std::logic_error&){wrong=true;}});other.join();
    Check(wrong,"Wrong-thread graphics teardown accepted");
    Backend backend;
    OriginalFrames frames(backend);
    SetGraphicsCacheInvalidator(Invalidate);
    std::array<float,4> deltas{};
    GraphicsFrameTasks* task_owner=nullptr;
    int fault=-1;
    auto callback=[&](unsigned i,float delta) {
        trace.push_back("BURE"[i]); deltas[i]=delta;
        Check(glIsFrameActive()==(i!=3),"Selected phase executed outside original frame state");
        Reject([&] { task_owner->RunAcquired(); });
        Reject([&] { task_owner->Release(); });
        Reject([&] { graphics.Release(); });
        Reject([] { ShutdownNativeTaskManager(); });
        if (int(i)==fault) throw std::runtime_error("Selected callback fault");
    };
    auto callbacks=[&] { return GraphicsFrameCallbacks{
        [&](float d){callback(0,d);},[&](float d){callback(1,d);},
        [&](float d){callback(2,d);},[&](float d){callback(3,d);}}; };
    Reject<std::invalid_argument>([&] {GraphicsFrameTasks missing(graphics,frames,{});});
    {
        GraphicsFrameTasks tasks(graphics,frames,callbacks());task_owner=&tasks;
        Reject([&] {GraphicsFrameTasks duplicate(graphics,frames,callbacks());});
        Reject([&] {graphics.Release();});
        wrong=false; std::thread other([&] {try {tasks.RunAcquired();}catch(const std::logic_error&){wrong=true;}});other.join();
        Check(wrong,"Wrong-thread scheduler call accepted");
        backend.available=false;Check(!frames.Acquire() && glGetCurrentFrame()==0,"Missing host frame advanced state");backend.available=true;
        trace.clear();tick+=6075000; // exactly 100 ms on original 60.75 MHz time base
        frames.Acquire();tasks.RunAcquired();
        Check(trace==std::vector<char>({'A','B','U','R','E','V','D','I','P','D'}),"Original scheduler priority/submit order changed");
        for(float d:deltas)Check(std::abs(d-.1f)<.000001f,"Original ticker/clamp delta missing");
        Check(glGetCurrentFrame()==1,"Original submitted count missing");
        nlTaskManager::SetTimeDilation(.5f);tick+=12150000;
        frames.Acquire();tasks.RunAcquired();
        for(float d:deltas)Check(std::abs(d-.05f)<.000001f,"Scheduler clamp-before-dilation changed");
        tasks.Release();tasks.Release();
        Check(!nlTaskManager::m_pInstance,"Scheduler teardown left manager live");
        Reject([&] {tasks.RunAcquired();});
    }
    for (fault=0;fault<5;++fault)
    {
        GraphicsFrameTasks tasks(graphics,frames,callbacks());task_owner=&tasks;
        backend.fail=fault==4;
        const auto before=glGetCurrentFrame();const auto generation=glNativeFrameGeneration();
        frames.Acquire();Reject<std::runtime_error>([&] {tasks.RunAcquired();});
        Check(!backend.open && !glIsFrameActive() && glGetCurrentFrame()==before,"Failure published/leaked an incomplete frame");
        Check(glNativeFrameGeneration()==generation+1,"Failure did not retire frame memory exactly once");
        Reject([&] {tasks.RunAcquired();});
        tasks.Release();backend.fail=false;
    }
    fault=-1;
    for (int timer_fault=0;timer_fault<3;++timer_fault)
    {
        GraphicsFrameTasks tasks(graphics,frames,callbacks());task_owner=&tasks;
        if(timer_fault==0) g_fTaskTimeLowerBound=.2f;
        if(timer_fault==1) g_fTaskTimeUpperBound=std::numeric_limits<float>::infinity();
        if(timer_fault==2) nlTaskManager::m_pInstance->mTimeDilation=-1;
        frames.Acquire();Reject<std::invalid_argument>([&] {tasks.RunAcquired();});
        Check(!backend.open && !glIsFrameActive(),"Invalid scheduler clock retained an acquired frame");
        g_fTaskTimeLowerBound=0;g_fTaskTimeUpperBound=.1f;tasks.Release();
    }
    {
        auto mutate=callbacks();mutate.update=[](float){nlTaskManager::SetNextState(2);};
        GraphicsFrameTasks tasks(graphics,frames,std::move(mutate));task_owner=&tasks;
        const auto before=glGetCurrentFrame();frames.Acquire();
        Reject([&] {tasks.RunAcquired();});
        Check(!backend.open && glGetCurrentFrame()==before,"Callback state mutation submitted a partial frame");tasks.Release();
    }
    {
        GraphicsFrameTasks tasks(graphics,frames,callbacks());task_owner=&tasks;
        // The old owner must not destroy an unrelated later manager at a reused address.
        ShutdownNativeTaskManager();nlTaskManager::Startup(1);auto* later=nlTaskManager::m_pInstance;
        Reject([&] {tasks.RunAcquired();});tasks.Release();
        Check(nlTaskManager::m_pInstance==later,"Old owner destroyed later scheduler");ShutdownNativeTaskManager();
    }
    {
        GraphicsFrameTasks tasks(graphics,frames,callbacks());task_owner=&tasks;
        nlTaskManager::SetNextState(2);
        frames.Acquire();Reject([&] {tasks.RunAcquired();});
        Check(!backend.open,"Mutated scheduler state retained an acquired frame");tasks.Release();
    }
    {
        GraphicsFrameTasks tasks(graphics,frames,callbacks());task_owner=&tasks;
        nlTaskManager::m_pInstance->mTaskList->mTimeDilated=false;
        frames.Acquire();Reject([&] {tasks.RunAcquired();});
        Check(!backend.open,"Mutated task clock retained an acquired frame");tasks.Release();
    }
    frames.Acquire();glBeginFrame(); // owner teardown cancels even a partial frame
    graphics.Release();frames.Release();graphics.Release();
    Check(!backend.open && backend.closed==backend.acquired,"Graphics owner leaked/double-closed acquired frame");
    Check(!OriginalViewsReady() && !glGetResourcePools() && !glGetMaterialProgram(0x21db4385),"Graphics teardown retained views/resources/programs");
}
}
int main()
{
    try
    {
        Reject([] {GraphicsStartup no_memory(640,480,Video,Drain);});
        std::vector<std::uint64_t> mem1(2*1024*1024),mem2(4*1024*1024);
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        auto recovered=[&] {Check(StandardAllocator.TotalFreeMemory()==free1 && VirtualAllocator.TotalFreeMemory()==free2,"Graphics transaction leaked game arenas");};
        Reject<std::invalid_argument>([] {GraphicsStartup bad(0,480,Video,Drain);});
        Reject<std::invalid_argument>([] {GraphicsStartup bad(640,480,{},Drain);});
        Reject<std::invalid_argument>([] {GraphicsStartup bad(640,480,Video,nullptr);});recovered();
        for(int i=0;i<3;++i)
        {
            Reject<std::runtime_error>([] {GraphicsStartup failed(640,480,[]{Video();throw std::runtime_error("Video setup fault");},Drain);});recovered();
            RunSession();recovered();
        }
        ResetStartupMemory();
        // Original PreInitFS allocation failure rolls back before any video call.
        StandardAllocator.Initialize(mem1.data(),1024*1024);VirtualAllocator.Initialize(mem2.data(),1024*1024);gMemoryInitialized=1;
        const auto small1=StandardAllocator.TotalFreeMemory(),small2=VirtualAllocator.TotalFreeMemory();
        bool video_called=false;
        Reject<std::exception>([&] {GraphicsStartup no_space(640,480,[&]{video_called=true;},Drain);});
        Check(!video_called,"Original memory failure reached video setup");
        Check(StandardAllocator.TotalFreeMemory()==small1 && VirtualAllocator.TotalFreeMemory()==small2 && !glGetResourcePools(),"Original memory failure leaked");
        ResetStartupMemory();
        std::cout<<checks<<" original startup/scheduler/rollback/lifetime checks and repeated arena recoveries passed\n";
        return 0;
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
