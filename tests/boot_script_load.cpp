#include "dvd_fixture_medium.h"
#include <atomic>
#include "runtime/boot_script_load.h"
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

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool ok, const char* message)
{ ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid boot read operation succeeded"); }
void Pump(BootScriptLoad& load, bool external, bool failed_read = false)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (load.State() == BootScriptLoadState::Loading)
    {
        try { if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service(); }
        catch (const std::runtime_error&) { if (!failed_read) throw; load.Poll(); }
        Check(std::chrono::steady_clock::now() < deadline, "Boot script read timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!nlAsyncReadsPending(nullptr), "Terminal boot script retained reads");
}
std::shared_ptr<const BootScriptAsset> Read(std::string_view mode, bool external)
{
    if (mode == "missing" || mode == "empty")
    {
        Reject([] { BootScriptLoad load; });
        Check(!nlAsyncReadsPending(nullptr), "Rejected file submitted a boot read");
        return {};
    }
    BootScriptLoad load;
    Reject([&] { load.Result(); });
    bool rejected = false;
    std::thread wrong([&] { try { load.Cancel(); } catch (const std::logic_error&) { rejected = true; } });
    wrong.join(); Check(rejected && load.State() == BootScriptLoadState::Loading, "Wrong-thread cancellation changed the load");
    Pump(load, external);
    if (mode == "malformed" || mode == "truncated")
    {
        Check(load.State() == BootScriptLoadState::Failed, "Malformed bytecode was published");
        Reject([&] { load.Result(); }); load.Cancel();
        Check(load.State() == BootScriptLoadState::Failed, "Cancel erased a decode failure");
        return {};
    }
    Check(load.State() == BootScriptLoadState::Ready, "Boot bytecode did not become ready");
    auto asset = load.Result();
    Check(asset && asset->script && !asset->bytes.empty(), "Boot asset is incomplete");
    if (mode == "success")
    {
        Check(asset->script->functions.size() == 1 && asset->script->globals == std::vector<std::uint32_t>{42}
            && asset->script->data == std::vector<std::uint32_t>{0xcafebeef}
            && asset->script->code == std::vector<std::uint16_t>{0x400b, 0x5000}, "Synthetic script fields changed");
        Check(asset->script->strings == std::vector<std::uint8_t>{'l','o','a','d','i','n','g',0}, "Boot script string storage changed");
    }
    load.Cancel(); load.Service();
    Check(load.Result() == asset, "Completed read lost retained assets after cancellation");
    return asset;
}
void Cancellation()
{
    {
        BootScriptLoad load; load.Cancel(); load.Cancel();
        Check(load.State() == BootScriptLoadState::Cancelled, "Cancelled boot read became ready");
        Reject([&] { load.Result(); });
    }
    { BootScriptLoad load; } // Destruction cancels before its callback context dies.
    Check(!nlAsyncReadsPending(nullptr), "Destroyed boot owner retained requests");
    nlServiceFileSystem();
    {
        BootScriptLoad load; nlShutdownFileSystem(); load.Poll();
        Check(load.State() == BootScriptLoadState::Failed, "File shutdown left a live boot read");
        Reject([&] { load.Result(); });
    }
    nlInitFileSystem();
}
struct Overlay
{
    std::atomic<unsigned> handles{0};
    std::atomic<bool> entered{false};
    static void* Open(void* context) { ++static_cast<Overlay*>(context)->handles; return context; }
    static void Close(void* p) { --static_cast<Overlay*>(p)->handles; }
    static std::int64_t Seek(void*, std::int64_t offset, std::int32_t) { return offset; }
    static std::int64_t Read(void* p, std::uint8_t*, std::size_t) { static_cast<Overlay*>(p)->entered=true; return -1; }
    explicit Overlay(std::uint64_t size)
    {
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek};
        aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{"/art/Scripts/async_loading.byte_code", this, size};
        aurora_dvd_overlay_files(&file, 1, nullptr);
    }
    ~Overlay() { aurora_dvd_overlay_files(nullptr, 0, nullptr); }
};
void Failures()
{
    for (bool external : {false, true})
    {
        Overlay failure(100); BootScriptLoad load;
        Pump(load, external, true);
        Check(load.State() == BootScriptLoadState::Failed, "Read failure became boot readiness");
        Reject([&] { load.Result(); });
        Check(failure.entered && !failure.handles, "Boot fault did not perform real I/O and retire its file");
        mscharged::test::FinishDVDTestFaultCase(true);
    }
    Overlay excessive(resources::MaximumAssetBytes + 1);
    Reject([] { BootScriptLoad load; });
    Check(!nlAsyncReadsPending(nullptr), "Oversized boot script submitted reads");
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
        Check(argc == 4, "Supply image, data directory and test mode");
        const std::string_view mode = argv[3];
        Check(mode == "success" || mode == "missing" || mode == "empty" || mode == "malformed"
            || mode == "truncated" || mode == "owned", "Unknown boot script test mode");
        Reject([] { BootScriptLoad load; });
        const auto folder = (std::filesystem::path(argv[2]) / "boot-script-data").string();
        std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged boot script"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot mount boot script disc"); session.disc = true; nlInitFileSystem();
        mscharged::test::ConfigureDVDTestMedium(argv[1]);
        std::shared_ptr<const BootScriptAsset> retained;
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            retained = Read(mode, i % 2);
            if (mode == "success") { Cancellation(); Failures(); }
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b, "Boot read leaked game arenas");
        }
        nlShutdownFileSystem();
        if (retained)
        {
            auto fresh = resources::ReadScriptBytecode(retained->bytes);
            Check(fresh->code == retained->script->code && fresh->functions.size() == retained->script->functions.size(),
                  "Retained script storage expired after NL shutdown");
            std::cout << "Retained boot script: " << retained->bytes.size() << " bytes, " << fresh->functions.size()
                      << " functions, " << fresh->code.size() << " instructions\n";
        }
        std::cout << checks << " boot script ownership checks passed; no boot services executed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
