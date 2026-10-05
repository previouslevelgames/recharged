#include "runtime/frontend_stack.h"
#include "Game/FE/FrontendSceneCatalog.h"
#include "Game/FE/FrontendStackSteps.h"
#include "Game/FE/FrontendSubhandlerSteps.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
const SceneEntry scene_entries[] = {
#include "Game/FE/FrontendSceneEntries.inc"
};
static_assert(std::size(scene_entries) == 107);
std::atomic<std::uint64_t> next_token{1};
void Require(bool value, const char* message)
{ if (!value) throw std::logic_error(message); }
struct Guard
{
    bool& busy;
    explicit Guard(bool& value) : busy(value) { busy = true; }
    ~Guard() { busy = false; }
};
bool Complete(const FrontendStackCallbacks& c)
{ return c.scene_created && c.initialize_subhandlers && c.after_base_update; }
void CheckCallbacks(const FrontendStackCallbacks& c)
{
    Require(Complete(c) || (!c.scene_created && !c.initialize_subhandlers && !c.after_base_update),
        "Frontend stack callbacks must be complete or explicitly unavailable");
}
}
struct FrontendSceneStack::Implementation
{
    struct EntryData
    {
        FrontendStackEntry value;
        FrontendStackRequest request;
        FrontendStackCallbacks callbacks;
        std::shared_ptr<FrontendSession> session;
        std::shared_ptr<FrontendHandler> handler;
        FrontendStackVisualFactory visual_factory;
        std::shared_ptr<FrontendStackVisual> visual;
        std::exception_ptr error;
    };
    struct Message { Token token; bool push; };
    struct Queue
    {
        std::deque<Message> values;
        void AddEnd(Message value) { values.push_back(value); }
    } queue;
    struct Stack
    {
        std::vector<Token> values;
        void AddStart(Token value) { values.insert(values.begin(), value); }
    } stack;
    FrontendInput& input;
    std::function<void()> drain;
    const std::thread::id thread = std::this_thread::get_id();
    std::map<Token, std::unique_ptr<EntryData>> entries;
    Token top_most = 0;
    bool busy = false, failed = false, released = false;

