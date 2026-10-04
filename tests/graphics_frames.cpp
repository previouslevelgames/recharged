#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/tasks.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxMemory.h"
#include "NL/nlTask.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks = 0, packets = 0;
std::vector<char> trace;
void Check(bool okay, const char* message) { ++checks; if (!okay) throw std::runtime_error(message); }
template<class Error = std::logic_error, class Action> void Reject(Action action)
{
    try { action(); } catch (const Error&) { ++checks; return; }
    throw std::runtime_error("Invalid graphics frame operation was accepted");
}
void Invalidate() { trace.push_back('I'); }
void Observe(GLView*, unsigned long, const glModelPacket* packet)
{
    if (packet)
    {
        // Actual view dispatch selects packet matrices. Merely counting the
        // packets missed stale current pointers after their frame was retired.
        glSetCurrentMatrix(packet->matrix);
        ++packets;
    }
}
void CheckIdleMatrix()
{
    Check(glGetCurrentMatrix() == glGetIdentityMatrix(), "Retired frame left a current borrowed matrix");
    glStateBundle snapshot; glStateSave(snapshot); glStateRestore(snapshot);
    nlMatrix4 matrix; glGetMatrix(glGetCurrentMatrix(), matrix);
    Check(matrix.e[0] == 1 && matrix.e[5] == 1 && matrix.e[10] == 1 && matrix.e[15] == 1,
        "Idle graphics state cannot restore its permanent identity matrix");
}
struct Backend : FrameBackend
{
    bool available = true, open = false, fail_render = false, fail_finish = false;
    unsigned acquisitions = 0, closes = 0;
    std::function<void()> callback;
    bool Acquire() override
    {
        trace.push_back('A');
        if (available) { open = true; ++acquisitions; }
        return available;
    }
    void Render() override
    {
        Check(open && !glIsFrameActive(), "Render did not follow original glEndFrame");
        trace.push_back('R');
        if (callback) callback();
        for (GLViewIterator i(&gRootView); !i.IsDone(); i.Next()) i.Current()->Iterate(Observe);
        if (fail_render) throw std::runtime_error("fixture render failure");
    }
    void Finish(bool present) override
    {
        Check(open, "Host frame closed twice");
        trace.push_back(present ? 'P' : 'X');
        open = false; ++closes;
        if (fail_finish) throw std::runtime_error("fixture failure after host close");
    }
    void Drain() override { trace.push_back('D'); }
    void WaitIdle() override { Check(!open,"GPU wait accepted an unfinished host frame"); trace.push_back('W'); }
    void Cancel() noexcept override
    {
        trace.push_back('C');
        if (open) { open = false; ++closes; }
    }
};
struct Task : nlTask
{
    std::function<void()> action;
    void Run(float) override { action(); }
    const char* GetName() override { return "Graphics lifecycle fixture"; }
};
void Run()
{
    Backend backend;
    Reject([&] { OriginalFrames premature(backend); });
    ViewMatrices matrices;
    MaterialPrograms materials;
    OriginalViews views(640,480);
    OriginalFrames frames(backend);
    auto* view = new (8,false) GLView(&matrices, {}, GLViewSort_None);
    gRootView.AddChild(view);
    Reject([&] { OriginalFrames duplicate(backend); });
    Check(glGetCurrentFrame()==0 && !glIsFrameActive(),"Initial original frame state is undefined");
    Reject([] { glBeginFrame(); }); Reject([] { glEndFrame(); }); Reject([] { glSendFrame(); });
    Reject([] { glplatBeginFrame(); }); Reject([] { glplatSendFrame(); });
    backend.available=false;
    const auto generation=glNativeFrameGeneration();
    Check(!frames.Acquire() && generation==glNativeFrameGeneration(),"Unavailable host frame consumed game frame memory");
    backend.available=true;
    glModelPacket packet{};
    packet.materialProgram=glGetMaterialProgram(0x21db4385); packet.numUniqueVertices=3;
    auto attach=[&] {
        nlMatrix4 identity; identity.SetIdentity(); packet.matrix=glAllocSetMatrix(identity);
        glSetCurrentMatrix(packet.matrix); // Also exercise discarded/cancelled construction.
        view->AttachPacket(&packet,0);
    };
    backend.callback=[&] {
        Reject([] { glBeginFrame(); }); Reject([] { glSendFrame(); }); Reject([] { glplatSendFrame(); });
        Reject([&] { frames.Acquire(); }); Reject([&] { frames.Cancel(); });
        Reject([&] { frames.Release(); }); Reject([&] { views.Release(); });
    };
    trace.clear();
    Check(frames.Acquire(),"Host frame unavailable");
    Reject([&] { frames.Acquire(); });
    glBeginFrame(); Check(glIsFrameActive(),"glBeginFrame did not publish active state");
    Reject([] { glFinish(); });
    Reject([] { glBeginFrame(); }); Reject([] { glSendFrame(); });
    attach(); glEndFrame(); Check(!glIsFrameActive(),"glEndFrame retained active state");
    Reject([] { glEndFrame(); });
    glSendFrame();
    CheckIdleMatrix();
    Reject<std::invalid_argument>([&] { nlMatrix4 retired; glGetMatrix(packet.matrix,retired); });
    Check(trace==std::vector<char>({'A','R','D','I','P','D'}),"Frame submit/drain/advance/present order changed");
    Check(glGetCurrentFrame()==1 && packets==1 && generation+1==glNativeFrameGeneration(),"Original successful frame bookkeeping failed");
    packets=0; view->Iterate(Observe); Check(packets==0,"Original glSendFrame did not reset packets");
    backend.callback={};
    glDiscardFrame(2); glDiscardFrame(1); glDiscardFrame(-1);
    for (int i=0;i<3;++i)
    {
        trace.clear(); frames.Acquire(); glBeginFrame(); attach(); glEndFrame(); glSendFrame();
        CheckIdleMatrix();
        Check((std::find(trace.begin(),trace.end(),'R')!=trace.end())==(i==2),"Discard count/max semantics changed");
        Check((std::find(trace.begin(),trace.end(),'X')!=trace.end())==(i<2),"Discarded frame was presented");
    }
    Check(glGetCurrentFrame()==4 && packets==1,"Discarded frames must increment original counter without rendering");
    {
        const auto before=glNativeFrameGeneration();
        frames.Acquire(); glBeginFrame(); attach(); glEndFrame();
        backend.fail_render=true;
        Reject<std::runtime_error>([] { glSendFrame(); });
        CheckIdleMatrix();
        backend.fail_render=backend.fail_finish=false;
        Check(glGetCurrentFrame()==4 && !glIsFrameActive() && glNativeFrameGeneration()==before+1,
              "Failed frame advanced twice or reported successful submission");
        packets=0; view->Iterate(Observe); Check(packets==0,"Failure retained stale packet pointers");
        frames.Acquire(); glBeginFrame(); glEndFrame(); glSendFrame();
        frames.Release();
    }
    {
        OriginalFrames second(backend);
        frames.Release();
        second.Acquire(); glBeginFrame(); attach(); glEndFrame();
        backend.fail_finish=true; const auto before=glNativeFrameGeneration();
        Reject<std::runtime_error>([] { glSendFrame(); }); backend.fail_finish=false;
        CheckIdleMatrix();
        Check(glGetCurrentFrame()==0 && glNativeFrameGeneration()==before+1 && !backend.open,
              "Post-close failure double-closed or double-advanced a frame");
        second.Acquire(); second.Cancel();
        CheckIdleMatrix();
        second.Acquire(); glBeginFrame(); attach(); second.Cancel();
        CheckIdleMatrix();
        Check(glGetCurrentFrame()==0 && !glIsFrameActive(),"Cancelled construction claimed a successful frame");
        bool wrong=false;
        std::thread worker([&] { try { second.Acquire(); } catch (const std::logic_error&) { wrong=true; } });
        worker.join(); Check(wrong,"Frame owner accepted another thread");
        nlTaskManager::Startup(1);
        Task begin, submit, end;
        begin.action=[] { glBeginFrame(); }; submit.action=attach;
        end.action=[] { glEndFrame(); glSendFrame(); };
        nlTaskManager::AddTask(&begin,0,1); nlTaskManager::AddTask(&submit,1,1); nlTaskManager::AddTask(&end,2,1);
        second.Acquire(); nlTaskManager::RunAllTasks();
        Check(glGetCurrentFrame()==1,"Original scheduler did not execute the complete frame sequence");
        ShutdownNativeTaskManager();
        glFinish();
        second.Acquire(); glBeginFrame(); attach();
        views.Release(); // Active frame cancelled before view/target destruction.
        Check(!backend.open && !glIsFrameActive() && !OriginalViewsReady(),"View shutdown retained an active graphics frame");
        second.Release();
    }
    Check(backend.closes==backend.acquisitions,"A host frame was leaked or closed more than once");
    materials.Release();
}
}
int main()
{
    try
    {
        Reject([] { glBeginFrame(); });
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        ResetStartupMemory();
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8); VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);
        gMemoryInitialized=1;
        for (int i=0;i<3;++i)
        {
            const GLMemoryRequirement requirements[]={{GLM_Header,65536},{GLM_VertexData,4096}};
            const GLMemoryConfig config{65536,4096,requirements,2,32};
            glInitMemory(&config); InitializeOriginalGraphicsState(); SetGraphicsCacheInvalidator(Invalidate);
            packets=0; Run(); glShutdownMemory();
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,
                  "Graphics frame session leaked game arenas");
        }
        ResetStartupMemory();
        std::cout << checks << " graphics frame ordering/discard/failure/scheduler/lifetime checks and three arena recoveries passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
