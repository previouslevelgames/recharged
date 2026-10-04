#include "runtime/animation_bundle.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/SHierarchy.h"
#include "Game/SAnim.h"
#include "Game/SAnim/AnimRetargeter.h"
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
#include <string_view>
#include <thread>

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool condition, const char* message)
{ ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void Reject(F action, std::source_location at = std::source_location::current())
{ ++checks; try { action(); } catch (const std::exception&) { return; }
  throw std::runtime_error("Invalid bundle operation accepted at test line " + std::to_string(at.line())); }
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read generated world fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
void Inspect(const AnimationBundle::Handle& bundle, std::uint32_t identity, unsigned count)
{
    Check(bundle && bundle->Hierarchy()->Data().GetHashID() == identity && bundle->Size() == count,
        "Bundle lost its authored hierarchy/animation association");
    auto hierarchy = bundle->Hierarchy();
    for (int node = 0; node < hierarchy->Data().GetNumNodes(); ++node)
    {
        Check(bundle->AnimationNode(node) == unsigned(node), "Original direct node identity changed");
        Check(bundle->AnimationNode(node, true) == unsigned(hierarchy->Data().GetMirroredNode(node)),
            "Original mirror-before-remap order changed");
    }
    Reject([&] { bundle->AnimationNode(hierarchy->Data().GetNumNodes()); });
    Reject([&] { bundle->At(bundle->Size()); });
    Check(!bundle->Find(0x1234abcd), "Missing animation silently fell back to a different track");
    for (unsigned i = 0; i < bundle->Size(); ++i)
    {
        auto animation = bundle->At(i);
        for (float time : {0.f, .125f, .5f, 1.f})
        {
            const auto root = animation->Root(time);
            Check(std::isfinite(root.translation[0]), "Retained original root sampling failed");
            for (int node = 0; node < hierarchy->Data().GetNumNodes(); ++node)
            {
                const auto index = bundle->AnimationNode(node, true);
                const auto keys = animation->Keys(index);
                if (keys.rotation) animation->RotationKey(index, keys.rotation - 1);
                Check(std::isfinite(animation->Weight(index, time).value), "Retained original weight sampling failed");
            }
        }
    }
}
void Decoder(const std::filesystem::path& folder)
{
    for (unsigned seed = 0; seed < 36; ++seed)
    {
        auto resident = Read(folder / ("alignment-" + std::to_string(seed) + ".res"));
        auto temporary = Read(folder / ("alignment-" + std::to_string(seed) + ".tmp"));
        auto a = AnimationBundle::Decode(resident, temporary, 0xaaaa);
        auto b = AnimationBundle::Decode(resident, temporary, 0xbbbb);
        Inspect(a, 0xaaaa, 2); Inspect(b, 0xbbbb, 2);
        Check(a->At(0)->Data().m_nHierarchySignature == 0x1111
            && b->At(0)->Data().m_nHierarchySignature == 0x2222, "Equal counts were mistaken for hierarchy identity");
        Check(a->Find(0x10) == a->At(1), "Original last-added-first hash lookup changed");
        Check(b->Find(0x21) == b->At(1), "World association was reset between resident and temporary files");
        Check(std::get<std::array<float, 4>>(a->Find(0x10)->RotationKey(0, 0))[0] == float(seed % 32 + 8) / 128,
            "Embedded absolute alignment or animation channel bits changed");
        Reject([&] { AnimationBundle::Decode(resident, temporary, 0xdead); });
        resident.clear(); temporary.clear();
        Check(a->Hierarchy()->Data().GetNodeID(1) == 0x80000001, "Hierarchy borrowed the discarded input bytes");
    }
    for (const auto mode : {"orphan", "parent", "empty", "signature", "nodes", "retarget", "duplicate",
                           "root", "truncated", "trailing"})
    {
        const auto resident = Read(folder / (std::string(mode) + ".res"));
        const auto temporary = Read(folder / (std::string(mode) + ".tmp"));
        Reject([&] { AnimationBundle::Decode(resident, temporary, 0xaaaa); });
    }
    // Extra animation nodes are unused by original direct hierarchy traversal;
    // equality is neither required for safety nor accepted as identity proof.
    const auto extra = AnimationBundle::Decode(Read(folder / "extra-nodes.res"), Read(folder / "extra-nodes.tmp"), 0xaaaa);
    Check(extra->At(1)->Data().m_nNumNodes == 3 && extra->Hierarchy()->Data().GetNumNodes() == 2,
        "Safe extra animation nodes were discarded or rejected");
}
AnimationBundleRequest Request(unsigned identity = 0xaaaa)
{ return {"/world.res", "/world.tmp", identity}; }
void Pump(AnimationBundleLoad& load, bool external = false, bool failure = false)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (load.State() == AnimationBundleState::Loading)
    {
        try { if (external) { nlServiceFileSystem(); load.Poll(); } else load.Service(); }
        catch (const std::runtime_error&) { if (!failure) throw; load.Poll(); }
        Check(std::chrono::steady_clock::now() < deadline, "Animation bundle read timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!nlAsyncReadsPending(nullptr), "Animation bundle transaction retained reads");
}
struct FaultFile
{
    enum Mode { Error, Short, Blocked } mode;
    std::atomic<bool> entered{false}, finished{false};
    std::atomic<unsigned> handles{0};
    std::mutex mutex;
    std::condition_variable gate;
    bool released = false;
    struct Handle { FaultFile* owner; std::int64_t position = 0; };
    static void* Open(void* context) { auto* f = static_cast<FaultFile*>(context); auto* h = new Handle{f}; ++f->handles; return h; }
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
    explicit FaultFile(Mode m, const char* path = "/world.tmp", std::size_t size = 128) : mode(m)
    {
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek}; aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{path, this, size}; aurora_dvd_overlay_files(&file, 1, nullptr);
    }
    ~FaultFile()
    {
        { std::lock_guard lock(mutex); released = true; } gate.notify_all();
        aurora_dvd_overlay_files(nullptr, 0, nullptr);
        if (handles) std::terminate(); // Never unwind live worker callback storage.
    }
    void Wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!entered) { Check(std::chrono::steady_clock::now() < deadline, "Fault worker did not start"); SDL_Delay(1); }
    }
};
void Transactions()
{
    AnimationBundleLoad load;
    Check(load.State() == AnimationBundleState::Idle && !load.Current(), "New bundle owner is not empty");
    load.Begin(Request()); Reject([&] { load.Result(); });
    bool wrong_thread = false;
    std::thread other([&] { try { load.Cancel(); } catch (const std::logic_error&) { wrong_thread = true; } });
    other.join(); Check(wrong_thread, "Wrong-thread bundle mutation was accepted");
    Pump(load); auto previous = load.Result(); Inspect(previous, 0xaaaa, 2);
    Check(load.CompletedFiles() == 2, "Ready bundle omitted a required file");
    for (const auto path : {"/missing.res", "/empty.res"})
    {
        auto request = Request(); request.resident = path;
        Reject([&] { load.Begin(request); });
        Check(load.State() == AnimationBundleState::Failed && load.Current() == previous, "Preflight failure replaced the active bundle");
        Reject([&] { load.Result(); });
    }
    for (const auto path : {"/bad.res", "/broken.res.zlib", "/oversize.res.zlib"})
    {
        auto request = Request(); request.resident = path; load.Begin(request); Pump(load, true);
        Check(load.State() == AnimationBundleState::Failed && load.Current() == previous, "Malformed replacement discarded the previous bundle");
        Reject([&] { load.Result(); }); load.Cancel();
        Check(load.State() == AnimationBundleState::Failed, "Cancellation erased a recorded failure");
    }
    { auto request = Request(); request.resident = std::string("/world.res\0ignored", 18);
      Reject([&] { load.Begin(request); }); Check(load.Current() == previous, "Embedded NUL changed the active bundle"); }
    load.Begin(Request(0xbbbb)); load.Cancel(); load.Cancel();
    Check(load.State() == AnimationBundleState::Cancelled && load.Current() == previous, "Cancelled replacement lost previous ownership");
    Reject([&] { load.Result(); });
    load.Begin(Request()); load.Begin({"/other.res", "/other.tmp", 0xbbbb}); Pump(load);
    auto next = load.Result(); Inspect(next, 0xbbbb, 2);
    Check(next != previous && previous->Hierarchy()->Data().GetHashID() == 0xaaaa, "Replacement invalidated retained handles");
    load.Begin({"/compressed.res.zlib", "/compressed.tmp.zlib", 0xaaaa}); Pump(load);
    Inspect(load.Result(), 0xaaaa, 2);
    load.Unload(); Check(!load.Current() && load.State() == AnimationBundleState::Idle, "Unload left its publication active");
    Inspect(previous, 0xaaaa, 2); Inspect(next, 0xbbbb, 2);
    // Completed raw reads remain unpublished until Poll; cancellation must not
    // accidentally publish them by calling Poll as a side effect.
    load.Begin(Request());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (load.CompletedFiles() != 2)
    { nlServiceFileSystem(); Check(std::chrono::steady_clock::now() < deadline, "Raw file completion timed out"); SDL_Delay(1); }
    load.Cancel(); Check(!load.Current() && load.State() == AnimationBundleState::Cancelled, "Cancel published staged data");
    load.Begin(Request()); nlShutdownFileSystem(); load.Poll();
    Check(load.State() == AnimationBundleState::Failed && !load.Current(), "NL shutdown left a pending bundle live");
    Reject([&] { load.Result(); }); nlInitFileSystem();
}
void Failures()
{
    AnimationBundleLoad load; load.Begin(Request()); Pump(load); auto previous = load.Result();
    for (auto mode : {FaultFile::Error, FaultFile::Short})
    {
        FaultFile fault(mode); load.Begin(Request()); Pump(load, false, true);
        Check(load.State() == AnimationBundleState::Failed && load.Current() == previous && fault.finished && !fault.handles,
            "Worker failure leaked storage or replaced the previous bundle"); Reject([&] { load.Result(); });
    }
    { FaultFile fault(FaultFile::Error, "/world.res", resources::MaximumAssetBytes + 1);
      Reject([&] { load.Begin(Request()); }); Check(load.Current() == previous && !fault.entered, "Oversized batch submitted a worker"); }
    for (bool replace : {false, true})
    {
        FaultFile fault(FaultFile::Blocked); load.Begin(Request()); fault.Wait();
        std::jthread release([&] { SDL_Delay(15); { std::lock_guard lock(fault.mutex); fault.released = true; } fault.gate.notify_all(); });
        if (replace) load.Begin({"/other.res", "/other.tmp", 0xbbbb}); else load.Cancel();
        Check(fault.finished && !fault.handles && load.Current() == previous, "Cancellation returned before draining an active worker");
        if (replace) { Pump(load); Inspect(load.Result(), 0xbbbb, 2); previous = load.Current(); }
        else Check(!nlAsyncReadsPending(nullptr), "Cancelled worker left an NL read active");
    }
    // Occupy63 of64 read slots. Aligned worlds reserve one each: first bundle
    // submission succeeds, second fails, and only that bundle's job is drained.
    {
        std::unique_ptr<nlFile> file(nlOpen("/world.res"));
        Check(file != nullptr, "Cannot open pool fixture");
        alignas(32) std::array<std::array<std::uint8_t, 32>, 63> buffers{};
        for (auto& bytes : buffers) { nlSeek(file.get(), 0, 0); nlReadAsync(file.get(), bytes.data(), 32, nullptr, 0, 32); }
        Reject([&] { load.Begin(Request()); });
        Check(load.State() == AnimationBundleState::Failed && load.Current() == previous,
            "Partial submission replaced the active bundle");
        nlCancelPendingAsyncReads(file.get(), nullptr);
        Check(!nlAsyncReadsPending(nullptr), "Partial submission retained a private read");
    }
    // Exhaust the real file-buffer arena, keeping all allocations scoped until
    // the failed request has been rolled back.
    {
        struct Blocks { std::vector<void*> data; ~Blocks() { for (auto* p : data) VirtualAllocator.Free(p); } } blocks;
        for (;;) { try { blocks.data.push_back(VirtualAllocator.Allocate(256 * 1024, 32, false)); } catch (const std::bad_alloc&) { break; } }
        const auto remaining = VirtualAllocator.LargestFreeBlock();
        if (remaining > 256) blocks.data.push_back(VirtualAllocator.Allocate(remaining - 128, 32, false));
        Reject([&] { load.Begin(Request()); });
        Check(load.Current() == previous && !nlAsyncReadsPending(nullptr), "Arena exhaustion leaked a read or lost the retained set");
    }
    // An unrelated NL completion may run inside Service. Mutations from it
    // must be rejected while this owner is servicing its transaction.
    struct Callback
    {
        AnimationBundleLoad* load; bool ran = false;
        static void Run(void* data, unsigned long, void* context)
        {
            std::unique_ptr<void, void(*)(void*)> free(data, nlFree);
            auto& c = *static_cast<Callback*>(context); c.ran = true;
            Reject([&] { c.load->Begin(Request()); }); Reject([&] { c.load->Cancel(); });
            Reject([&] { c.load->Poll(); }); Reject([&] { c.load->Service(); }); Reject([&] { c.load->Unload(); });
        }
    } callback{&load};
    nlLoadEntireFileAsync("/world.res", Callback::Run, &callback, 32, AllocateEnd, nullptr, 0, &VirtualAllocator);
    load.Begin(Request()); Pump(load);
    Check(callback.ran, "Reentrant NL callback was not exercised");
}
void CharacterData(const AnimationBundle::Handle& bundle, bool synthetic)
{
    Check(bundle && bundle->Retargets() && bundle->Size() > 0, "Character bundle omitted native owners");
    Reject([&] { bundle->AnimationNode(0); });
    for (unsigned track = 0; track < bundle->Size(); ++track)
    {
        const auto& data = bundle->At(track)->Data();
        for (int node = 0; node < bundle->Hierarchy()->Data().GetNumNodes(); ++node)
            for (bool mirror : {false, true})
            {
                const auto source = bundle->MappedNode(track, node, mirror);
                Check(!source || *source < data.m_nNumNodes, "Character map exceeds the source animation");
                if (source) bundle->At(track)->Weight(*source, .5f);
            }
    }
    if (synthetic)
    {
        Check(bundle->Size() == 2 && bundle->Retarget(0) && bundle->Retarget(1), "Character did not select both map signatures");
        Check(!bundle->MappedNode(0, 0) && bundle->MappedNode(0, 1) == 2
            && bundle->MappedNode(0, 0, true) == 2 && !bundle->MappedNode(0, 1, true),
            "Target mirror order or the unmapped sentinel changed");
        Check(bundle->MappedNode(1, 0) == 0 && !bundle->MappedNode(1, 1), "Second track used the first track's map");
        Check(bundle->Retarget(0)->m_NumBones == 0 && bundle->Retarget(0)->m_Unknown08 == 2,
            "Serialized metadata was mistaken for the target map length");
    }
}
void Characters(bool bad)
{
    AnimationBundleLoad load; load.Begin(Request()); Pump(load); auto previous = load.Result();
    if (bad)
    {
        try { load.BeginCharacter(0); Pump(load); } catch (const std::runtime_error&) { load.Poll(); }
        Check(load.State() == AnimationBundleState::Failed && load.Current() == previous,
            "Invalid or missing character input replaced the active world bundle");
        Reject([&] { load.Result(); }); Check(!nlAsyncReadsPending(nullptr), "Character failure retained a read");
        return;
    }
    Reject([&] { load.BeginCharacter(20); }); Check(load.Current() == previous, "Invalid character index changed publication");
    load.BeginCharacter(0); Check(load.Current() == previous, "Character published before three reads validated"); Pump(load);
    previous = load.Result(); CharacterData(previous, true);
    Check(load.CompletedFiles() == 3, "Character transaction omitted a required read");
    load.BeginCharacter(4); load.Cancel(); Check(load.Current() == previous, "Character cancellation discarded the current bundle");
    load.BeginCharacter(0); load.BeginCharacter(4); Pump(load, true); CharacterData(load.Result(), true);
    Check(load.Current() != previous && std::string_view(load.Current()->Hierarchy()->Data().m_szName) == "luigi",
        "Pending character replacement published the stale identity");
    CharacterData(previous, true); previous = load.Current();
    for (const auto path : {CharacterAnimation(0).hierarchy_path, CharacterAnimation(0).animation_path,
                           CharacterAnimation(0).retarget_path})
    {
        const std::string absolute = "/" + std::string(path);
        FaultFile fault(FaultFile::Error, absolute.c_str()); load.BeginCharacter(0); Pump(load, false, true);
        Check(load.State() == AnimationBundleState::Failed && load.Current() == previous && !fault.handles,
            "A character read failure changed publication or retained a worker");
    }
    {
        const std::string path = "/" + std::string(CharacterAnimation(0).retarget_path);
        FaultFile fault(FaultFile::Blocked, path.c_str()); load.BeginCharacter(0); fault.Wait();
        std::jthread release([&] { SDL_Delay(15); { std::lock_guard lock(fault.mutex); fault.released = true; } fault.gate.notify_all(); });
        load.Cancel(); Check(fault.finished && !fault.handles && load.Current() == previous,
            "Character cancellation did not drain its active third read");
    }
    // Whole-file loading supplies a padded allocation/capacity, so even a
    // nonaligned logical size consumes one raw read (no scratch-tail split).
    // Leave two slots: third submission must fail and roll back the first two.
    {
        std::unique_ptr<nlFile> file(nlOpen("/world.res"));
        alignas(32) std::array<std::array<std::uint8_t, 32>, 64> buffers{};
        for (unsigned i = 0; i < 62; ++i)
        { nlSeek(file.get(), 0, 0); nlReadAsync(file.get(), buffers[i].data(), 32, nullptr, 0, 32); }
        Reject([&] { load.BeginCharacter(0); });
        Check(load.State() == AnimationBundleState::Failed && load.Current() == previous, "Third submission failure changed the current character");
        nlCancelPendingAsyncReads(file.get(), nullptr); Check(!nlAsyncReadsPending(nullptr), "Partial character submission leaked a read");
    }
    load.BeginCharacter(0); nlShutdownFileSystem(); load.Poll();
    Check(load.State() == AnimationBundleState::Failed && load.Current() == previous, "File shutdown invalidated retained character data");
    nlInitFileSystem(); load.Unload(); CharacterData(previous, true);
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
        Check(argc == 4, "Supply image, data directory and synthetic/owned mode");
        const bool owned = std::string_view(argv[3]) == "owned", bad = std::string_view(argv[3]) == "bad-character";
        if (!owned && !bad) Decoder(argv[2]);
        { AnimationBundleLoad load; Reject([&] { load.Begin(Request()); }); }
        const auto folder = (std::filesystem::path(argv[2]) / "animation-bundle-data").string();
        std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged animation bundles"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Session session; const auto host = aurora_initialize(argc, argv, &config); session.live = true;
        Check(host.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot open animation disc"); session.disc = true; nlInitFileSystem();
        AnimationBundle::Handle retained;
        AnimationBundle::Handle retained_character;
        for (unsigned repeat = 0; repeat < 3; ++repeat)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            if (!owned && !bad) { Transactions(); Failures(); }
            if (!owned) Characters(bad);
            else
            {
                AnimationBundleLoad characters;
                for (unsigned index = 0; index < 20; ++index)
                { characters.BeginCharacter(index); Pump(characters, repeat % 2); retained_character = characters.Result(); CharacterData(retained_character, false); }
            }
            {
                AnimationBundleLoad load;
                for (const auto identity : owned ? std::array{0x04fb2f26u, 0xd625cf79u} : std::array{0xaaaau, 0xbbbbu})
                {
                    const AnimationBundleRequest request = owned ? AnimationBundleRequest{
                        "Art/fe/environments/main/gameworld.res.zlib", "Art/fe/environments/main/gameworld.tmp.zlib", identity} : Request(identity);
                    load.Begin(request); Pump(load, repeat % 2); retained = load.Result();
                    Inspect(retained, identity, owned && identity == 0x04fb2f26 ? 4 : 2);
                    if (owned) Check(retained->Hierarchy()->Data().GetNumNodes() == 4, "Owned FE rig changed");
                }
                load.Unload();
            }
            Check(StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b,
                "Animation bundle transaction failed to recover both arenas");
        }
        nlShutdownFileSystem(); ResetStartupMemory();
        Inspect(retained, owned ? 0xd625cf79 : 0xbbbb, 2);
        if (retained_character) CharacterData(retained_character, false);
        std::cout << checks << " animation bundle checks passed; retained direct-index rigs survive unload and arena shutdown\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