    Implementation(FrontendInput& i, std::function<void()> d) : input(i), drain(std::move(d))
    {
        Require(bool(drain), "Frontend stack requires an actual frame drain callback");
        input.HasFocusLock(this); stack.values.reserve(32);
    }
    void Thread() const
    { Require(thread == std::this_thread::get_id(), "Frontend stack requires its creating thread"); }
    void Ready() const { Thread(); Require(!released, "Frontend stack has been released"); }
    void Mutable(bool cleanup = false) const
    {
        Ready(); Require(!busy, "Recursive frontend stack mutation is unsupported");
        Require(!nlGetCurrentAsyncRead(), "Frontend stack mutation inside an NL callback is unsupported");
        Require(cleanup || !failed, "Frontend stack requires cleanup after a drain or shared-service failure");
    }
    EntryData& Find(Token token) const
    {
        const auto it = entries.find(token);
        Require(it != entries.end(), "Frontend stack identity is absent or belongs to another owner");
        return *it->second;
    }
    void Drain()
    {
        try { drain(); }
        catch (...) { failed = true; throw; }
    }
    void Validate(const EntryData& e) const
    {
        Require(e.session && e.session->State() == FrontendSessionState::Ready,
            "Frontend callback changed the retained load owner");
        const auto current = e.session->Current();
        Require(current && e.value.prepared && current->visuals == e.value.prepared->visuals
            && current->images == e.value.prepared->images && current->request.path == e.value.prepared->request.path,
            "Frontend callback replaced its resource generation");
    }
    void Initialize(EntryData& e)
    {
        if (!Complete(e.callbacks) && !e.visual_factory) return;
        struct Adapter
        {
            Implementation& owner;
            EntryData& entry;
            void SetPresentation(const FrontendSession::Handle& frame)
            {
                Require(frame && entry.session->Current() == frame, "Frontend creation requires its exact presentation");
                // The native presentation binding is retained session ownership,
                // established here before either original creation stage runs.
                entry.handler = std::make_shared<FrontendHandler>(entry.session, owner.input);
            }
            void SceneCreated()
            {
                FrontendStackContext context{entry.value.token, *entry.session, *entry.handler, entry.value.movement};
                if (entry.visual_factory)
                {
                    auto visual = entry.visual_factory({entry.value.token, entry.session, entry.handler, entry.value.movement});
                    Require(visual && visual->StackScene() == entry.value.scene
                        && visual->StackSession() == entry.session && visual->StackHandler() == entry.handler
                        && visual->Current() == entry.session->Current(), "Selected visual factory returned foreign ownership");
                    visual->AttachStack(); entry.visual=std::move(visual);entry.handler->AttachStack();
                    entry.value.handler_scope = FrontendStackHandlerScope::SelectedVisual;
                }
                else { entry.callbacks.scene_created(context); entry.value.handler_scope = FrontendStackHandlerScope::SuppliedCallbacks; }
                owner.Validate(entry);
            }
            void InitializeSubHandlers()
            {
                FrontendStackContext context{entry.value.token, *entry.session, *entry.handler, entry.value.movement};
                if (entry.visual)
                {
                    // The selected concrete classes inherit this exact empty
                    // source method; it does not complete their SceneCreated.
                    FrontendBaseHandlerInitializeSubHandlers();
                    entry.value.subhandlers = FrontendStackSubhandlers::SourceEmpty;
                }
                else { entry.callbacks.initialize_subhandlers(context); entry.value.subhandlers = FrontendStackSubhandlers::SuppliedCallbacks; }
                owner.Validate(entry);
            }
        } adapter{*this, e};
        FrontendInitializeScene(adapter, e.value.prepared);
        e.value.prepared = e.session->Current();
        e.value.state = FrontendStackState::AwaitingPublication;
    }
    void Fail(EntryData& e)
    {
        e.error = std::current_exception();
        if (e.session) e.session->Cancel();
        e.value.state = FrontendStackState::Failed;
    }
    void Remove(Token token, bool drained = false)
    {
        auto& e = Find(token);
        // Destruction keeps the resource owner until the handler and its
        // callback captures have gone away. Snapshots remain independently owned.
        struct Adapter
        {
            Implementation& owner; EntryData& entry; bool drained;
            void ReleaseResourceHandles() { if (!drained) owner.Drain(); }
            void UnloadPackage() { if (entry.session) entry.session->Cancel(); }
        } adapter{*this, e, drained};
        FrontendUnloadScene(adapter);
        if (e.visual) e.visual->ReleaseStack();
        e.visual.reset();
        if (e.handler) { if(e.visual_factory) e.handler->ReleaseStack(); else e.handler->Release(); }
        e.handler.reset(); e.callbacks = {}; e.visual_factory = {};
        if (e.session) e.session->Pop();
        if (top_most == token) top_most = 0;
        std::erase(stack.values, token); entries.erase(token);
    }
    void Process()
    {
        while (!queue.values.empty())
        {
            const auto command = queue.values.front();
            auto& e = Find(command.token);
            if (command.push)
            {
                FrontendPushScene(stack, command.token);
                try
                {
                    e.session = std::make_shared<FrontendSession>();
                    FrontendSessionRequest request;
                    request.path = '/' + std::string(FrontendSceneStack::SourcePath(e.request.scene));
                    request.language = e.request.language; request.image_profile = e.request.image_profile;
                    request.initial_slide = e.request.initial_slide; request.animate = e.request.animate;
                    if(e.request.shared_resources)e.session->BeginShared(std::move(request),e.request.shared_resources);
                    else e.session->Begin(std::move(request),e.request.resources_mode);
                    e.value.state = FrontendStackState::Loading;
                }
                catch (...) { Fail(e); }
            }
            else Remove(command.token);
            queue.values.pop_front();
        }
    }
    void Poll()
    {
        Process();
        for (const auto token : stack.values)
        {
            auto& e = Find(token);
            if (e.value.queued_pop) continue;
            try
            {
                if (e.value.state == FrontendStackState::Loading)
                {
                    e.session->Poll();
                    if (e.session->State() == FrontendSessionState::Loading) continue;
                    e.value.prepared = e.session->Result();
                    e.value.state = FrontendStackState::AwaitingHandler;
                }
                if (e.value.state == FrontendStackState::AwaitingHandler) Initialize(e);
            }
            catch (...) { Fail(e); }
        }
    }
    void Release()
    {
        Thread(); if (released) return; Mutable(true); Guard guard(busy);
        Drain();
        while (!entries.empty()) Remove(entries.begin()->first, true);
        queue.values.clear(); top_most = 0; released = true;
        // Destroy captures while reentrant mutation remains guarded.
        drain = {};
    }
    ~Implementation() { try { Release(); } catch (...) { std::terminate(); } }
};
FrontendSceneStack::FrontendSceneStack(FrontendInput& input, std::function<void()> drain)
    : impl_(std::make_unique<Implementation>(input, std::move(drain))) {}
