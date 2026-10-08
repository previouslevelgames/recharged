#include "dvd_fixture_medium.h"
#include <atomic>
#include "runtime/nis_bootstrap.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
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
void Check(bool result, const char* message)
{ ++checks; if (!result) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid NIS operation succeeded"); }
void Pump(NisBootstrapLoad& load, bool external, bool allow_read_error = false)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!load.Ready())
    {
        try { if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service(); }
        catch (const std::runtime_error&) { if (!allow_read_error) throw; load.Poll(); }
        Check(std::chrono::steady_clock::now() < end, "NIS bootstrap timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!nlAsyncReadsPending(nullptr), "Terminal bootstrap retained a pending read");
}
void Load(std::string_view mode, bool external)
{
    if (mode == "missing")
    {
        Reject([] { NisBootstrapLoad load; });
        Check(!nlAsyncReadsPending(nullptr), "Missing resource submitted reads");
        return;
    }
    std::shared_ptr<const NisBootstrapAssets> retained;
    {
        NisBootstrapLoad load;
        Reject([&] { load.Result(); });
        Pump(load, external);
        if (mode == "malformed") { Reject([&] { load.Result(); }); return; }
        Check(load.CompletedMask() == 31, "NIS bootstrap did not finish all five resources");
        retained = load.Result();
        Check(!retained->triggers.empty() && !retained->animation_proxy.empty(), "Missing bytecode bytes");
        if (mode == "owned")
        {
            Check(retained->dictionary.size() == 457 && retained->no_mirror.size() == 33
                && retained->no_picture_in_picture.size() == 51, "Owned bootstrap resource counts changed");
            bool eight = false;
            for (const auto& entry : retained->dictionary) eight |= entry.animation_starts.size() == 8;
            Check(eight, "Owned eight-actor positions were discarded");
        }
        else
        {
            Check(retained->dictionary.size() == 1 && retained->dictionary[0].animation_starts.size() == 8
                && retained->dictionary[0].proxies.size() == 1, "Synthetic NIS dictionary data differs");
            Check(retained->no_mirror.size() == 2 && retained->no_picture_in_picture.size() == 1,
                  "Synthetic NIS lists differ");
        }
        load.Cancel(); // Completed results remain valid.
        Check(load.Result() == retained, "Cancelling completed bootstrap discarded its result");
    }
    Check(!retained->dictionary.front().name.empty(), "Bootstrap result died with read owner");
}
void Cancellation()
{
    {
        NisBootstrapLoad load; load.Cancel(); load.Cancel();
        Check(load.Ready() && load.CompletedMask() == 0, "Cancelled bootstrap published readiness bits");
        Reject([&] { load.Result(); });
    }
    Check(!nlAsyncReadsPending(nullptr), "Cancellation did not drain requests");
    {
        NisBootstrapLoad load;
        nlShutdownFileSystem(); load.Poll();
        Check(load.Ready(), "File shutdown did not terminate bootstrap");
        Reject([&] { load.Result(); });
    }
    nlInitFileSystem();
    { NisBootstrapLoad load; } // Destruction must cancel callbacks before their context dies.
    Check(!nlAsyncReadsPending(nullptr), "Destruction retained callback contexts");
    nlServiceFileSystem();
}
struct ReadFailure
{
    std::atomic<unsigned> handles{0};
    std::atomic<bool> entered{false};
    static void* Open(void* context) { ++static_cast<ReadFailure*>(context)->handles; return context; }
    static void Close(void* p) { --static_cast<ReadFailure*>(p)->handles; }
    static std::int64_t Seek(void*, std::int64_t offset, std::int32_t) { return offset; }
    static std::int64_t Read(void* p, std::uint8_t*, std::size_t) { static_cast<ReadFailure*>(p)->entered=true; return -1; }
    ReadFailure()
    {
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek};
        aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{"/art/Scripts/nis_triggers.byte_code", this, 72};
        aurora_dvd_overlay_files(&file, 1, nullptr);
    }
    ~ReadFailure() { aurora_dvd_overlay_files(nullptr, 0, nullptr); }
};
void FailRead()
{
    for (bool external : {false, true})
    {
        ReadFailure failure; NisBootstrapLoad load;
        Pump(load, external, true);
        Reject([&] { load.Result(); });
        Check((load.CompletedMask() & 1) == 0, "Failed script read set its completion bit");
        Check(failure.entered && !failure.handles, "NIS fault did not perform real I/O and retire its file");
        mscharged::test::FinishDVDTestFaultCase(true);
    }
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
        Check(argc == 4, "Supply ISO, data directory and success/missing/malformed/owned");
        const std::string_view mode = argv[3];
        Check(mode == "success" || mode == "missing" || mode == "malformed" || mode == "owned", "Unknown mode");
        const auto folder = (std::filesystem::path(argv[2]) / "nis-bootstrap-data").string();
        std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged NIS bootstrap"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot mount NIS disc"); session.disc = true; nlInitFileSystem();
        mscharged::test::ConfigureDVDTestMedium(argv[1]);
        for (unsigned i = 0; i < (mode == "owned" ? 1u : 3u); ++i)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            Load(mode, i % 2);
            if (mode == "success") { Cancellation(); FailRead(); }
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b,
                  "NIS bootstrap leaked native arenas");
        }
        std::cout << checks << " NIS bootstrap checks passed; five-resource scope, no interpreter/actor initialization\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
