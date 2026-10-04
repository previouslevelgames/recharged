#include "runtime/graphics_startup.h"
#include "runtime/graphics_state.h"
#include "runtime/frames.h"
#include "runtime/materials.h"
#include "runtime/views.h"
#include "runtime/tasks.h"
#include "Game/GraphicsMemoryStartup.h"
#include "Game/Task/GraphicsTaskPriorities.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glStartupStages.h"
#include "NL/nlMemory.h"
#include "NL/nlTask.h"
#include <array>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mscharged
{
namespace { GraphicsStartup* active_startup = nullptr; }
struct GraphicsStartup::Impl
{
    const std::thread::id thread = std::this_thread::get_id();
    std::unique_ptr<MaterialPrograms> materials;
    std::unique_ptr<OriginalViews> views;
    GraphicsFrameTasks* tasks = nullptr;
    bool busy = false;
};
void GraphicsStartup::Ready() const
{
    if (!impl_ || active_startup != this || impl_->thread != std::this_thread::get_id())
        throw std::logic_error("Graphics startup requires its live owner thread");
}
GraphicsStartup::GraphicsStartup(unsigned width, unsigned height,
    const std::function<void()>& configure_video, void (*drain)())
{
    if (!configure_video || !drain || !width || !height || width > 1024 || height > 1024)
        throw std::invalid_argument("Graphics startup requires valid dimensions and video/drain services");
    if (active_startup || !gMemoryInitialized || glGetResourcePools() || OriginalViewsReady())
        throw std::logic_error("Graphics startup requires exclusive initialized game arenas");
    impl_ = std::make_unique<Impl>();
    active_startup = this;
    impl_->busy = true;
    try
    {
        ResetOriginalFrameState();
        gl_StartupMemory(InitializeOriginalGraphicsMemory);
        impl_->materials = std::make_unique<MaterialPrograms>();
        configure_video();
        InitializeOriginalGraphicsStartupState();
        impl_->views = std::make_unique<OriginalViews>(width, height, drain);
        impl_->busy = false;
    }
    catch (...)
    {
        impl_->views.reset();
        impl_->materials.reset();
        glShutdownMemory();
        active_startup = nullptr;
        throw;
    }
}
GraphicsStartup::~GraphicsStartup() { Release(); }
void GraphicsStartup::Release()
{
    if (!impl_) return;
    Ready();
    if (impl_->busy || impl_->tasks)
        throw std::logic_error("Release graphics frame tasks before graphics startup");
    // Frames/layers register real teardown hooks with OriginalViews.
    impl_->views->Release();
    impl_->materials->Release();
    glShutdownMemory();
    impl_.reset();
    active_startup = nullptr;
}

struct GraphicsFrameTasks::Impl
{
    struct Task final : nlTask
    {
        std::function<void(float)> action;
        const char* name = nullptr;
        void Run(float delta) override { action(delta); }
        const char* GetName() override { return name; }
    };
    GraphicsStartup& startup;
    OriginalFrames& frames;
    GraphicsFrameCallbacks callbacks;
    std::array<Task,4> tasks;
    nlTaskManager* manager = nullptr;
    const std::thread::id thread = std::this_thread::get_id();
    bool running = false, failed = false;
    Impl(GraphicsStartup& graphics, OriginalFrames& lifecycle, GraphicsFrameCallbacks functions)
        : startup(graphics), frames(lifecycle), callbacks(std::move(functions)) {}
    bool Registered() const
    {
        for (const auto& task : tasks) if (task.mNativeRegistered) return true;
        return false;
    }
    void Ready() const
    {
        if (thread != std::this_thread::get_id())
            throw std::logic_error("Graphics frame tasks require their owner thread");
        startup.Ready();
        if (running) throw std::logic_error("Cannot mutate or recursively run graphics frame callbacks");
    }
    void CheckManager() const
    {
        if (nlTaskManager::m_pInstance != manager || !Registered())
            throw std::logic_error("Original graphics scheduler was externally released");
        // This bounded owner cannot accept hidden extra tasks or state changes.
        if (manager->mCurrentState != 1 || manager->mPendingState != 1 || manager->mTaskList != &tasks[3])
            throw std::logic_error("Selected graphics scheduler ownership or state changed");
        constexpr unsigned priorities[] = {GraphicsTaskPriorities::Begin, GraphicsTaskPriorities::Update,
            GraphicsTaskPriorities::Render, GraphicsTaskPriorities::End};
        for (unsigned i=0; i<tasks.size(); ++i)
            if (!tasks[i].mNativeRegistered || !tasks[i].mTimeDilated || tasks[i].m_next != &tasks[(i+1)%4]
                || tasks[i].m_prev != &tasks[(i+3)%4] || tasks[i].mPriority != priorities[i]
                || tasks[i].mActiveStates != (i==1 || i==2 ? GraphicsTaskPriorities::WorldStates : ~0u))
                throw std::logic_error("Selected graphics scheduler task ring changed");
    }
};
GraphicsFrameTasks::GraphicsFrameTasks(GraphicsStartup& startup, OriginalFrames& frames,
    GraphicsFrameCallbacks callbacks)
{
    startup.Ready();
    if (!callbacks.begin || !callbacks.update || !callbacks.render || !callbacks.end)
        throw std::invalid_argument("Every selected graphics frame callback requires an implementation");
    if (startup.impl_->busy || startup.impl_->tasks || nlTaskManager::m_pInstance)
        throw std::logic_error("Graphics frame tasks require an exclusive idle scheduler");
    impl_ = std::make_unique<Impl>(startup, frames, std::move(callbacks));
    auto& p = *impl_;
    p.tasks[0].name = "Selected graphics begin";
    p.tasks[1].name = "Selected graphics update";
    p.tasks[2].name = "Selected graphics render";
    p.tasks[3].name = "Selected graphics end";
    p.tasks[0].action = [&p](float delta) { glBeginFrame(); p.callbacks.begin(delta); p.CheckManager(); };
    p.tasks[1].action = [&p](float delta) { p.callbacks.update(delta); p.CheckManager(); };
    p.tasks[2].action = [&p](float delta) { p.callbacks.render(delta); p.CheckManager(); };
    p.tasks[3].action = [&p](float delta) { glEndFrame(); p.callbacks.end(delta); p.CheckManager(); glSendFrame(); };
    nlTaskManager::Startup(1);
    p.manager = nlTaskManager::m_pInstance;
    try
    {
        nlTaskManager::AddTask(&p.tasks[0], GraphicsTaskPriorities::Begin, ~0u);
        nlTaskManager::AddTask(&p.tasks[1], GraphicsTaskPriorities::Update, GraphicsTaskPriorities::WorldStates);
        nlTaskManager::AddTask(&p.tasks[2], GraphicsTaskPriorities::Render, GraphicsTaskPriorities::WorldStates);
        nlTaskManager::AddTask(&p.tasks[3], GraphicsTaskPriorities::End, ~0u);
        startup.impl_->tasks = this;
    }
    catch (...) { ShutdownNativeTaskManager(); throw; }
}
GraphicsFrameTasks::~GraphicsFrameTasks() { Release(); }
void GraphicsFrameTasks::RunAcquired()
{
    if (!impl_) throw std::logic_error("Graphics frame tasks are released");
    auto& p = *impl_; p.Ready();
    if (p.failed) throw std::logic_error("Failed graphics frame tasks require teardown");
    p.running = p.startup.impl_->busy = true;
    try { p.CheckManager(); nlTaskManager::RunAllTasks(); }
    catch (...)
    {
        p.running = p.startup.impl_->busy = false;
        p.failed = true;
        p.frames.Cancel();
        throw;
    }
    p.running = p.startup.impl_->busy = false;
}
void GraphicsFrameTasks::Release()
{
    if (!impl_) return;
    auto& p = *impl_; p.Ready();
    p.frames.Cancel();
    // External scheduler shutdown already detaches our borrowed ring nodes;
    // never tear down a later manager even if its allocator reused the address.
    if (p.Registered())
    {
        if (nlTaskManager::m_pInstance != p.manager)
            throw std::logic_error("Registered graphics tasks lost their original manager");
        ShutdownNativeTaskManager();
    }
    p.startup.impl_->tasks = nullptr;
    impl_.reset();
}
}
