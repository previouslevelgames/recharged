#include "runtime/frontend_resource_contexts.h"
#include "resources/frontend_layout.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/FE/FrontendResourceSteps.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks = 0;
void Check(bool yes, const char* message)
{ ++checks; if (!yes) throw std::runtime_error(message); }
template<class F> void Reject(F action, std::source_location at = std::source_location::current())
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid FE resource operation accepted at line " + std::to_string(at.line())); }
template<class F> void Pump(FrontendResourceContexts& owner, F done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!done())
    { owner.Service(); Check(std::chrono::steady_clock::now() < deadline, "FE resource/source admission timed out"); SDL_Delay(1); }
}
bool Linked(const GLResourcePool* pool)
{
    const auto* first = glGetResourcePools(); if (!first) return false;
    auto* p = first; do { if (p == pool) return true; p = p->m_next; } while (p != first); return false;
}
// Real original GL pools/texture slots without a GPU. There are no submitted
// frames in this CPU test. The deliberate drain fault observes retained teardown
// ownership; it does not qualify GPU references or a native presented frame.
struct Binding
{
    GLResourcePool* pool = nullptr;
    GLResourceMark mark = 0;
    std::vector<std::pair<const Texture*, PlatTexture*>> pages;
    std::shared_ptr<const Texture> texture;
    std::shared_ptr<const FrontendFont> font;
    bool released = false;
    explicit Binding(const char* name)
    {
        const GLMemoryRequirement requirement{GLM_Header, 16384};
        pool = glCreateResourcePool(&requirement, 1, name); mark = pool->MarkResource();
    }
    void Add(const Texture& source)
    {
        ValidateFrontendImageTexture(source);
        Require(glGetTextureIndex(source.id) == 0xffff, "Test resource hash is already registered");
        auto* native = new (pool->Allocate(sizeof(PlatTexture), GLM_Header)) PlatTexture;
        native->m_Width = source.width; native->m_Height = source.height;
        native->m_Levels = native->m_MaxLevel = source.levels;
        native->m_Format = static_cast<eGXTextureFormat>(source.game_format);
        native->m_nPaletteEntries = source.palette_entries;
        std::copy(source.bits.begin(), source.bits.end(), native->m_Bits);
        native->m_SwizzledData = const_cast<std::uint8_t*>(source.pixels.data());
        native->m_PaletteData = reinterpret_cast<u16*>(const_cast<std::uint8_t*>(source.palette.data()));
        native->m_NativeDataBytes = source.pixels.size(); native->m_NativePaletteBytes = source.palette.size();
        glRegisterTexture(source.id, native, pool); pages.emplace_back(&source, native);
    }
    void Validate() const
    {
        Require(!released && pool && Linked(pool) && pool->m_level == 1 && mark && glGetTextureManager(),
                "Actual context registration mark was retired");
        for (const auto& [source, native] : pages)
        {
            const auto index = native->m_TextureIndex;
            Require(index != 0xffff && index < glGetTextureManager()->mCapacity
                    && glGetTextureManager()->mTextures[index] == native && glGetTextureIndex(source->id) == index
                    && pool->m_inventory->GetTexture(source->id) == native
                    && native->m_SwizzledData == source->pixels.data()
                    && native->m_PaletteData == reinterpret_cast<const u16*>(source->palette.data())
                    && native->m_NativeDataBytes == source->pixels.size(),
                    "Actual registered font/texture slot or retained data differs");
        }
    }
    void Release()
    {
        if (released) return; Validate(); pool->ReleaseResource(mark); glDestroyResourcePool(pool);
        pool = nullptr; mark = 0; released = true;
    }
    ~Binding() { if (pool) { pool->ReleaseResource(mark); glDestroyResourcePool(pool); } }
};
struct Services
{
    std::vector<std::string> events;
    std::vector<std::weak_ptr<Binding>> bindings;
    bool mark_admit = true, texture_admit = true, font_admit = true, fail_release = false, corrupt_lease = false;
    unsigned marks = 0, textures = 0, fonts = 0, releases = 0;
    unsigned failure_after = 0;
    FrontendResourceContexts* reentrant = nullptr;
    std::shared_ptr<FrontendResourceLease> Lease(std::shared_ptr<Binding> binding, FrontendResourceLeaseKind kind)
    {
        auto lease = std::make_shared<FrontendResourceLease>(); lease->kind = kind;
        lease->registration = binding; lease->marker = binding->mark;
        lease->texture = binding->texture; lease->font = binding->font; bindings.push_back(binding);
        lease->validate = [binding] { binding->Validate(); };
        lease->release = [this, binding]
        {
            if (reentrant) { Reject([&] { reentrant->Poll(); }); Reject([&] { reentrant->Release(); }); }
            if (fail_release && releases >= failure_after) throw std::runtime_error("Injected actual registration drain failure");
            ++releases; binding->Release();
        };
        if (corrupt_lease) lease->validate = {};
        return lease;
    }
    FrontendResourceRegistrations Registrations()
    {
        FrontendResourceRegistrations result;
        result.mark_context = [this](auto scene)
        {
            if (!mark_admit) return FrontendResourceRegistrations::Lease{};
            events.emplace_back("mark:" + std::to_string(scene->token)); ++marks;
            return Lease(std::make_shared<Binding>("Real FE scene context"), FrontendResourceLeaseKind::Context);
        };
        result.texture = [this](auto scene, auto texture)
        {
            if (!texture_admit) return FrontendResourceRegistrations::Lease{};
            events.emplace_back("texture:" + std::to_string(scene->token)); ++textures;
            auto binding = std::make_shared<Binding>("Real retained FE texture"); binding->texture = texture; binding->Add(*texture);
            return Lease(binding, FrontendResourceLeaseKind::Texture);
        };
        result.font = [this](auto scene, auto font)
        {
            if (!font_admit) return FrontendResourceRegistrations::Lease{};
            events.emplace_back("font:" + std::to_string(scene->token)); ++fonts;
            auto binding = std::make_shared<Binding>("Real retained FE font pages"); binding->font = font;
            for (const auto& page : font->pages) binding->Add(page);
            return Lease(binding, FrontendResourceLeaseKind::Font);
        };
        return result;
    }
    void Clean() const { for (const auto& weak : bindings) Check(weak.expired(), "FE registration lease retained original pool/slots"); }
};
struct Creation
{
    Services& services;
    bool presentation = true, created = true, subhandlers = true, fail_created = false;
    unsigned presentations = 0, creations = 0, subs = 0;
    FrontendResourceSceneHandle retained;
    FrontendResourceSceneCallbacks Callbacks()
    {
        FrontendResourceSceneCallbacks callbacks;
        callbacks.original_handler_dependencies = {"GameInfo/SaveLoad/Mii/HBM are outside selected CPU callbacks"};
        callbacks.set_presentation = [this](auto scene)
        {
            Check(scene && scene->package, "SetPresentation preceded actual package arrival");
            if (!presentation) return FrontendResourceCreationAdmission{false, {"SetPresentation retained owner"}};
            retained = scene; ++presentations; services.events.emplace_back("presentation:" + std::to_string(scene->token));
            return FrontendResourceCreationAdmission{true, {}};
        };
        callbacks.scene_created = [this](auto scene)
        {
            Check(retained == scene && presentations, "SceneCreated preceded SetPresentation");
            if (fail_created) throw std::runtime_error("Injected concrete creation failure");
            if (!created) return FrontendResourceCreationAdmission{false, {"Actual selected handler service"}};
            ++creations; services.events.emplace_back("created:" + std::to_string(scene->token));
            return FrontendResourceCreationAdmission{true, {}};
        };
        callbacks.initialize_subhandlers = [this](auto scene)
        {
            Check(retained == scene && creations, "InitializeSubHandlers preceded SceneCreated");
            if (!subhandlers) return FrontendResourceCreationAdmission{false, {"Subhandler resource association"}};
            ++subs; services.events.emplace_back("subhandlers:" + std::to_string(scene->token));
            return FrontendResourceCreationAdmission{true, {}};
        };
        return callbacks;
    }
};
void ContextOnlySourceTurns()
{
    Services services; Creation first{services}, second{services};
    FrontendResourceContexts owner(services.Registrations()); services.reentrant = &owner;
    const auto a = owner.QueueScene("/Art/fe/empty.fen", first.Callbacks());
    Check(owner.SceneStatus(a).source_state == 1 && !owner.SceneStatus(a).active && owner.Status().discard_frame,
          "Source construction/queued push was guessed ready");
    owner.Poll();
    while (!owner.Scene(a)->package) { nlServiceFileSystem(); owner.Poll(); SDL_Delay(1); }
    Check(owner.SceneStatus(a).source_state == 5 && owner.SceneStatus(a).completion_callbacks == 0
          && owner.Status().queued_resources == 1 && owner.Status().current_context == a,
          "Context sentinel failed to retain original Waiting service turn");
    const auto identity = std::to_string(a);
    const std::vector<std::string> expected{"presentation:" + identity, "created:" + identity,
        "subhandlers:" + identity, "mark:" + identity};
    Check(services.events == expected, "Original early creation/context admission order changed");
    owner.Poll(); Check(owner.SceneStatus(a).source_state == 6 && owner.SceneStatus(a).completion_callbacks == 1
          && owner.Status().all_resource_scenes_valid && !owner.Status().current_context,
          "Queue-empty source completion did not execute exactly once");
    Check(!owner.SceneStatus(a).full_scene_created && !owner.SceneStatus(a).original_handler_dependencies.empty(),
          "Resource state6 fabricated full concrete handler readiness");
    const auto retained = owner.Scene(a); owner.QueuePop(a);
    Check(owner.Status().discard_frame && owner.Status().push_pop_messages == 1, "Queued pop failed original manager validity predicate");
    owner.Poll(); Reject([&] { owner.Scene(a); }); Check(retained->package->resources.empty(), "Retained FEN did not survive source pop");
    const auto b = owner.QueueScene("/Art/fe/empty.fen", second.Callbacks()); Pump(owner, [&] { return owner.SceneStatus(b).source_state == 6; });
    Check(b != a && first.presentations == 1 && first.creations == 1 && first.subs == 1,
          "Restart reused original callback token or replayed admitted creation");
    owner.Release(); services.Clean();
}
void OrderedResources(bool owned)
{
    Services services; Creation creation{services}; creation.created = false;
    FrontendResourceContexts owner(services.Registrations()); services.reentrant = &owner; owner.BeginMainResources();
    const auto token = owner.QueueScene(owned ? "/Art/fe/sms2_start.fen" : "/Art/fe/saved-valid.fen", creation.Callbacks());
    Pump(owner, [&] { return owner.SceneStatus(token).pending == FrontendResourcePending::SceneCreated; });
    Check(owner.SceneStatus(token).source_state == 5 && !owner.SceneStatus(token).valid_resources && services.marks == 0
          && owner.Status().queued_resources == owner.Scene(token)->package->resources.size() + 1,
          "Pending SceneCreated issued resources or fabricated state6");
    for (unsigned i = 0; i < 4; ++i) owner.Service();
    Check(creation.presentations == 1 && !creation.creations && !creation.subs && !services.marks,
          "Pending creation replayed admitted source calls");
    creation.created = true; creation.subhandlers = false; owner.Poll();
    Check(owner.SceneStatus(token).pending == FrontendResourcePending::InitializeSubHandlers && creation.creations == 1
          && !services.marks, "Subhandlers did not retain exact source stage");
    creation.subhandlers = true; services.mark_admit = false; owner.Poll();
    Check(owner.SceneStatus(token).pending == FrontendResourcePending::ContextRegistration
          && owner.SceneStatus(token).source_state == 5 && !services.marks,
          "Missing actual context registration became a successful marker");
    services.mark_admit = true; services.texture_admit = false; services.font_admit = false;
    Pump(owner, [&] { const auto pending = owner.SceneStatus(token).pending;
        return pending == FrontendResourcePending::TextureRegistration || pending == FrontendResourcePending::FontRegistration; });
    const auto old_resources = owner.Status().queued_resources;
    Check(owner.SceneStatus(token).source_state == 5 && old_resources && owner.Status().discard_frame,
          "Decoded assets established original resource readiness without GL registration");
    services.texture_admit = services.font_admit = true;
    Pump(owner, [&] { owner.Check(token); return owner.SceneStatus(token).source_state == 6; });
    const auto status = owner.SceneStatus(token);
    Check(status.resources && status.valid_resources == status.resources && status.completion_callbacks == 1
          && owner.Status().all_resource_scenes_valid && !status.full_scene_created,
          "Real resource completion or selected/full callback boundary differs");
    Check(creation.presentations == 1 && creation.creations == 1 && creation.subs == 1,
          "Successful admission repeated source creation operations");
    auto frame = owner.Scene(token); const auto old_marks = services.marks, old_textures = services.textures, old_fonts = services.fonts;
    Creation second{services}; const auto other = owner.QueueScene(frame->path, second.Callbacks());
    Check(owner.Status().discard_frame, "Second queued source scene failed whole-manager discard rule");
    Pump(owner, [&] { owner.Check(other); return owner.SceneStatus(other).source_state == 6; });
    Check(services.marks == old_marks + 1 && services.textures == old_textures && services.fonts == old_fonts,
          "Shared real font/image leases reread or re-registered resources");
    const auto before_slots = glGetTextureManager()->mFreeIndices->mCount;
    owner.QueuePop(token); owner.Poll();
    Check(owner.SceneStatus(other).source_state == 6 && owner.Status().all_resource_scenes_valid
          && glGetTextureManager()->mFreeIndices->mCount == before_slots,
          "First donor pop retired another scene's real shared texture/font leases");
    services.fail_release = true; owner.QueuePop(other); Reject([&] { owner.Poll(); });
    Check(owner.SceneStatus(other).retiring && owner.Status().discard_frame && owner.Status().push_pop_messages == 1,
          "Failed registration drain discarded exact retained queue/owner");
    services.fail_release = false; owner.Poll(); Reject([&] { owner.Scene(other); });
    owner.Release(); Check(frame->package && !frame->package->resources.empty(), "Host FEN ownership did not survive graphics teardown"); services.Clean();
}
void OverlappingContexts()
{
    Services services; Creation first{services}, second{services}; second.presentation = false;
    FrontendResourceContexts owner(services.Registrations());
    const auto a = owner.QueueScene("/Art/fe/empty.fen", first.Callbacks());
    const auto b = owner.QueueScene("/Art/fe/empty.fen", second.Callbacks()); owner.Poll();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    // Stage both genuine NL completions before native orchestration. This
    // deterministically leaves two source contexts in the same resource queue.
    while (!owner.Scene(a)->package || !owner.Scene(b)->package)
    { nlServiceFileSystem(); Check(std::chrono::steady_clock::now() < deadline, "Overlapping source package reads timed out"); SDL_Delay(1); }
    owner.Poll(); owner.Poll();
    Check(owner.SceneStatus(a).source_state == 5 && !owner.SceneStatus(a).completion_callbacks
          && owner.SceneStatus(b).source_state == 5 && owner.Status().current_context == a
          && owner.Status().queued_resources == 1 && owner.Status().discard_frame,
          "Pending next creation bypassed the source context-switch completion gate");
    second.presentation = true; services.mark_admit = false; owner.Poll();
    Check(owner.SceneStatus(a).source_state == 6 && owner.SceneStatus(a).completion_callbacks == 1
          && owner.SceneStatus(b).source_state == 5 && owner.SceneStatus(b).pending == FrontendResourcePending::ContextRegistration,
          "Original previous-context completion did not precede the new mark admission");
    for (unsigned i = 0; i < 3; ++i) owner.Poll();
    Check(owner.SceneStatus(a).completion_callbacks == 1 && services.marks == 1,
          "Missing next context mark replayed an already-admitted previous completion");
    services.mark_admit = true; owner.Poll();
    Check(owner.SceneStatus(b).source_state == 6 && owner.SceneStatus(b).completion_callbacks == 1
          && owner.SceneStatus(a).completion_callbacks == 1 && owner.Status().all_resource_scenes_valid,
          "Final overlapping queue completion or manager-wide validity differs");
    owner.Release(); services.Clean();
}
void PendingAndFailures()
{
    {
        FrontendResourceContexts owner({}); const auto a = owner.QueueScene("/Art/fe/empty.fen");
        Pump(owner, [&] { return owner.SceneStatus(a).pending == FrontendResourcePending::SetPresentation; });
        Check(owner.SceneStatus(a).source_state == 5 && !owner.SceneStatus(a).completion_callbacks
              && owner.Status().queued_resources == 1, "Missing default creation callback fabricated success");
        owner.Release();
    }
    {
        Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations());
        const auto a = owner.QueueScene("/Art/fe/embedded.fen", creation.Callbacks());
        Pump(owner, [&] { return owner.SceneStatus(a).pending == FrontendResourcePending::EmbeddedContext; });
        Check(owner.SceneStatus(a).source_state == 5 && !owner.SceneStatus(a).completion_callbacks && owner.Status().discard_frame,
              "Embedded null-scene context was dereferenced or fabricated complete");
        owner.Release(); services.Clean();
    }
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations());
        if (mode != 0) owner.BeginMainResources();
        if (mode == 3) services.corrupt_lease = true;
        const char* path = mode == 0 ? "/Art/fe/bad.fen" : mode == 1 ? "/Art/fe/missing-image.fen" :
                           mode == 2 ? "/Art/fe/dynamic.fen" : "/Art/fe/session.fen";
        const auto token = owner.QueueScene(path, creation.Callbacks());
        Pump(owner, [&] { return owner.SceneStatus(token).failed || owner.SceneStatus(token).pending == FrontendResourcePending::DynamicTexture; });
        Check(owner.SceneStatus(token).source_state != 6 && owner.Status().discard_frame
              && !owner.SceneStatus(token).completion_callbacks, "Missing/malformed resource or invalid lease became state6");
        if (mode != 2) Reject([&] { owner.Check(token); });
        else Check(!owner.SceneStatus(token).failed, "Unselected dynamic resource was misreported as a malformed file");
        owner.Release(); services.Clean();
    }
    {
        Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations());
        creation.fail_created = true; const auto a = owner.QueueScene("/Art/fe/empty.fen", creation.Callbacks());
        Pump(owner, [&] { return owner.SceneStatus(a).failed; });
        Check(owner.SceneStatus(a).source_state == 5 && services.marks == 0, "Failed creation issued queued resources");
        Reject([&] { owner.Check(a); }); owner.Release(); services.Clean();
    }
    {
        Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations());
        const auto a = owner.QueueScene("/Art/fe/empty.fen", creation.Callbacks()); owner.Poll();
        const auto retained = owner.Scene(a); owner.QueuePop(a); owner.Poll();
        Check(!creation.presentations && !nlAsyncReadsPending(nullptr), "Cancelled source FEN delivered stale creation/readiness");
        Reject([&] { owner.Scene(a); });
        Creation next{services}; const auto b = owner.QueueScene("/Art/fe/empty.fen", next.Callbacks());
        Pump(owner, [&] { return owner.SceneStatus(b).source_state == 6; });
        Check(b != a && !retained->package && next.presentations == 1, "New source token received cancelled callback context");
        bool thread_rejected = false; std::thread worker([&] { try { owner.QueuePop(b); } catch (const std::logic_error&) { thread_rejected = true; } });
        worker.join(); Check(thread_rejected, "Foreign thread mutated original context owner");
        owner.Release(); services.Clean();
    }
    {
        Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations()); owner.BeginMainResources();
        const auto a = owner.QueueScene("/Art/fe/fallback.fen", creation.Callbacks());
        Pump(owner, [&] { owner.Check(a); return owner.SceneStatus(a).source_state == 6; });
        Check(services.fonts == 1 && services.textures == 0 && owner.SceneStatus(a).completion_callbacks == 1,
              "Original first-registered font fallback did not bind a real font/page lease");
        Creation second{services}; const auto b = owner.QueueScene("/Art/fe/fallback2.fen", second.Callbacks());
        Pump(owner, [&] { owner.Check(b); return owner.SceneStatus(b).source_state == 6; });
        Check(services.fonts == 1 && owner.SceneStatus(b).valid_resources == 1,
              "Two original missing aliases re-registered the same fallback font pages");
        owner.Release(); services.Clean();
    }
}
void RegistrationInvalidationAndPartialRelease()
{
    {
        Services a_services, b_services; Creation a_creation{a_services}, b_creation{b_services};
        FrontendResourceContexts a(a_services.Registrations()), b(b_services.Registrations());
        const auto first = a.QueueScene("/Art/fe/empty.fen", a_creation.Callbacks());
        const auto second = b.QueueScene("/Art/fe/empty.fen", b_creation.Callbacks());
        Check(first != second, "Independent owners reused a stale scene-token identity");
        Reject([&] { a.QueuePop(second); }); Reject([&] { b.SceneStatus(first); });
        a.Release(); b.Release(); a_services.Clean(); b_services.Clean();
    }
    {
        Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations()); owner.BeginMainResources();
        Pump(owner, [&] { return owner.Status().main_assets_ready; });
        const auto token = owner.QueueScene("/Art/fe/session.fen", creation.Callbacks()); owner.Poll();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!owner.Scene(token)->package)
        { nlServiceFileSystem(); Check(std::chrono::steady_clock::now() < deadline, "Validation source FEN timed out"); SDL_Delay(1); }
        owner.Poll(); owner.Poll();
        Check(owner.SceneStatus(token).source_state == 5 && owner.SceneStatus(token).valid_resources == 1
              && owner.Status().queued_resources == 1, "Actual final texture did not retain original Waiting turn");
        auto binding = services.bindings.back().lock(); Check(binding && binding->texture && binding->pages.size() == 1,
              "Actual texture registration owner is absent");
        auto* texture = binding->pages[0].second; const auto index = texture->m_TextureIndex;
        glGetTextureManager()->mTextures[index] = nullptr; owner.Poll();
        Check(owner.SceneStatus(token).failed && owner.SceneStatus(token).source_state == 5
              && !owner.SceneStatus(token).completion_callbacks && owner.Status().discard_frame,
              "Invalidated actual texture slot passed queue-empty state6 admission");
        Reject([&] { owner.Check(token); }); glGetTextureManager()->mTextures[index] = texture;
        binding.reset(); owner.Release(); services.Clean();
    }
    {
        Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations()); owner.BeginMainResources();
        const auto token = owner.QueueScene("/Art/fe/pair.fen", creation.Callbacks());
        Pump(owner, [&] { owner.Check(token); return owner.SceneStatus(token).source_state == 6; });
        Check(owner.SceneStatus(token).valid_resources == 2 && services.textures == 1 && services.fonts == 1,
              "Hidden full resource ring did not bind real texture and font pages");
        const auto slots = glGetTextureManager()->mFreeIndices->mCount;
        services.fail_release = true; services.failure_after = 1; owner.QueuePop(token); Reject([&] { owner.Service(); });
        Check(services.releases == 1 && owner.SceneStatus(token).valid_resources == 1
              && glGetTextureManager()->mFreeIndices->mCount == slots + 1 && owner.Status().discard_frame,
              "Partial failed release replayed work or dropped its remaining real font/context leases");
        services.fail_release = false; owner.Poll();
        Check(services.releases == 3 && owner.Status().scenes == 0 && !owner.Status().push_pop_messages,
              "Retried partial release repeated an already-disposed registration");
        owner.Release(); services.Clean();
    }
}
struct Host
{
    bool live = false, disc = false;
    ~Host() { if (live) ResetStartupFiles(); if (disc) aurora_dvd_close(); if (live) { ResetStartupMemory(); aurora_shutdown(); } }
};
}
int main(int argc, char** argv)
{
    try
    {
        Check(argc == 4, "Supply ISO/RVZ, output directory and mode"); const std::string mode = argv[3];
        const auto folder = (std::filesystem::path(argv[2]) / "resource-runtime-data").string(); std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Original FE resource contexts"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL; config.windowWidth = 320; config.windowHeight = 240;
        config.windowPosX = config.windowPosY = -1; config.logLevel = LOG_WARNING; config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Host host; const auto state = aurora_initialize(argc, argv, &config); host.live = true; Check(state.window, "Aurora core initialization failed");
        InitializeStartupOS(); nlInitMemory(); Check(aurora_dvd_open(argv[1]), "Cannot open FE resource test disc"); host.disc = true; nlInitFileSystem();
        for (unsigned repeat = 0; repeat < 2; ++repeat)
        {
            const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
            const GLMemoryRequirement requirement{GLM_Header, 32768}; const GLMemoryConfig memory{65536, 65536, &requirement, 1, 512};
            glInitResourcePools(); Check(glInitMemory(&memory), "Original GL memory did not initialize"); InitializeOriginalGraphicsState();
            const auto slots = glGetTextureManager()->mFreeIndices->mCount;
            if (mode == "success") { ContextOnlySourceTurns(); OverlappingContexts(); OrderedResources(false);
                PendingAndFailures(); RegistrationInvalidationAndPartialRelease(); }
            else if (mode == "owned") OrderedResources(true);
            else
            {
                Services services; Creation creation{services}; FrontendResourceContexts owner(services.Registrations());
                bool started = false;
                try { owner.BeginMainResources(); started = true; }
                catch (const std::exception&) { Check(mode == "missing-loc" || mode == "short-images", "Unexpected synchronous asset admission failure"); }
                if (started)
                {
                    const auto a = owner.QueueScene("/Art/fe/session.fen", creation.Callbacks());
                    Pump(owner, [&] { return owner.Status().main_assets_failed; });
                    for (unsigned i = 0; i < 4; ++i) owner.Service();
                    Check(owner.SceneStatus(a).source_state != 6 && owner.Status().discard_frame,
                          "Malformed font/image asset established source scene readiness");
                }
                owner.Release(); services.Clean();
            }
            Check(glGetTextureManager()->mFreeIndices->mCount == slots, "FE resource context teardown leaked real texture slots");
            glShutdownMemory(); Check(!nlAsyncReadsPending(nullptr), "FE resource source owner retained NL work");
            Check(StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2,
                  "Repeated FE context lifecycle leaked native arenas");
        }
        std::cout << checks << " original FE resource-context checks passed (" << mode << ")\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
