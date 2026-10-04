#include "runtime/frontend_visuals.h"
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
void Check(bool result, const char* message) { ++checks; if (!result) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid frontend visual read accepted"); }
void Pump(FrontendVisualLoad& load, bool external, bool allow_read_error = false)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!load.Ready())
    {
        try { if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service(); }
        catch (const std::runtime_error&) { if (!allow_read_error) throw; load.Poll(); }
        Check(std::chrono::steady_clock::now() < end, "Frontend visual reads timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!nlAsyncReadsPending(nullptr), "Frontend visual reads retained requests");
}
void Run(std::string_view mode, bool external, FrontendLanguage language)
{
    if (mode.starts_with("missing") || mode == "empty") { Reject([&] { FrontendVisualLoad load(language); }); return; }
    std::shared_ptr<const FrontendVisualAssets> retained;
    {
        FrontendVisualLoad load(language); Reject([&] { load.Result(); });
        bool wrong_thread = false;
        std::thread foreign([&] { try { load.Poll(); } catch (const std::logic_error&) { wrong_thread = true; } });
        foreign.join(); Check(wrong_thread, "Foreign thread serviced frontend visual files");
        Pump(load, external);
        if (mode != "success" && mode != "owned")
        { Reject([&] { load.Result(); }); return; }
        retained = load.Result();
        Check(load.CompletedMask() == 7 && retained->text && retained->heading && retained->localization,
            "Partial frontend visual set was published");
        Check(retained->text->alias == resources::FrontendNameHash("fot-rodinprob18")
            && retained->heading->alias == resources::FrontendNameHash("scratchy36"), "Original frontend font aliases differ");
        if (mode == "owned")
            Check(retained->localization->strings.size() == 1695 && retained->text->glyphs.size() == 267
                && retained->heading->glyphs.size() == 117, "Owned frontend visual coverage changed");
        else Check(retained->localization->Get(1) == u"AB", "Generated localization bytes changed");
    }
    Check(resources::LayoutFrontendText(retained->text, u"AB").quads.size() == 2,
        "Font storage died with its loading owner");
}
void Cancel()
{
    for (unsigned pumps : {0u, 1u, 2u})
    {
        FrontendVisualLoad load(FrontendLanguage::English);
        for (unsigned i = 0; i < pumps && !load.Ready(); ++i) load.Service();
        const bool was_ready = load.Ready(); load.Cancel(); load.Cancel();
        Check(load.Ready(), "Visual cancellation did not become terminal");
        if (!was_ready) Reject([&] { load.Result(); });
    }
    { FrontendVisualLoad load(FrontendLanguage::English); nlShutdownFileSystem(); load.Poll();
        Check(load.Ready(), "Visual read missed file shutdown"); Reject([&] { load.Result(); }); }
    nlInitFileSystem();
    { FrontendVisualLoad load(FrontendLanguage::English); }
    Check(!nlAsyncReadsPending(nullptr), "Visual destruction retained callbacks");
    Reject([] { FrontendVisualLoad load(static_cast<FrontendLanguage>(99)); });
}
struct ReadFailure
{
    bool short_read;
    static void* Open(void* context) { return context; }
    static void Close(void*) {}
    static std::int64_t Seek(void*, std::int64_t offset, std::int32_t) { return offset; }
    static std::int64_t Read(void* context, std::uint8_t*, std::size_t)
    { return static_cast<ReadFailure*>(context)->short_read ? 0 : -1; }
    explicit ReadFailure(bool short_read) : short_read(short_read)
    {
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek}; aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{"/Art/fe/fonts/eurfontheading36.res", this, 100};
        aurora_dvd_overlay_files(&file, 1, nullptr);
    }
    ~ReadFailure() { aurora_dvd_overlay_files(nullptr, 0, nullptr); }
};
void FailReads()
{
    for (bool short_read : {false, true})
    {
        ReadFailure failure(short_read); FrontendVisualLoad load(FrontendLanguage::English);
        Pump(load, short_read, true); Reject([&] { load.Result(); });
        Check((load.CompletedMask() & 4) == 0, "Failed font read published its completion bit");
    }
    std::vector<void*> blocks;
    for (unsigned size = 1024 * 1024; size >= 32; size /= 2)
        for (;;) { try { blocks.push_back(VirtualAllocator.Allocate(size, 32, false)); } catch (const std::bad_alloc&) { break; } }
    Reject([] { FrontendVisualLoad load(FrontendLanguage::English); });
    for (void* p : blocks) VirtualAllocator.Free(p);
    Check(!nlAsyncReadsPending(nullptr), "Failed visual allocation retained requests");
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
        const auto folder = (std::filesystem::path(argv[2]) / "frontend-visual-data").string();
        std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged frontend visual files"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
#ifdef MSCHARGED_TEST_WORLD_GX
        config.desiredBackend = BACKEND_VULKAN;
#endif
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot mount visual disc"); session.disc = true; nlInitFileSystem();
        std::shared_ptr<const FrontendVisualAssets> after_session;
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            Run(mode, i % 2, static_cast<FrontendLanguage>(i)); if (mode == "success") { Cancel(); FailReads(); }
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b,
                  "Visual reads leaked native arenas");
        }
        if (mode == "success")
        {
            FrontendVisualLoad load(FrontendLanguage::English); Pump(load, false); after_session = load.Result();
        }
        ResetStartupFiles(); aurora_dvd_close(); session.disc = false;
        ResetStartupMemory(); aurora_shutdown(); session.live = false;
        if (after_session) Check(resources::LayoutFrontendText(after_session->text, u"AB").quads.size() == 2,
            "Retained frontend font depended on released game arenas");
        std::cout << checks << " frontend visual ownership checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