FrontendSceneStack::~FrontendSceneStack() = default;
std::string_view FrontendSceneStack::SourcePath(unsigned scene)
{
    if (scene >= std::size(scene_entries) || !scene_entries[scene].mFenFileName)
        throw std::out_of_range("Original frontend scene has no file association");
    Require(static_cast<unsigned>(scene_entries[scene].mSceneID) == scene, "Original frontend scene table order changed");
    return scene_entries[scene].mFenFileName;
}
FrontendSceneStack::Token FrontendSceneStack::QueuePush(FrontendStackRequest request, FrontendStackCallbacks callbacks)
{
    auto& s = *impl_; s.Mutable(); CheckCallbacks(callbacks); (void)SourcePath(request.scene);
    Require(request.language == FrontendLanguage::English || request.language == FrontendLanguage::NAFrench
        || request.language == FrontendLanguage::NASpanish, "Frontend scene stack supports the qualified USA languages");
    Require(request.resources_mode==FrontendSessionResourcesMode::Scene||request.resources_mode==FrontendSessionResourcesMode::PermanentMain,
        "Frontend stack resource mode is unsupported");
    Require(!request.shared_resources||request.resources_mode==FrontendSessionResourcesMode::Scene,
        "Frontend stack cannot both load permanent resources and borrow a token");
    Require((!request.shared_resources&&request.resources_mode==FrontendSessionResourcesMode::Scene)
        ||request.image_profile==FrontendImageProfile::Main,"Shared frontend stack resources require Main profile");
    Require(request.movement <= 2, "Frontend scene movement is outside original ScreenMovement");
    Require(s.entries.size() < 32, "Frontend scene stack exceeds its original 32-entry limit");
    const auto token = next_token.fetch_add(1);
    Require(token && token != std::numeric_limits<Token>::max(), "Frontend scene identity range exhausted");
    Guard guard(s.busy);
    auto entry = std::make_unique<Implementation::EntryData>();
    entry->value.token = token; entry->value.scene = request.scene; entry->value.movement = request.movement;
    entry->request = std::move(request); entry->callbacks = std::move(callbacks);
    s.entries.emplace(token, std::move(entry));
    try { FrontendQueueScene(s.queue, Implementation::Message{token, true}); }
    catch (...) { s.entries.erase(token); throw; }
    return token;
}
FrontendSceneStack::Token FrontendSceneStack::QueuePop()
{
    auto& s = *impl_; s.Mutable();
    for (const auto token : s.stack.values)
        if (!s.Find(token).value.queued_pop) { QueuePop(token); return token; }
    throw std::logic_error("Frontend scene stack has no unqueued processed entry to pop");
}
void FrontendSceneStack::QueuePop(Token token)
{
    auto& s = *impl_; s.Mutable(); auto& e = s.Find(token);
    Require(e.value.state != FrontendStackState::Queued && !e.value.queued_pop,
        "Frontend scene is not processed or already queued for pop");
    Guard guard(s.busy); FrontendQueueScene(s.queue, Implementation::Message{token, false});
    e.value.queued_pop = true;
}
void FrontendSceneStack::Cancel(Token token)
{
    auto& s = *impl_; s.Mutable(); auto& e = s.Find(token);
    Require(!e.value.published, "Published frontend scenes must use queued pop and drain");
    Guard guard(s.busy);
    // A caller may have prepared graphics for this candidate; drain even though
    // no visible snapshot has been acknowledged yet.
    s.Remove(token);
    std::erase_if(s.queue.values, [token](const auto& v) { return v.token == token; });
}
void FrontendSceneStack::Bind(Token token, FrontendStackCallbacks callbacks)
{
    auto& s = *impl_; s.Mutable(); auto& e = s.Find(token);
    Require(Complete(callbacks), "Frontend scene handler services are unavailable");
    Require(!Complete(e.callbacks) && !e.visual_factory && !e.value.queued_pop
        && (e.value.state == FrontendStackState::Queued || e.value.state == FrontendStackState::Loading
            || e.value.state == FrontendStackState::AwaitingHandler), "Frontend scene creation is already bound or complete");
    Guard guard(s.busy); e.callbacks = std::move(callbacks);
}
void FrontendSceneStack::BindVisual(Token token, FrontendStackVisualFactory factory)
{
    auto& s=*impl_;s.Mutable();auto& e=s.Find(token);
    Require(factory&&(e.value.scene==1||e.value.scene==13||e.value.scene==14||e.value.scene==15||e.value.scene==23), "Selected visual factory supports Main, Options, Audio, Visual and Credits only");
    Require(!Complete(e.callbacks)&&!e.visual_factory&&!e.value.queued_pop
        &&(e.value.state==FrontendStackState::Queued||e.value.state==FrontendStackState::Loading
            ||e.value.state==FrontendStackState::AwaitingHandler), "Frontend visual creation is already bound or complete");
    Guard guard(s.busy);e.visual_factory=std::move(factory);
}
void FrontendSceneStack::Poll()
{ auto& s = *impl_; s.Mutable(); Guard guard(s.busy); s.Poll(); }
void FrontendSceneStack::Service()
{
    auto& s = *impl_; s.Mutable(); Guard guard(s.busy); s.Poll();
    if (std::none_of(s.entries.begin(), s.entries.end(), [](const auto& v)
        { return v.second->value.state == FrontendStackState::Loading; })) return;
    try { nlServiceFileSystem(); }
    catch (...)
    {
        s.failed = true;
        for (auto& [token, e] : s.entries)
            if (e->value.state == FrontendStackState::Loading) s.Fail(*e);
        throw;
    }
    s.Poll();
}
void FrontendSceneStack::Update(float delta, PresentedInput input)
{
    auto& s = *impl_; s.Mutable();
    Require(std::isfinite(delta) && delta >= 0 && delta <= 60, "Frontend scene delta exceeds its bounded profile");
    Poll(); Guard guard(s.busy);
    for (const auto token : s.stack.values)
    {
        auto& e = s.Find(token);
        if (e.value.queued_pop || e.value.state != FrontendStackState::Published) continue;
        try
        {
            s.Validate(e);
            Require(e.session->Current() == e.value.published, "Frontend handler mutated outside its stack callback");
            if (e.visual)
            {
                Require(e.visual->Current()==e.value.published,"Selected visual mutated outside its stack update");
                const bool admitted=e.visual->CanUpdateStack();
                s.Validate(e);
                Require(e.session->Current()==e.value.published&&e.visual->Current()==e.value.published,
                    "Selected visual mutated during pre-base admission");
                if(!admitted)continue;
                auto proof=e.handler->StackUpdateOnce(e.value.published,delta);
                Require(proof.Before()==e.value.published&&proof.After()==e.session->Current()&&proof.Delta()==delta,
                    "Frontend stack base update proof differs");
                e.visual->UpdateStack(std::move(proof),e.value.published,[&]{if(input)input(token,e.value.published);});
                Require(e.visual->Current()==e.session->Current(),"Selected visual lost its current frame");
            }
            else
            {
                e.handler->Update(e.value.published, delta);
                FrontendStackContext context{token, *e.session, *e.handler, e.value.movement};
                e.callbacks.after_base_update(context, delta);
            }
            s.Validate(e);
            e.value.prepared = e.session->Current();
            e.value.state = FrontendStackState::AwaitingPublication;
        }
        catch (...) { s.Fail(e); }
    }
}
void FrontendSceneStack::Publish(Token token, const FrontendSession::Handle& frame)
{
    const FrontendStackPublication publication{token, frame};
    Publish(std::span{&publication, 1});
}
void FrontendSceneStack::Publish(std::span<const FrontendStackPublication> publications)
{
    auto& s = *impl_; s.Mutable();
    Require(!publications.empty() && publications.size() <= 32, "Frontend publication batch must contain 1..32 entries");
    struct Candidate { Implementation::EntryData* entry; FrontendSession::Handle frame; };
    std::vector<Candidate> candidates; candidates.reserve(publications.size());
    for (const auto& publication : publications)
    {
        auto& e = s.Find(publication.token); const auto& frame = publication.prepared;
        Require(std::none_of(candidates.begin(), candidates.end(), [&](const auto& v) { return v.entry == &e; }),
            "Frontend publication batch repeats an entry");
        Require(!e.value.queued_pop && e.value.state == FrontendStackState::AwaitingPublication
            && frame && frame == e.value.prepared, "Frontend publication requires the exact prepared frame");
        s.Validate(e); Require(e.session->Current() == frame, "Frontend publication is stale");
        candidates.push_back({&e, frame});
    }
    Guard guard(s.busy); s.Drain();
    try
    {
        for (const auto& candidate : candidates)
        {
            s.Validate(*candidate.entry);
            Require(candidate.entry->session->Current() == candidate.frame, "Frontend drain changed a candidate scene");
        }
    }
    catch (...) { s.failed = true; throw; }
    for (auto& candidate : candidates)
    {
        candidate.entry->value.published = std::move(candidate.frame);
        candidate.entry->value.state = FrontendStackState::Published;
    }
}
void FrontendSceneStack::SetVisible(Token token, bool visible)
{ auto& s = *impl_; s.Mutable(); s.Find(token).value.visible = visible; }
void FrontendSceneStack::SetTopMost(Token token)
{ auto& s = *impl_; s.Mutable(); if (token) (void)s.Find(token); s.top_most = token; }
FrontendStackEntry FrontendSceneStack::Entry(Token token) const
{ impl_->Ready(); return impl_->Find(token).value; }
FrontendSessionResources::Handle FrontendSceneStack::Resources(Token token)const
{
    const auto& s=*impl_;s.Ready();const auto& e=s.Find(token);
    if(!e.request.shared_resources&&e.request.resources_mode==FrontendSessionResourcesMode::Scene)return {};
    Require(e.session&&e.session->Current(),"Frontend stack resources are not ready");
    return e.session->SharedResources();
}
std::vector<FrontendStackEntry> FrontendSceneStack::Entries() const
{
    const auto& s = *impl_; s.Ready(); std::vector<FrontendStackEntry> result;
    result.reserve(s.entries.size());
    for (const auto token : s.stack.values) result.push_back(s.Find(token).value);
    for (const auto& command : s.queue.values) if (command.push) result.push_back(s.Find(command.token).value);
    return result;
}
std::vector<FrontendStackEntry> FrontendSceneStack::RenderPlan() const
{
    const auto& s = *impl_; s.Ready(); std::vector<FrontendStackEntry> result;
    const auto append = [&](Token token)
    {
        const auto& e = s.Find(token).value;
        if (!e.queued_pop && FrontendSceneVisible(bool(e.published), e.visible)) result.push_back(e);
    };
    if (s.top_most && std::find(s.stack.values.begin(), s.stack.values.end(), s.top_most) != s.stack.values.end()) append(s.top_most);
    for (const auto token : s.stack.values) if (token != s.top_most) append(token);
    return result;
}
bool FrontendSceneStack::AllReady() const
{
    const auto& s = *impl_; s.Ready();
    return !s.failed && s.queue.values.empty() && std::all_of(s.entries.begin(), s.entries.end(),
        [](const auto& v) { return v.second->value.state == FrontendStackState::Published; });
}
bool FrontendSceneStack::Failed() const { impl_->Ready(); return impl_->failed; }
void FrontendSceneStack::RethrowFailure(Token token) const
{ impl_->Ready(); const auto& e = impl_->Find(token); if (e.error) std::rethrow_exception(e.error); }
void FrontendSceneStack::Release() { impl_->Release(); }
}
