#include "dvd_fixture_medium.h"
#include <atomic>
#include "runtime/audio_bank_load.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "audio_bank_fixture.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message)
{ ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid audio load succeeded"); }
auto Catalog()
{
    auto catalog = std::make_shared<resources::AudioBankCatalog>();
    catalog->names.push_back({25, "fixture"});
    catalog->slots.push_back({23, 0x1234, 0, false});
    return catalog;
}
void Pump(AudioBankLoad& load, bool external = false, bool read_error = false)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (load.State() == AudioBankLoadState::Loading)
    {
        try { if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service(); }
        catch (const std::runtime_error&) { if (!read_error) throw; load.Poll(); }
        Check(std::chrono::steady_clock::now() < deadline, "Audio bank load timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!nlAsyncReadsPending(nullptr), "Terminal audio bank retained a request");
}
void ReachWave(AudioBankLoad& load)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (load.Progress().requested_reads == 1)
    {
        load.Service();
        Check(load.State() == AudioBankLoadState::Loading && std::chrono::steady_clock::now() < deadline,
              "Audio metadata did not advance to wave read");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(load.Progress().completed_reads == 1, "Wave was read before its metadata callback");
}
struct Overlay
{
    std::atomic<unsigned> handles{0};
    std::atomic<bool> entered{false};
    static void* Open(void* p) { ++static_cast<Overlay*>(p)->handles; return p; }
    static void Close(void* p) { --static_cast<Overlay*>(p)->handles; }
    static std::int64_t Seek(void*, std::int64_t at, std::int32_t) { return at; }
    static std::int64_t Read(void* p, std::uint8_t*, std::size_t) { static_cast<Overlay*>(p)->entered=true; return -1; }
    Overlay(const char* path, std::uint64_t size)
    {
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek};
        aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{path, this, size};
        aurora_dvd_overlay_files(&file, 1, nullptr);
    }
    ~Overlay() { aurora_dvd_overlay_files(nullptr, 0, nullptr); }
};
void Lifecycle()
{
    auto catalog = Catalog();
    Reject([&] { AudioBankLoad load({}, 0, 0); });
    Reject([&] { AudioBankLoad load(catalog, 25, 23); }); // IDs are not array indices.
    catalog->slots[0].streaming = true;
    Reject([&] { AudioBankLoad load(catalog, 0, 0); });
    catalog->slots[0].streaming = false;
    catalog->names[0].name = "../fixture";
    Reject([&] { AudioBankLoad load(catalog, 0, 0); });
    catalog->names[0].name = "fixture";
    {
        AudioBankLoad load(catalog, 0, 0);
        catalog->names[0].name = "changed";
        catalog->slots[0].id = 99;
        Pump(load);
        Check(load.Result()->name.name == "fixture" && load.Result()->slot.id == 23,
              "Mutable catalog alias changed the submitted request");
        catalog->names[0].name = "fixture"; catalog->slots[0].id = 23;
    }
    {
        AudioBankLoad load(catalog, 0, 0);
        std::unique_ptr<nlFile> file(nlOpen("audio/fixture.resbun"));
        alignas(32) std::array<std::uint8_t, 32> data{};
        struct Context { AudioBankLoad* load; bool guarded = false; } context{&load};
        nlReadAsync(file.get(), data.data(), data.size(),
            [](nlFile*, void*, unsigned, nlFileAsyncParam param) {
                auto& c = *reinterpret_cast<Context*>(param);
                try { c.load->Cancel(); } catch (const std::logic_error&) { c.guarded = true; }
                throw std::runtime_error("Unrelated shared-pump callback failure");
            }, reinterpret_cast<nlFileAsyncParam>(&context), data.size());
        Pump(load, false, true);
        Check(context.guarded && load.State() == AudioBankLoadState::Failed,
              "Shared-pump exception or callback mutation bypassed bank cancellation");
        Reject([&] { load.Result(); });
    }
    for (bool wave : {false, true})
    {
        {
            AudioBankLoad load(catalog, 0, 0);
            if (wave) ReachWave(load);
            load.Cancel(); load.Cancel(); load.Service();
            Check(load.State() == AudioBankLoadState::Cancelled, "Cancelled bank became ready");
            Reject([&] { load.Result(); });
        }
        { AudioBankLoad load(catalog, 0, 0); if (wave) ReachWave(load); }
        Check(!nlAsyncReadsPending(nullptr), "Destroyed audio bank retained a callback");
        {
            AudioBankLoad load(catalog, 0, 0);
            if (wave) ReachWave(load);
            nlShutdownFileSystem(); load.Poll();
            Check(load.State() == AudioBankLoadState::Failed, "NL shutdown left a live bank read");
            Reject([&] { load.Result(); });
        }
        nlInitFileSystem();
    }
    for (bool external : {false, true}) for (const char* path : {"/audio/fixture.resbun", "/audio/fixture.nlxwb"})
    {
        Overlay bad_read(path, 128);
        AudioBankLoad load(catalog, 0, 0);
        Pump(load, external, true);
        Check(load.State() == AudioBankLoadState::Failed, "Failed audio read became ready");
        Reject([&] { load.Result(); });
        Check(bad_read.entered && !bad_read.handles, "Audio-bank fault did not perform real I/O and retire its file");
        mscharged::test::FinishDVDTestFaultCase(true);
    }
    for (const auto& [path, size] : std::initializer_list<std::pair<const char*, std::uint64_t>>{
        {"/audio/fixture.resbun", 0}, {"/audio/fixture.nlxwb", 0},
        {"/audio/fixture.resbun", resources::MaximumAssetBytes + 1}, {"/audio/fixture.nlxwb", 64 * 1024 * 1024 + 1}})
    {
        Overlay excessive(path, size);
        Reject([&] { AudioBankLoad load(catalog, 0, 0); });
        Check(!nlAsyncReadsPending(nullptr), "Invalid file sizes submitted audio reads");
    }
}
LoadedAudioBank::Handle Read(resources::AudioBankCatalog::Handle catalog,
                            unsigned name, unsigned slot, bool external, bool malformed)
{
    AudioBankLoad load(catalog, name, slot);
    Check(load.Progress().requested_reads == 1 && load.Progress().completed_reads == 0,
          "Wave read started before metadata completed");
    Reject([&] { load.Result(); });
    bool rejected = false;
    std::thread foreign([&] { try { load.Cancel(); } catch (const std::logic_error&) { rejected = true; } });
    foreign.join(); Check(rejected, "Foreign thread mutated the audio read");
    Pump(load, external);
    if (malformed)
    {
        Check(load.State() == AudioBankLoadState::Failed, "Malformed audio bank was published");
        Reject([&] { load.Result(); }); load.Cancel();
        Check(load.State() == AudioBankLoadState::Failed, "Cancel erased a decode failure");
        return {};
    }
    auto retained = load.Result();
    Check(retained && retained->bank && retained->bank->Samples().size() == 5, "Audio graph is incomplete");
    Check(retained->name_index == name && retained->slot_index == slot
          && retained->name.name == catalog->names[name].name && retained->slot.id == catalog->slots[slot].id,
          "Audio load confused record IDs and request indices");
    Check(load.Progress().requested_reads == 2 && load.Progress().completed_reads == 2,
          "Audio load did not complete both reads");
    load.Cancel(); load.Service();
    Check(load.Result() == retained, "Terminal bank lost its retained result");
    return retained;
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
        if (argc == 3 && std::string_view(argv[1]) == "--fixture")
        {
            const auto f = audio_bank_fixture::Make();
            const auto root = std::filesystem::path(argv[2]);
            std::filesystem::create_directories(root);
            for (const auto& [name, bytes] : {std::pair{"fixture.resbun", f.bytes}, {"fixture.nlxwb", f.wave}})
            { std::ofstream out(root / name, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); Check(bool(out), "Fixture write failed"); }
            return 0;
        }
        Check(argc == 4, "Supply image, data directory and test mode");
        const std::string_view mode = argv[3];
        Check(mode == "success" || mode == "missing" || mode == "malformed" || mode == "short-wave" || mode == "owned", "Unknown audio load test mode");
        Reject([] { AudioBankLoad load(Catalog(), 0, 0); });
        const auto folder = (std::filesystem::path(argv[2]) / "audio-bank-load-data").string();
        std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged audio bank load"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot mount audio bank disc"); session.disc = true; nlInitFileSystem();
        mscharged::test::ConfigureDVDTestMedium(argv[1]);
        resources::AudioBankCatalog::Handle catalog = Catalog();
        unsigned name = 0, slot = 0;
        if (mode == "owned")
        {
            std::unique_ptr<nlFile> file(nlOpen("audio/nlxgs.bun")); Check(bool(file), "Missing owned audio catalog");
            const auto size = nlFileSize(file.get(), nullptr); Check(size <= resources::MaximumAssetBytes, "Catalog exceeds budget");
            std::vector<std::uint8_t> data(size); nlRead(file.get(), data.data(), size, size);
            catalog = resources::ReadAudioBankCatalog(data); name = 25; slot = 23;
        }
        LoadedAudioBank::Handle retained;
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            if (mode == "missing") Reject([&] { AudioBankLoad load(catalog, name, slot); });
            else retained = Read(catalog, name, slot, attempt % 2, mode == "malformed" || mode == "short-wave");
            if (mode == "success") Lifecycle();
            Check(!nlAsyncReadsPending(nullptr), "Audio load retained a pending read");
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b, "Audio load leaked game arenas");
        }
        nlShutdownFileSystem(); ResetStartupMemory();
        if (retained)
        {
            Check(retained->bank->Samples().size() == 5 && !retained->bank->SampleBytes(0).empty(), "Retained bank expired with game arenas");
            std::cout << "Retained " << retained->name.name << " in slot " << retained->slot_index
                      << ": " << retained->bank->WaveBytes() << " wave bytes\n";
        }
        std::cout << checks << " audio bank load checks passed; no audio slot/voice readiness published\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
