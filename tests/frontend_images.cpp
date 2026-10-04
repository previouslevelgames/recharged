#include "runtime/frontend_images.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <source_location>
#include <thread>

using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action, std::source_location at = std::source_location::current())
{
    ++checks; try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid image operation accepted at line " + std::to_string(at.line()));
}
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read image test input: " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
FrontendScene Scene(std::initializer_list<std::uint32_t> hashes)
{
    FrontendScene scene{};
    for (auto hash : hashes) scene.resources.push_back({unsigned(scene.resources.size() * 32), 0, hash, 0xffffffff, true});
    return scene;
}
constexpr std::uint32_t Hash(std::string_view value)
{ std::uint32_t hash = 0xffffffff; for (unsigned char c : value) hash = hash * 33 + c; return hash; }
FrontendImageCatalog::Handle Decode(Bytes data, const FrontendScene& scene)
{
    const std::array bundle{FrontendImageBundle{data, FrontendImageBundleKind::Permanent}};
    return ReadFrontendImages(scene, bundle);
}
void Decoder(const std::filesystem::path& folder)
{
    auto bytes = Read(folder / "formats.bundle");
    const auto entries = ReadFrontendImageDirectory(bytes);
    Check(entries.size() == 9, "Directory count changed");
    FrontendScene scene{};
    for (unsigned i = 0; i < 9; ++i) scene.resources.push_back({i * 32, 0, 0x100 + i, 0xfedcba98, true});
    const auto catalog = Decode(bytes, scene);
    bytes.clear(); bytes.shrink_to_fit();
    const unsigned gx[] = {4, 5, 14, 6, 1, 0, 1, 3, 9};
    const unsigned sizes[] = {160, 160, 64, 320, 96, 64, 96, 160, 96};
    for (unsigned i = 0; i < 9; ++i)
    {
        const auto& texture = *catalog->textures.at(0x100 + i);
        Check(texture.id == 0x100 + i && texture.game_format == i && texture.gx_format == gx[i]
            && texture.width == 8 && texture.height == 8 && texture.levels == 2, "Texture metadata changed");
        Check(texture.bits == std::array<std::uint8_t, 4>{1, 2, 3, 4}
            && texture.pixels.size() == sizes[i], "Tile extent or channel bits changed");
        for (unsigned n = 0; n < texture.pixels.size(); ++n)
            Check(texture.pixels[n] == (i + 17 + n) % (i == 8 ? 16 : 256), "Texture bytes differ from independent sequence");
        Check(texture.palette_entries == (i == 8 ? 16 : 0), "Palette count changed");
        if (i == 8) for (unsigned n = 0; n < 16; ++n)
            Check(U16(texture.palette, n * 2) == (0x8000 | (n * 1027 & 0x7fff)), "Palette byte order changed");
    }
    bytes = Read(folder / "valid.bundle");
    const auto required = Scene({0x11, 0x22});
    for (const auto& path : std::filesystem::directory_iterator(folder))
        if (path.path().extension() == ".bad") Reject([&] { Decode(Read(path.path()), required); });
    const auto directory = ReadFrontendImageDirectory(bytes);
    const auto required_end = directory.back().offset + directory.back().length;
    for (std::size_t size = 0; size < required_end; ++size)
        Reject([&] { Decode(Bytes(bytes).first(size), required); });
    Reject([&] { Decode(bytes, Scene({0xdead})); });
    auto forged = required; forged.resources[0].type = 3; Reject([&] { Decode(bytes, forged); });
    auto empty = Read(folder / "empty.bundle"); Check(ReadFrontendImageDirectory(empty).empty(), "Empty shipped bundle rejected");
    empty[15] = 0; Reject([&] { ReadFrontendImageDirectory(empty); });
    const auto dynamic = Scene({Hash("movie"), Hash("target/grab_texture")});
    auto unsupported = ReadFrontendImages(dynamic, {});
    Check(unsupported->textures.empty() && unsupported->unavailable.size() == 2, "Dynamic images became fake textures");
    const std::array wrong_order{FrontendImageBundle{bytes, FrontendImageBundleKind::OnDemand},
        FrontendImageBundle{bytes, FrontendImageBundleKind::Permanent}};
    Reject([&] { ReadFrontendImages(required, wrong_order); });
    const std::array bad_kind{FrontendImageBundle{bytes, static_cast<FrontendImageBundleKind>(9)}};
    Reject([&] { ReadFrontendImages(required, bad_kind); });
    // No FEN resources require no NL services, including a dynamic-only scene.
    FrontendImageLoad load; load.Begin({}); Check(load.CompletedFiles() == 0 && load.Result()->textures.empty(), "Text-only scene submitted reads");
    load.Begin(dynamic); Check(load.CompletedFiles() == 0 && load.Result()->unavailable.size() == 2, "Dynamic-only scene submitted reads");
    Reject([&] { load.Begin(required); }); Check(load.Current() == unsupported || load.Current()->unavailable.size() == 2,
        "Initialization failure discarded retained dynamic status");
}
void Pump(FrontendImageLoad& load, bool external = false, bool errors = false)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (load.State() == FrontendImageState::Loading)
    {
        try { if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service(); }
        catch (...) { if (!errors) throw; load.Poll(); }
        Check(std::chrono::steady_clock::now() < deadline, "Frontend image reads timed out"); SDL_Delay(1);
    }
}
void Inspect(const FrontendImageCatalog::Handle& catalog, bool ingame)
{
    Check(catalog && catalog->textures.size() == 2 && catalog->unavailable.empty(), "Image catalog count differs");
    Check(catalog->textures.at(0x11)->pixels[0] == (ingame ? 80 : 65), "Permanent/first-directory precedence changed");
    Check(catalog->textures.at(ingame ? 0x22 : 0x33)->pixels[0] == (ingame ? 3 : 90), "On-demand resolution changed");
}
struct FaultFile
{
    enum Mode { Error, Short, Blocked } mode;
    std::atomic<bool> entered{false}, finished{false};
    std::atomic<unsigned> handles{0};
    std::mutex mutex; std::condition_variable gate; bool released = false;
    struct Handle { FaultFile* owner; std::int64_t position = 0; };
    static void* Open(void* context) { auto* f = static_cast<FaultFile*>(context); ++f->handles; return new Handle{f}; }
    static void Close(void* context) { auto* h = static_cast<Handle*>(context); --h->owner->handles; delete h; }
    static std::int64_t Seek(void* context, std::int64_t offset, std::int32_t origin)
    { if (origin || offset < 0 || offset > 128) return -1; return static_cast<Handle*>(context)->position = offset; }
    static std::int64_t Read(void* context, std::uint8_t* data, std::size_t size)
    {
        auto& h = *static_cast<Handle*>(context); auto& f = *h.owner; f.entered = true;
        if (f.mode == Error) { f.finished = true; return -1; }
        if (f.mode == Blocked)
        {
            std::unique_lock lock(f.mutex);
            if (!f.gate.wait_for(lock, std::chrono::seconds(2), [&] { return f.released; }))
            { f.finished = true; return -1; }
        }
        const auto count = std::min<std::size_t>(size, f.mode == Short ? (h.position ? 0 : 3) : 128 - h.position);
        std::fill_n(data, count, 0); h.position += count; f.finished = true; return count;
    }
    explicit FaultFile(Mode m, const char* path = "/art/fe/InGameUI.Dmn", std::size_t size = 128) : mode(m)
    {
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek}; aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{path, this, size}; aurora_dvd_overlay_files(&file, 1, nullptr);
    }
    ~FaultFile()
    {
        { std::lock_guard lock(mutex); released = true; } gate.notify_all();
        aurora_dvd_overlay_files(nullptr, 0, nullptr);
        if (handles) std::terminate();
    }
    void Wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!entered) { Check(std::chrono::steady_clock::now() < deadline, "Fault worker did not start"); SDL_Delay(1); }
    }
};
void Transactions()
{
    auto main = Scene({0x11, 0x33}), ingame = Scene({0x11, 0x22});
    FrontendImageLoad load;
    Check(load.State() == FrontendImageState::Idle && !load.Current(), "New owner is not empty");
    load.Begin(main); main.resources.clear(); Reject([&] { load.Result(); }); Pump(load);
    auto previous = load.Result(); Inspect(previous, false); Check(load.CompletedFiles() == 1, "Main profile selected wrong files");
    bool rejected = false;
    std::thread other([&] { try { load.Begin(ingame); } catch (const std::logic_error&) { rejected = true; } });
    other.join(); Check(rejected, "Wrong-thread image mutation accepted");
    Reject([&] { load.Begin(ingame, static_cast<FrontendImageProfile>(4)); });
    Check(load.Current() == previous, "Invalid profile changed publication");
    load.Begin(ingame); Pump(load);
    Check(load.State() == FrontendImageState::Failed && load.Current() == previous, "Missing ordinary hash silently became ready");
    Reject([&] { load.Result(); });
    load.Begin(ingame, FrontendImageProfile::InGame); load.Cancel(); load.Cancel();
    Check(load.State() == FrontendImageState::Cancelled && load.Current() == previous, "Cancellation lost previous catalog");
    load.Begin(Scene({0x11, 0x33})); load.Begin(ingame, FrontendImageProfile::InGame); Pump(load, true);
    auto next = load.Result(); Inspect(next, true); Inspect(previous, false);
    Check(next != previous && load.CompletedFiles() == 2, "Replacement did not publish both qualified files");
    load.Unload(); Check(!load.Current(), "Unload left its handle active"); Inspect(next, true);
    load.Begin(ingame, FrontendImageProfile::InGame);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (load.CompletedFiles() != 2)
    { nlServiceFileSystem(); Check(std::chrono::steady_clock::now() < deadline, "Raw completion timed out"); SDL_Delay(1); }
    load.Cancel(); Check(!load.Current() && load.State() == FrontendImageState::Cancelled, "Cancel published staged bytes");
    load.Begin(ingame, FrontendImageProfile::InGame); nlShutdownFileSystem(); load.Poll();
    Check(load.State() == FrontendImageState::Failed, "File shutdown left a pending owner"); nlInitFileSystem();
}
void Failures()
{
    auto scene = Scene({0x11, 0x22}); FrontendImageLoad load;
    load.Begin(scene, FrontendImageProfile::InGame); Pump(load); auto previous = load.Result();
    for (auto mode : {FaultFile::Error, FaultFile::Short})
    {
        FaultFile fault(mode); load.Begin(scene, FrontendImageProfile::InGame); Pump(load, false, true);
        Check(load.State() == FrontendImageState::Failed && load.Current() == previous && fault.finished && !fault.handles,
            "Worker failure changed publication or leaked a handle"); Reject([&] { load.Result(); });
    }
    { FaultFile fault(FaultFile::Error, "/art/fe/InGameUI.Dmn", MaximumAssetBytes + 1);
      Reject([&] { load.Begin(scene, FrontendImageProfile::InGame); });
      Check(load.Current() == previous && !fault.entered, "Oversized bundle submitted work"); }
    for (bool replace : {false, true})
    {
        FaultFile fault(FaultFile::Blocked); load.Begin(scene, FrontendImageProfile::InGame); fault.Wait();
        std::jthread release([&] { SDL_Delay(15); { std::lock_guard lock(fault.mutex); fault.released = true; } fault.gate.notify_all(); });
        if (replace) load.Begin(Scene({0x11, 0x33})); else load.Cancel();
        Check(fault.finished && !fault.handles && load.Current() == previous, "Cancellation did not drain the active worker");
        if (replace) { Pump(load); Inspect(load.Result(), false); previous = load.Current(); }
        else Check(!nlAsyncReadsPending(nullptr), "Cancellation retained an NL read");
    }
    {
        std::unique_ptr<nlFile> file(nlOpen("/art/fe/MainUI.Dmn"));
        alignas(32) std::array<std::array<std::uint8_t, 32>, 63> buffers{};
        for (auto& bytes : buffers) { nlSeek(file.get(), 0, 0); nlReadAsync(file.get(), bytes.data(), 32, nullptr, 0, 32); }
        Reject([&] { load.Begin(scene, FrontendImageProfile::InGame); });
        Check(load.Current() == previous, "Partial submission discarded the current catalog");
        nlCancelPendingAsyncReads(file.get(), nullptr); Check(!nlAsyncReadsPending(nullptr), "Partial submission retained work");
    }
    {
        struct Blocks { std::vector<void*> values; ~Blocks() { for (auto p : values) VirtualAllocator.Free(p); } } blocks;
        for (;;) { try { blocks.values.push_back(VirtualAllocator.Allocate(256 * 1024, 32, false)); } catch (const std::bad_alloc&) { break; } }
        const auto remaining = VirtualAllocator.LargestFreeBlock();
        if (remaining > 256) blocks.values.push_back(VirtualAllocator.Allocate(remaining - 128, 32, false));
        Reject([&] { load.Begin(scene, FrontendImageProfile::InGame); });
        Check(load.Current() == previous && !nlAsyncReadsPending(nullptr), "Arena exhaustion leaked or changed the catalog");
    }
    struct Callback
    {
        FrontendImageLoad* load; bool ran = false;
        static void Run(void* data, unsigned long, void* context)
        {
            std::unique_ptr<void, void(*)(void*)> free(data, nlFree);
            auto& c = *static_cast<Callback*>(context); c.ran = true;
            Reject([&] { c.load->Begin({}); }); Reject([&] { c.load->Poll(); }); Reject([&] { c.load->Cancel(); });
            Reject([&] { c.load->Service(); }); Reject([&] { c.load->Unload(); });
        }
    } callback{&load};
    nlLoadEntireFileAsync("/art/fe/MainUI.Dmn", Callback::Run, &callback, 32, AllocateEnd, nullptr, 0, &VirtualAllocator);
    load.Begin(scene, FrontendImageProfile::InGame); Pump(load); Check(callback.ran, "Reentrant callback not exercised");
}
struct Session
{
    bool live = false, disc = false;
    ~Session() { if (live) ResetStartupFiles(); if (disc) aurora_dvd_close(); if (live) { ResetStartupMemory(); aurora_shutdown(); } }
};
}
int main(int argc, char** argv)
{
    try
    {
        Check(argc == 4, "Supply image, fixture directory and mode");
        const std::string mode = argv[3]; const bool success = mode == "success", owned = mode == "owned";
        if (success) Decoder(argv[2]);
        const auto folder = (std::filesystem::path(argv[2]) / "image-runtime-data").string(); std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged frontend images"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot open frontend image disc"); session.disc = true; nlInitFileSystem();
        FrontendImageCatalog::Handle retained;
        for (unsigned repeat = 0; repeat < 3; ++repeat)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            if (success) { Transactions(); Failures(); }
            FrontendImageLoad load;
            if (owned)
            {
                for (const auto& path : std::filesystem::directory_iterator(std::filesystem::path(argv[2]) / "scenes"))
                {
                    if (path.path().extension() != ".fen") continue;
                    auto scene = ReadFrontendScene(Read(path.path()));
                    for (auto profile : {FrontendImageProfile::Main, FrontendImageProfile::InGame})
                    {
                        load.Begin(scene, profile); Pump(load, repeat % 2);
                        if (load.State() == FrontendImageState::Ready)
                        {
                            retained = load.Result();
                            if (!repeat) std::cout << "ACCEPT\t" << path.path().filename().string() << '\t'
                                << (profile == FrontendImageProfile::Main ? "main" : "ingame") << '\t'
                                << retained->textures.size() << '\t' << retained->unavailable.size() << '\n';
                        }
                        else
                        {
                            Check(load.State() == FrontendImageState::Failed, "Owned unsupported scene has no terminal failure");
                            if (!repeat) { try { load.Result(); } catch (const std::exception& e) { std::cout << "REJECT\t"
                                << path.path().filename().string() << '\t' << (profile == FrontendImageProfile::Main ? "main" : "ingame") << '\t' << e.what() << '\n'; } }
                        }
                    }
                }
            }
            else if (!success)
            {
                load.Begin({}); auto previous = load.Result();
                const bool ingame = mode == "missing-permanent" || mode == "missing-demand" || mode == "bad-demand";
                try { load.Begin(ingame ? Scene({0x11, 0x22}) : Scene({0x11, 0x33}),
                    ingame ? FrontendImageProfile::InGame : FrontendImageProfile::Main); Pump(load); } catch (const std::exception&) { load.Poll(); }
                Check(load.State() == FrontendImageState::Failed && load.Current() == previous, "Missing/malformed image input became ready");
                Reject([&] { load.Result(); });
            }
            else { load.Begin(Scene({0x11, 0x22}), FrontendImageProfile::InGame); Pump(load); retained = load.Result(); }
            load.Unload(); Check(!nlAsyncReadsPending(nullptr), "Transaction retained NL work");
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b, "Image transaction did not recover both arenas");
        }
        nlShutdownFileSystem(); ResetStartupMemory();
        if (retained) for (const auto& [hash, texture] : retained->textures)
            Check(texture && hash == texture->id && !texture->pixels.empty(), "Retained texture did not survive arena shutdown");
        std::cout << checks << " frontend image checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
