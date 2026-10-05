#include "runtime/frontend_resource_contexts.h"
#include "resources/frontend_layout.h"
#include "runtime/whole_file.h"
#include "Game/FE/FrontendResourceSteps.h"
#include "Game/FE/FrontendStackSteps.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <exception>
#include <map>
#include <limits>
#include <set>
#include <thread>

namespace mscharged
{
namespace
{
using resources::Require;
using Token = FrontendResourceSceneToken;
using Lease = FrontendResourceRegistrations::Lease;
struct PendingCreation { };
struct PendingRegistration { };
std::atomic<Token> next_identity{1};
Token NewIdentity()
{
    auto candidate = next_identity.load(std::memory_order_relaxed);
    while (candidate && candidate < std::numeric_limits<Token>::max())
        if (next_identity.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
            return candidate;
    throw std::overflow_error("Frontend resource scene identity space exhausted");
}
void Validate(const Lease& lease, FrontendResourceLeaseKind kind)
{
    Require(lease && lease->kind == kind && lease->registration && lease->validate && lease->release,
            "Frontend resources require retained registration, validation and release leases");
    lease->validate();
}
}
struct FrontendResourceContexts::Implementation
{
    struct Entry;
    struct Loaded
    {
        Lease lease;
        unsigned references = 0;
        bool released = false;
    };
    struct Handle
    {
        Entry* scene = nullptr;
        std::uint32_t type = 0, hash = 0, offset = 0;
        bool m_bValid = false;
        Loaded* loaded = nullptr;
        std::uint32_t binding_hash = 0;
        bool IsValid() const { return m_bValid; }
    };
    struct Context : Handle
    {
        Entry* m_pFESceneContext = nullptr;
        std::uint64_t m_glResourceMarker = 0;
        Lease lease;
        Context() { type = 2; }
    };
    struct Entry
    {
        Implementation& owner;
        Token token;
        FrontendResourceSceneCallbacks callbacks;
        std::shared_ptr<FrontendResourceSceneView> view;
        FrontendResourceSceneStatus status;
        int mState = 1;
        std::vector<Handle> handles;
        Context context;
        unsigned read_token = 0, completion_order = 0;
        std::size_t size = 0;
        bool read_completed = false, queued = false;
        std::exception_ptr error;
        Entry(Implementation& o, Token t, std::string path, FrontendResourceSceneCallbacks cb)
            : owner(o), token(t), callbacks(std::move(cb)), view(std::make_shared<FrontendResourceSceneView>())
        {
            view->token = token; view->path = std::move(path); status.token = token;
            status.original_handler_dependencies = callbacks.original_handler_dependencies;
            context.scene = context.m_pFESceneContext = this;
        }
        void AllResourcesLoadedCallback()
        {
            // The real source body is empty. Counting observes the genuine
            // completion operation; it supplies no unavailable handler service.
            FrontendSceneResourcesLoadedCallback();
            ++status.completion_callbacks;
            status.pending = FrontendResourcePending::None;
            status.missing.clear();
        }
        void CancelRead()
        {
            if (read_token) nlCancelEntireFileLoad(read_token, nullptr);
            read_token = 0;
        }
    };
    struct Message { Token token; bool pop; };
    const std::thread::id thread = std::this_thread::get_id();
    bool busy = false, released = false;
    unsigned completion_sequence = 0;
    FrontendResourceRegistrations registrations;
    std::map<Token, std::unique_ptr<Entry>> entries;
    std::deque<Message> messages;
    std::deque<Handle*> queue;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Loaded> loaded;
    Handle* current_resource = nullptr;
    Context* current_context = nullptr;
    std::unique_ptr<FrontendVisualLoad> visual_load;
    std::unique_ptr<FrontendImageLoad> image_load;
    std::shared_ptr<const FrontendVisualAssets> visuals;
    resources::FrontendImageCatalog::Handle images;
    std::exception_ptr assets_error;
    explicit Implementation(FrontendResourceRegistrations input) : registrations(std::move(input)) { }
    void Thread() const
    {
        if (thread != std::this_thread::get_id() || busy || nlGetCurrentAsyncRead())
            throw std::logic_error("Frontend resources require their nonrecursive NL owner thread");
    }
    void Mutable() const { Thread(); if (released) throw std::logic_error("Frontend resource owner was released"); }
    Entry& Find(Token token) const
    {
        const auto item = entries.find(token);
        if (!token || item == entries.end()) throw std::out_of_range("Unknown frontend resource scene token");
        return *item->second;
    }
    void Start(Entry& e)
    {
        Require(gMemoryInitialized && nlFileSystemReady(), "Frontend FEN requires native memory and NL files");
        std::unique_ptr<nlFile> file(nlOpen(e.view->path.c_str()));
        if (!file) throw std::runtime_error("Frontend resource scene is missing: " + e.view->path);
        e.size = nlFileSize(file.get(), nullptr);
        Require(e.size >= 16 && e.size <= resources::MaximumAssetBytes, "Frontend FEN exceeds its checked asset limits");
        e.status.active = true;
        e.read_token = nlLoadEntireFileAsync(e.view->path.c_str(), Complete, &e, 32, AllocateEnd, nullptr, 0, &VirtualAllocator);
        if (!e.read_token && !e.read_completed) throw std::runtime_error("Frontend FEN read was not queued");
    }
    static void Complete(void* data, unsigned long size, void* context)
    {
        std::unique_ptr<void, void(*)(void*)> buffer(data, nlFree);
        auto& e = *static_cast<Entry*>(context);
        e.read_token = 0; e.read_completed = true;
        e.completion_order = ++e.owner.completion_sequence;
        try
        {
            Require(e.owner.thread == std::this_thread::get_id() && size == e.size,
                    "Frontend FEN callback owner or byte count differs");
            e.view->package = std::make_shared<resources::FrontendScene>(resources::ReadFrontendScene(
                    {static_cast<const std::uint8_t*>(data), size}));
        }
        catch (...) { e.error = std::current_exception(); }
    }
    void Fail(Entry& e, std::exception_ptr failure)
    {
        if (!e.error) e.error = failure;
        e.status.failed = true;
        e.CancelRead();
    }
    void SetPending(Entry& e, FrontendResourcePending kind, std::vector<std::string> missing)
    { e.status.pending = kind; e.status.missing = std::move(missing); }
    void StageCreation(Entry& e)
    {
        struct Adapter
        {
            Implementation& owner; Entry& e;
            void Admit(FrontendResourceCreationStage stage, const FrontendResourceSceneCallbacks::Function& function,
                       FrontendResourcePending pending, const char* name)
            {
                if (e.status.creation > stage) return; // Native pending retry never repeats admitted source calls.
                FrontendResourceCreationAdmission result;
                if (function) result = function(e.view);
                if (!result.admitted)
                {
                    if (result.missing.empty()) result.missing.emplace_back(name);
                    owner.SetPending(e, pending, std::move(result.missing));
                    throw PendingCreation{};
                }
                e.status.creation = static_cast<FrontendResourceCreationStage>(static_cast<unsigned>(stage) + 1);
                e.status.pending = FrontendResourcePending::None; e.status.missing.clear();
            }
            void SetPresentation(FrontendResourceSceneHandle)
            { Admit(FrontendResourceCreationStage::SetPresentation, e.callbacks.set_presentation,
                    FrontendResourcePending::SetPresentation, "SetPresentation provider"); }
            void SceneCreated()
            { Admit(FrontendResourceCreationStage::SceneCreated, e.callbacks.scene_created,
                    FrontendResourcePending::SceneCreated, "Concrete SceneCreated services"); }
            void InitializeSubHandlers()
            { Admit(FrontendResourceCreationStage::InitializeSubHandlers, e.callbacks.initialize_subhandlers,
                    FrontendResourcePending::InitializeSubHandlers, "InitializeSubHandlers provider"); }
        } adapter{*this, e};
        FrontendInitializeScene(adapter, FrontendResourceSceneHandle(e.view));
    }
    void QueuePackage(Entry& e)
    {
        Require(e.view->package && e.view->package->resources.size() <= 16384,
                "Frontend resource ring exceeds its native limits");
        Require(queue.size() + e.view->package->resources.size() + 1 <= 32768,
                "Frontend queued resources exceed the native limit");
        e.handles.reserve(e.view->package->resources.size());
        for (const auto& r : e.view->package->resources)
            e.handles.push_back({&e, r.type, r.hash, r.offset, false, nullptr});
        // Allocate the whole successor queue before source state/queue mutation.
        auto next = queue; next.push_back(&e.context);
        for (auto& h : e.handles) next.push_back(&h);
        e.status.resources = e.handles.size();
        FrontendScenePackageResources(e, [&] { queue.swap(next); e.queued = true; }, [&] { StageCreation(e); });
    }
    void PollPackages()
    {
        std::vector<Entry*> completed;
        for (auto& [token, pointer] : entries)
        {
            auto& e = *pointer;
            if (!e.status.active || e.status.retiring || e.status.failed) continue;
            if (e.error) { Fail(e, e.error); continue; }
            if (e.mState == 1 && !e.read_completed && !WholeFileLoadPending(e.read_token))
            { Fail(e, std::make_exception_ptr(std::runtime_error("Frontend FEN read failed or file services stopped"))); continue; }
            if (e.mState == 1 && e.read_completed) completed.push_back(&e);
        }
        std::sort(completed.begin(), completed.end(), [](const auto* a, const auto* b) { return a->completion_order < b->completion_order; });
        for (auto* e : completed)
        {
            try { QueuePackage(*e); }
            catch (const PendingCreation&) { }
            catch (...) { Fail(*e, std::current_exception()); }
        }
        for (auto& [token, pointer] : entries)
        {
            auto& e = *pointer;
            if (!e.queued || e.status.failed || e.status.retiring || e.status.creation == FrontendResourceCreationStage::Complete
                || std::find(completed.begin(), completed.end(), &e) != completed.end()) continue;
            try { StageCreation(e); }
            catch (const PendingCreation&) { }
            catch (...) { Fail(e, std::current_exception()); }
        }
    }
    void PollAssets()
    {
        if (assets_error || visuals || !visual_load) return;
        try
        {
            visual_load->Poll(); image_load->Poll();
            if (visual_load->Ready()) (void)visual_load->Result();
            if (image_load->State() == FrontendImageState::Failed) (void)image_load->Result();
            if (!visual_load->Ready() || image_load->State() != FrontendImageState::Ready) return;
            visuals = visual_load->Result(); images = image_load->Result();
            Require(visuals && visuals->localization && !visuals->font_registration_order.empty() && images,
                    "Frontend Main resources lack actual localization/font/page/image completion");
        }
        catch (...) { assets_error = std::current_exception(); visual_load->Cancel(); image_load->Cancel(); }
    }
    std::uint64_t MarkResource(Context* context)
    {
        auto& e = *context->scene;
        if (!context->lease)
        {
            if (registrations.mark_context) context->lease = registrations.mark_context(e.view);
            if (!context->lease)
            { SetPending(e, FrontendResourcePending::ContextRegistration, {"Real GL resource-pool mark"}); throw PendingRegistration{}; }
        }
        Validate(context->lease, FrontendResourceLeaseKind::Context);
        Require(context->lease->marker, "Frontend context mark did not establish a real marker");
        e.status.pending = FrontendResourcePending::None; e.status.missing.clear();
        return context->lease->marker;
    }
    Context*& CurrentContext() { return current_context; }
    Context* PermanentContext() { return nullptr; } // Null-scene/permanent operations are explicitly unselected.
    Handle*& CurrentResource() { return current_resource; }
    bool Empty() const { return queue.empty(); }
    void RemoveFirst() { Require(!queue.empty() && queue.front() == current_resource, "Frontend resource queue identity changed"); queue.pop_front(); }
    void CompleteContext(Context* context)
    {
        Require(context && context->m_pFESceneContext && !context->scene->status.failed
                && context->scene->status.creation == FrontendResourceCreationStage::Complete,
                "Frontend context cannot complete a missing or failed scene/creation callback");
        for (const auto& h : context->scene->handles)
        {
            Require(h.m_bValid && h.loaded, "Frontend context completed an invalid resource");
            Validate(h.loaded->lease, h.type == 0 ? FrontendResourceLeaseKind::Texture : FrontendResourceLeaseKind::Font);
        }
        Validate(context->lease, FrontendResourceLeaseKind::Context);
        // A native missing mark may retry after the previous source completion.
        // Retain that exact already-admitted completion instead of replaying it.
        if (context->scene->mState != 6) FrontendCompleteResourceContext(context);
    }
    bool BindResource(Handle& h)
    {
        auto& e = *h.scene;
        if (h.type == 2)
        { SetPending(e, FrontendResourcePending::EmbeddedContext, {"Embedded/null-scene FERT_SCENE provider"}); return false; }
        if (h.type == 0 && resources::IsDynamicFrontendImage(h.hash))
        { SetPending(e, FrontendResourcePending::DynamicTexture, {"Actual dynamic texture producer/registration"}); return false; }
        if (assets_error) std::rethrow_exception(assets_error);
        if (!visuals || !images)
        { SetPending(e, FrontendResourcePending::Assets, {"Actual Main font/page/localization/permanent image loads"}); return false; }
        std::shared_ptr<const resources::FrontendFont> font;
        if (h.type == 1)
        {
            font = resources::FindFrontendFont(visuals->font_registration_order, h.hash, true);
            Require(bool(font), "Original font fallback has no registered font");
        }
        // Original aliases may fall back to the same first-registered font.
        // Retain one actual page registration, irrespective of requested alias.
        h.binding_hash = font ? font->alias : h.hash;
        const auto key = std::pair{h.type, h.binding_hash};
        auto old = loaded.find(key);
        if (old != loaded.end())
        {
            Validate(old->second.lease, h.type == 0 ? FrontendResourceLeaseKind::Texture : FrontendResourceLeaseKind::Font);
            h.loaded = &old->second; ++h.loaded->references; h.m_bValid = true;
            e.status.pending = FrontendResourcePending::None; e.status.missing.clear(); return true;
        }
        Lease lease;
        std::shared_ptr<const resources::Texture> texture;
        if (h.type == 0)
        {
            const auto found = images->textures.find(h.hash);
            Require(found != images->textures.end() && found->second && found->second->id == h.hash,
                    "Frontend texture is absent from actual Main permanent resources");
            texture = found->second; resources::ValidateFrontendImageTexture(*texture);
            if (registrations.texture) lease = registrations.texture(e.view, texture);
        }
        else if (h.type == 1)
        {
            if (registrations.font) lease = registrations.font(e.view, font);
        }
        else throw std::runtime_error("Unknown frontend queued resource type");
        if (!lease)
        {
            SetPending(e, h.type == 0 ? FrontendResourcePending::TextureRegistration : FrontendResourcePending::FontRegistration,
                       {h.type == 0 ? "Real texture GL registration" : "Real retained font/page registration"});
            return false;
        }
        // Keep admitted registrations before validation can throw, so teardown
        // always owns partial/failed providers and can retry a genuine release.
        auto [item, inserted] = loaded.emplace(key, Loaded{lease, 1, false});
        Require(inserted, "Frontend registration collided during nonrecursive admission");
        h.loaded = &item->second;
        Validate(lease, h.type == 0 ? FrontendResourceLeaseKind::Texture : FrontendResourceLeaseKind::Font);
        Require(h.type == 0 ? lease->texture == texture : lease->font == font,
                "Frontend registration did not retain the exact decoded resource");
        h.m_bValid = true; e.status.pending = FrontendResourcePending::None; e.status.missing.clear();
        // A new texture retains original Waiting scheduling even if its checked
        // permanent registration completes synchronously. Font is AlreadyLoaded.
        return h.type == 1;
    }
    bool Issue(Handle& h)
    {
        try
        {
            if (&h == &h.scene->context)
            { FrontendIssueResourceContext(*this, &h.scene->context); return false; }
            return BindResource(h);
        }
        catch (const PendingRegistration&) { return false; }
        catch (...) { Fail(*h.scene, std::current_exception()); return false; }
    }
    bool IssueFirstAlreadyLoaded()
    {
        Require(!queue.empty(), "Frontend resource issue requires a queue head");
        const auto& e = *queue.front()->scene;
        if (e.status.failed || e.status.retiring || e.status.creation != FrontendResourceCreationStage::Complete)
            return false;
        current_resource = queue.front(); return Issue(*current_resource);
    }
    void UpdateResources()
    {
        if (!messages.empty() || queue.empty()) return;
        const auto& front = *queue.front()->scene;
        if (front.status.failed || front.status.retiring || front.status.creation != FrontendResourceCreationStage::Complete) return;
        if (current_resource && !current_resource->m_bValid) Issue(*current_resource);
        if (current_resource && current_resource->m_bValid)
        {
            try
            {
                if (current_resource == &current_resource->scene->context)
                    Validate(current_resource->scene->context.lease, FrontendResourceLeaseKind::Context);
                else
                    Validate(current_resource->loaded->lease, current_resource->type == 0 ? FrontendResourceLeaseKind::Texture : FrontendResourceLeaseKind::Font);
            }
            catch (...) { Fail(*current_resource->scene, std::current_exception()); return; }
        }
        FrontendResourceQueueUpdate(*this);
    }
    void Retire(Entry& e)
    {
        e.status.retiring = true; SetPending(e, FrontendResourcePending::Release, {"Actual registration/GPU drain and release"});
        e.CancelRead();
        for (auto& h : e.handles) if (h.loaded)
        {
            auto* registration = h.loaded;
            if (registration->references == 1 && !registration->released)
            { registration->lease->release(); registration->released = true; }
            Require(registration->references, "Frontend borrowed registration count underflow");
            if (--registration->references == 0) loaded.erase({h.type, h.binding_hash});
            h.loaded = nullptr; h.m_bValid = false;
        }
        if (e.context.lease)
        { e.context.lease->release(); e.context.lease.reset(); }
        if (current_resource && current_resource->scene == &e) current_resource = nullptr;
        if (current_context && current_context->scene == &e) current_context = nullptr;
        std::erase_if(queue, [&](const auto* h) { return h->scene == &e; });
    }
    void ProcessMessages()
    {
        while (!messages.empty())
        {
            const auto message = messages.front(); auto& e = Find(message.token);
            if (message.pop) { Retire(e); entries.erase(message.token); }
            else try { Start(e); } catch (...) { Fail(e, std::current_exception()); }
            messages.pop_front();
        }
    }
    void Poll()
    {
        ProcessMessages(); PollAssets(); PollPackages(); UpdateResources();
    }
};
FrontendResourceContexts::FrontendResourceContexts(FrontendResourceRegistrations input)
    : impl_(std::make_unique<Implementation>(std::move(input))) { }
FrontendResourceContexts::~FrontendResourceContexts()
{ try { Release(); } catch (...) { std::terminate(); } }
void FrontendResourceContexts::BeginMainResources(FrontendLanguage language)
{
    auto& s = *impl_; s.Mutable();
    Require(s.entries.empty() && s.messages.empty() && !s.visual_load, "Frontend Main resources require a fresh scene-free owner");
    s.visual_load = std::make_unique<FrontendVisualLoad>(language);
    try { s.image_load = std::make_unique<FrontendImageLoad>(); s.image_load->BeginPermanentMain(); }
    catch (...) { s.visual_load->Cancel(); s.visual_load.reset(); s.image_load.reset(); throw; }
}
FrontendResourceSceneToken FrontendResourceContexts::QueueScene(std::string path, FrontendResourceSceneCallbacks callbacks)
{
    auto& s = *impl_; s.Mutable();
    Require(!path.empty() && path.front() == '/' && path.size() <= 4096 && path.find('\0') == std::string::npos,
            "Invalid frontend FEN path");
    Require(s.entries.size() < 32, "Frontend scene ownership limit exhausted");
    const auto token = NewIdentity();
    auto e = std::make_unique<Implementation::Entry>(s, token, std::move(path), std::move(callbacks));
    s.messages.push_back({token, false});
    try { s.entries.emplace(token, std::move(e)); }
    catch (...) { s.messages.pop_back(); throw; }
    return token;
}
void FrontendResourceContexts::QueuePop(Token token)
{
    auto& s = *impl_; s.Mutable(); (void)s.Find(token);
    if (std::any_of(s.messages.begin(), s.messages.end(), [&](const auto& message) { return message.token == token && message.pop; })) return;
    s.messages.push_back({token, true});
}
void FrontendResourceContexts::Poll()
{
    auto& s = *impl_; s.Mutable(); s.busy = true;
    struct Leave { bool& busy; ~Leave() { busy = false; } } leave{s.busy}; s.Poll();
}
void FrontendResourceContexts::Service()
{
    auto& s = *impl_; s.Mutable(); s.busy = true;
    struct Leave { bool& busy; ~Leave() { busy = false; } } leave{s.busy};
    s.ProcessMessages();
    try { nlServiceFileSystem(); }
    catch (...)
    {
        for (auto& [token, e] : s.entries) if (e->status.active && e->mState != 6) s.Fail(*e, std::current_exception());
        throw;
    }
    s.Poll();
}
FrontendResourceSceneStatus FrontendResourceContexts::SceneStatus(Token token) const
{
    auto& s = *impl_; s.Thread(); const auto& e = s.Find(token); auto status = e.status;
    status.source_state = e.mState;
    status.valid_resources = std::count_if(e.handles.begin(), e.handles.end(), [](const auto& h) { return h.m_bValid; });
    return status;
}
FrontendResourceSceneHandle FrontendResourceContexts::Scene(Token token) const
{ auto& s = *impl_; s.Thread(); return s.Find(token).view; }
FrontendResourceContextStatus FrontendResourceContexts::Status() const
{
    auto& s = *impl_; s.Thread(); FrontendResourceContextStatus status;
    status.scenes = s.entries.size(); status.push_pop_messages = s.messages.size(); status.queued_resources = s.queue.size();
    status.current_context = s.current_context ? s.current_context->scene->token : 0;
    status.main_assets_ready = bool(s.visuals); status.main_assets_failed = bool(s.assets_error);
    std::vector<const Implementation::Entry*> active;
    for (const auto& [token, e] : s.entries)
    {
        if (e->status.failed || e->status.retiring) { status.discard_frame = true; return status; }
        if (e->status.active) active.push_back(e.get());
        if (e->mState == 6)
        {
            Validate(e->context.lease, FrontendResourceLeaseKind::Context);
            for (const auto& h : e->handles) if (h.loaded)
                Validate(h.loaded->lease, h.type == 0 ? FrontendResourceLeaseKind::Texture : FrontendResourceLeaseKind::Font);
        }
    }
    status.all_resource_scenes_valid = FrontendAllResourceScenesValid(active, s.messages);
    status.discard_frame = !status.all_resource_scenes_valid; return status;
}
void FrontendResourceContexts::Check(Token token) const
{ auto& s = *impl_; s.Thread(); const auto& e = s.Find(token); if (e.error) std::rethrow_exception(e.error); }
void FrontendResourceContexts::Release()
{
    auto& s = *impl_; s.Thread(); if (s.released) return;
    // Pop/cancel pending reads before graphics owners and then the asset loads.
    for (auto entry = s.entries.rbegin(); entry != s.entries.rend(); ++entry) QueuePop(entry->first);
    s.busy = true;
    try
    {
        s.ProcessMessages();
        if (s.visual_load) s.visual_load->Cancel(); if (s.image_load) s.image_load->Cancel();
        s.visual_load.reset(); s.image_load.reset(); s.visuals.reset(); s.images.reset();
        Require(s.loaded.empty() && s.queue.empty() && !s.current_resource && !s.current_context,
                "Frontend release retained resource ownership");
        s.released = true; s.busy = false;
    }
    catch (...) { s.busy = false; throw; }
}
}
