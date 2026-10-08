#include "dvd_fixture_medium.h"
#include <atomic>
#include "runtime/particle_files.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid particle read accepted"); }
void Pump(ParticleFileLoad& load, bool external = false, bool read_failure = false)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (load.State() == ParticleFileState::Loading)
    {
        try { if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service(); }
        catch (const std::runtime_error&) { if (!read_failure) throw; load.Poll(); }
        Check(std::chrono::steady_clock::now() < deadline, "Particle read timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!nlAsyncReadsPending(nullptr), "Terminal particle batch retained reads");
}
std::shared_ptr<const ParticleFiles> Read(std::string_view mode, bool external)
{
    if (mode.starts_with("missing-") || mode.starts_with("empty-"))
    {
        Reject([] { ParticleFileLoad load; });
        Check(!nlAsyncReadsPending(nullptr), "Rejected particle batch submitted reads");
        return {};
    }
    ParticleFileLoad load; Reject([&] { load.Result(); });
    bool wrong = false;
    std::thread other([&] { try { load.Cancel(); } catch (const std::logic_error&) { wrong = true; } });
    other.join(); Check(wrong && load.State() == ParticleFileState::Loading, "Wrong-thread mutation accepted");
    Pump(load, external);
    if (mode.starts_with("compressed-"))
    {
        Check(load.State() == ParticleFileState::Failed, "Invalid compressed particle data published");
        Reject([&] { load.Result(); }); load.Cancel();
        Check(load.State() == ParticleFileState::Failed, "Cancel erased the particle read error");
        return {};
    }
    Check(load.State() == ParticleFileState::Ready && load.CompletedFiles() == 4, "Particle batch is incomplete");
    auto result = load.Result();
    for (unsigned i = 0; i < result->data.size(); ++i)
    {
        Check(!result->data[i].empty() && result->source_sizes[i] != 0, "Particle file storage is empty");
        if (mode == "success")
            Check(result->data[i].size() == 101 + i * 17 && std::ranges::all_of(result->data[i],
                [i](auto byte) { return byte == i + 1; }), "Particle bytes changed or compressed file was not decoded");
    }
    load.Cancel(); load.Service(); Check(load.Result() == result, "Ready particle result was discarded");
    return result;
}
void Cancellation()
{
    { ParticleFileLoad load; load.Cancel(); load.Cancel();
      Check(load.State() == ParticleFileState::Cancelled, "Cancellation failed"); Reject([&] { load.Result(); }); }
    { ParticleFileLoad load; }
    Check(!nlAsyncReadsPending(nullptr), "Destroyed particle batch retained callbacks");
    nlServiceFileSystem();
    { ParticleFileLoad load; nlShutdownFileSystem(); load.Poll();
      Check(load.State() == ParticleFileState::Failed, "NL shutdown left live particle reads"); Reject([&] { load.Result(); }); }
    nlInitFileSystem();
}
struct Overlay
{
    std::atomic<unsigned> handles{0};
    std::atomic<bool> entered{false};
    static void* Open(void* user) { ++static_cast<Overlay*>(user)->handles; return user; }
    static void Close(void* p) { --static_cast<Overlay*>(p)->handles; }
    static std::int64_t Seek(void*, std::int64_t offset, std::int32_t) { return offset; }
    static std::int64_t Read(void* p, std::uint8_t*, std::size_t) { static_cast<Overlay*>(p)->entered=true; return -1; }
    Overlay(unsigned index, std::uint64_t size, bool batch = false)
    {
        static const char* paths[]{"/art/effects/effects.bun", "/art/effects/effectsNonRes.bun.zlib",
            "/art/objects/effectsgeometry.bun", "/art/objects/effectsgeometrytextures.rlt"};
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek}; aurora_dvd_overlay_callbacks(&callbacks);
        std::array<AuroraOverlayFile, 4> files{};
        for (unsigned i = 0; i < 4; ++i) files[i] = {paths[i], this, size};
        aurora_dvd_overlay_files(batch ? files.data() : &files[index], batch ? 4 : 1, nullptr);
    }
    ~Overlay() { aurora_dvd_overlay_files(nullptr, 0, nullptr); }
};
void Failures()
{
    for (unsigned i = 0; i < 4; ++i)
    {
        Overlay overlay(i, 100); ParticleFileLoad load; Pump(load, i % 2, true);
        Check(load.State() == ParticleFileState::Failed, "Read error became particle readiness");
        Reject([&] { load.Result(); });
        Check(overlay.entered && !overlay.handles, "Particle fault did not perform real I/O and retire its file");
        mscharged::test::FinishDVDTestFaultCase(true);
    }
    { Overlay overlay(0, resources::MaximumAssetBytes + 1); Reject([] { ParticleFileLoad load; }); }
    { Overlay overlay(0, resources::MaximumAssetBytes, true); Reject([] { ParticleFileLoad load; }); }
    Check(!nlAsyncReadsPending(nullptr), "Size rejection retained reads");
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
        Check(argc == 4, "Supply image, data directory and mode");
        const std::string_view mode = argv[3];
        Reject([] { ParticleFileLoad load; });
        const auto folder = (std::filesystem::path(argv[2]) / "particle-files-data").string();
        std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged particle files"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot open particle disc"); session.disc = true; nlInitFileSystem();
        mscharged::test::ConfigureDVDTestMedium(argv[1]);
        std::shared_ptr<const ParticleFiles> retained;
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            retained = Read(mode, i % 2);
            if (mode == "success") { Cancellation(); Failures(); }
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b,
                "Particle batch did not recover both game arenas");
        }
        nlShutdownFileSystem(); ResetStartupMemory();
        if (retained)
        {
            std::size_t total = 0;
            for (const auto& bytes : retained->data) total += bytes.size();
            Check(total != 0, "Retained particle data expired after shutdown");
            std::cout << "Retained particle files: 4 files, " << total << " decoded bytes; no effects registered\n";
        }
        std::cout << checks << " particle file ownership checks passed; both arenas recovered\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
