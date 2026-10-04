#include "runtime/frontend_world_files.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "resources/world_scene.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool result, const char* message) { ++checks; if (!result) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid world read accepted"); }
bool Loading(FrontendWorldFileState state) { return state <= FrontendWorldFileState::Tweaks; }
void Pump(FrontendWorldFileLoad& load, bool external)
{
    auto previous = load.State();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Loading(load.State()))
    {
        if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service();
        const auto state = load.State();
        Check(state == FrontendWorldFileState::Failed || state == previous
            || static_cast<int>(state) == static_cast<int>(previous) + 1, "World load skipped an original resource stage");
        previous = state;
        Check(std::chrono::steady_clock::now() < end, "World read timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!nlAsyncReadsPending(nullptr), "World read retained requests after completion");
}
void Run(std::string_view mode, bool external)
{
    if (mode == "missing-res") { Reject([] { FrontendWorldFileLoad load; }); return; }
    std::shared_ptr<const FrontendWorldFiles> retained;
    {
        FrontendWorldFileLoad load;
        Reject([&] { load.Result(); });
        bool wrong_thread = false;
        std::thread foreign([&] { try { load.Poll(); } catch (const std::logic_error&) { wrong_thread = true; } });
        foreign.join(); Check(wrong_thread, "Foreign thread serviced world files");
        Pump(load, external);
        if (mode != "success" && mode != "partial" && mode != "missing-model" && mode != "owned")
        { Check(load.State() == FrontendWorldFileState::Failed, "Invalid world file succeeded"); Reject([&] { load.Result(); }); return; }
        retained = load.Result();
        if (mode == "missing-model")
        {
            Reject([&] { resources::ReadAvailableWorldScene(retained->resident, retained->temporary); });
            return;
        }
        const auto available = resources::ReadAvailableWorldScene(retained->resident, retained->temporary);
        const auto records = resources::ReadWorldObjectIndex(retained->resident);
        Check(available.scene.objects.size() + available.unavailable.size() + available.parent_records == records.size(),
              "World object coverage accounting differs");
        if (mode == "owned")
        {
            Check(records.size() == 1175 && available.scene.objects.size() == 135
                && available.unavailable.size() == 1007 && available.parent_records == 33, "Owned world coverage changed");
            Check(retained->resident.size() == 355408 && retained->temporary.size() == 4760032
                && retained->tweaks.size() == 322, "Owned world file sizes changed");
        }
        else
        {
            Check(available.scene.objects.size() == (mode == "partial" ? 1u : 2u)
                && available.unavailable.size() == (mode == "partial" ? 1u : 0u), "Synthetic world selection differs");
            Check(available.scene.models.size() == 1, "World did not share repeated model data");
        }
        std::cout << "World files: " << available.scene.objects.size() << " supported objects, "
            << available.unavailable.size() << " unavailable, " << available.parent_records << " parent records\n";
    }
    Check(!retained->resident.empty() && !retained->temporary.empty() && !retained->tweaks.empty(),
          "World file storage died with its load owner");
}
void Cancel()
{
    { FrontendWorldFileLoad load; load.Cancel(); load.Cancel();
        Check(load.State() == FrontendWorldFileState::Cancelled, "World cancellation did not become terminal");
        Reject([&] { load.Result(); }); }
    { FrontendWorldFileLoad load; nlShutdownFileSystem(); load.Poll();
        Check(load.State() == FrontendWorldFileState::Failed, "World read missed file shutdown");
        Reject([&] { load.Result(); }); }
    nlInitFileSystem();
    { FrontendWorldFileLoad load; }
    Check(!nlAsyncReadsPending(nullptr), "World destruction retained a read callback");
}
struct Session
{
    bool live = false, disc = false;
    ~Session() { if (live) ResetStartupFiles(); if (disc) aurora_dvd_close();
        if (live) { ResetStartupMemory(); aurora_shutdown(); } }
};
}
int main(int argc, char** argv)
{
    try
    {
        Check(argc == 4, "Supply disc, data directory and mode");
        const std::string_view mode = argv[3];
        const auto folder = (std::filesystem::path(argv[2]) / "frontend-world-data").string();
        std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged frontend world files"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
#ifdef MSCHARGED_TEST_WORLD_GX
        config.desiredBackend = BACKEND_VULKAN;
#endif
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot mount world disc"); session.disc = true; nlInitFileSystem();
        for (unsigned i = 0; i < (mode == "owned" ? 1u : 3u); ++i)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            Run(mode, i % 2); if (mode == "success") Cancel();
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b,
                  "World file reads leaked native arenas");
        }
        std::cout << checks << " frontend world resource checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
