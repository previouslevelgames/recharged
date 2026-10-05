// Compile the production movie unit here to exercise its private active flag
// without exposing a production switch that could claim a movie was loaded.
#include "Game/Sys/movie.cpp"
#include "runtime/tasks.h"
#include "NL/nlTask.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
namespace
{
template<class Action> void Stopped(const char* symbol, Action action)
{
    try { action(); }
    catch (const mscharged::StartupStopped& error)
    {
        if (std::string(error.what()).find(symbol) == 0) return;
        throw;
    }
    throw std::runtime_error("Unavailable movie service reported success");
}
struct Task : nlTask
{
    int calls = 0;
    void Run(float) override { ++calls; }
    const char* GetName() override { return "Movie boundary fixture"; }
};
void ScheduledMovie()
{
    alignas(64) static std::array<std::byte,4096> arena{};
    StandardAllocator.Initialize(arena.data(),arena.size());
    CurrentAllocator=&StandardAllocator; gMemoryInitialized=1;
    Task inactive,first,last;
    nlTaskManager::Startup(1);
    nlTaskManager::AddTask(&inactive,0,2);
    nlTaskManager::AddTask(&first,1,1);
    nlTaskManager::AddTask(&last,2,1);
    g_bActive=true;
    Stopped("MoviePlay", [] { nlTaskManager::RunAllTasks(); });
    if (inactive.calls || first.calls!=1 || last.calls)
        throw std::runtime_error("Movie service must be called immediately after each active task");
    g_bActive=false;
    bool rejected=false;
    try { nlTaskManager::RunAllTasks(); } catch (const std::logic_error&) { rejected=true; }
    if (!rejected) throw std::runtime_error("Frame resumed after a partially delivered movie failure");
    mscharged::ShutdownNativeTaskManager();
    if (StandardAllocator.TotalFreeMemory()!=arena.size())
        throw std::runtime_error("Movie service failure leaked scheduler allocations");
    gMemoryInitialized=0; CurrentAllocator=nullptr; StandardAllocator={};
}
}
int main()
{
    try
    {
        for (int round = 0; round < 3; ++round)
        {
            if (IsMovieActive() || MoviePlay() || MovieStop() || IsMovieFinished() || GetMovieFrame())
                throw std::runtime_error("Original inactive movie contract changed");
            Stopped("MovieInit", [] { MovieInit(); });
            Stopped("MovieQuit", [] { MovieQuit(); });
            Stopped("MovieStart", [] { MovieStart("intro.thp", true, false, false); });
            if (IsMovieActive()) throw std::runtime_error("Rejected MovieStart published active state");
            g_bActive = true; // Test-only state injection into the compiled original unit.
            Stopped("MoviePlay", [] { MoviePlay(); });
            Stopped("MovieStop", [] { MovieStop(); });
            if (!IsMovieActive()) throw std::runtime_error("Missing movie cleanup falsely cleared ownership");
            g_bActive = false;
        }
        ScheduledMovie();
        std::cout << "Original inactive movie queries and explicit init/start/active play/stop boundaries passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
