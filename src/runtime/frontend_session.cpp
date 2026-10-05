#include "runtime/frontend_session.h"
#include "runtime/whole_file.h"
#include "resources/frontend_animation.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <algorithm>
#include <exception>
#include <thread>

namespace mscharged
{
struct FrontendSession::Implementation
{
    const std::thread::id thread = std::this_thread::get_id();
    FrontendSessionState state = FrontendSessionState::Idle;
    FrontendSessionProgress progress;
    Handle current;
    FrontendSessionResources::Handle current_resources;
    std::unique_ptr<resources::FrontendAnimationPlayback> playback;
    std::exception_ptr error;
    bool servicing = false;
    struct Pending
    {
        Implementation& owner;
        FrontendSessionRequest request;
        unsigned token = 0;
        std::size_t size = 0;
        bool complete = false;
        std::exception_ptr error;
        std::unique_ptr<resources::FrontendScene> graph;
        std::unique_ptr<FrontendVisualLoad> visuals;
        std::unique_ptr<FrontendImageLoad> images;
        FrontendSessionResourcesMode mode = FrontendSessionResourcesMode::Scene;
        FrontendSessionResources::Handle shared;
        explicit Pending(Implementation& s, FrontendSessionRequest r) : owner(s), request(std::move(r)) {}
        ~Pending() { if (token) nlCancelEntireFileLoad(token, nullptr); }
        void Start()
        {
            if (!gMemoryInitialized || !nlFileSystemReady())
                throw std::logic_error("Frontend session requires initialized memory and NL files");
            const auto& path = request.path;
            if (path.empty() || path.front() != '/' || path.size() > 4096 || path.find('\0') != std::string::npos)
                throw std::invalid_argument("Invalid frontend scene path");
            if (request.initial_slide.size() > 4096 || request.initial_slide.find('\0') != std::string::npos)
                throw std::invalid_argument("Invalid initial frontend slide name");
            if (request.image_profile != FrontendImageProfile::Main && request.image_profile != FrontendImageProfile::InGame
                && request.image_profile != FrontendImageProfile::BootLoading)
                throw std::invalid_argument("Unknown frontend image profile");
            {
                std::unique_ptr<nlFile> file(nlOpen(path.c_str()));
                if (!file) throw std::runtime_error("Frontend scene file is missing: " + path);
                size = nlFileSize(file.get(), nullptr);
                if (size < 16 || size > resources::MaximumAssetBytes)
                    throw std::length_error("Frontend scene file exceeds its limits");
            }
            if (!shared) visuals = std::make_unique<FrontendVisualLoad>(request.language);
            token = nlLoadEntireFileAsync(path.c_str(), Complete, this, 32, AllocateEnd, nullptr, 0, &VirtualAllocator);
            if (!token && !complete) throw std::runtime_error("Frontend scene read was not queued");
            if (error) std::rethrow_exception(error);
        }
        static void Complete(void* data, unsigned long size, void* context)
        {
            std::unique_ptr<void, void(*)(void*)> buffer(data, nlFree);
            auto& s = *static_cast<Pending*>(context);
            s.owner.CheckThread(); s.token = 0; s.complete = true;
            try
            {
                if (size != s.size) throw std::runtime_error("Frontend scene changed size during its read");
                s.graph = std::make_unique<resources::FrontendScene>(resources::ReadFrontendScene(
                    {static_cast<const std::uint8_t*>(data), size}));
            }
            catch (...) { s.error = std::current_exception(); }
        }
        FrontendSessionProgress Progress() const
        {
            return {complete, visuals ? visuals->CompletedMask() : 0, images ? images->CompletedFiles() : 0};
        }
    };
    std::unique_ptr<Pending> pending;
    void CheckThread() const
    {
        if (thread != std::this_thread::get_id())
            throw std::logic_error("Frontend session requires its NL servicing thread");
    }
    void CheckMutation() const
    {
        CheckThread();
        if (servicing) throw std::logic_error("Frontend session mutation during shared NL service is not supported");
    }
    void Drain()
    {
        if (pending) progress = pending->Progress();
        pending.reset();
    }
    resources::FrontendReference InitialSlide(const Pending& p) const
    {
        if (p.request.initial_slide.empty()) return p.graph->active_slide;
        resources::FrontendReference selected;
        for (const auto& slide : p.graph->slides)
            if (slide.name == p.request.initial_slide && std::find(p.graph->presentation_slides.begin(),
                p.graph->presentation_slides.end(), slide.offset) != p.graph->presentation_slides.end())
            {
                if (selected) throw std::invalid_argument("Frontend presentation slide name is ambiguous");
                selected = slide.offset;
            }
        if (!selected) throw std::invalid_argument("Frontend presentation slide name is absent");
        return selected;
    }
    static resources::FrontendLayoutFrame Layout(const FrontendSessionFrame& next)
    {
        const std::array fonts{next.visuals->text, next.visuals->heading};
        if(next.request.image_profile==FrontendImageProfile::BootLoading)
        {
            if(next.visuals->font_registration_order.empty())throw std::logic_error("Boot layout requires actual font registration order");
            return resources::BuildFrontendLayout(next.graph,*next.visuals->localization,next.visuals->font_registration_order,{},*next.images,{true,true});
        }
        return resources::BuildFrontendLayout(next.graph, *next.visuals->localization, fonts, {}, *next.images);
    }
    void Poll()
    {
        if (!pending) return;
        try
        {
            auto& p = *pending;
            if (p.error) std::rethrow_exception(p.error);
            if (!p.complete && !WholeFileLoadPending(p.token))
                throw std::runtime_error("Frontend scene read failed or file services stopped");
            if (p.visuals)
            {
                p.visuals->Poll();
                if (p.visuals->Ready()) (void)p.visuals->Result();
            }
            if (p.graph && !p.images && !p.shared)
            {
                (void)InitialSlide(p); // Reject the explicit CLI selection before image work.
                p.images = std::make_unique<FrontendImageLoad>();
                if (p.mode == FrontendSessionResourcesMode::PermanentMain) p.images->BeginPermanentMain();
                else p.images->Begin(*p.graph, p.request.image_profile);
            }
            if (p.images)
            {
                p.images->Poll();
                if (p.images->State() == FrontendImageState::Failed) (void)p.images->Result();
            }
            progress = p.Progress();
            if (!p.graph || (!p.shared && (!p.visuals->Ready() || !p.images || p.images->State() != FrontendImageState::Ready))) return;
            auto next = std::make_shared<FrontendSessionFrame>();
            next->request = p.request;
            next->visuals = p.shared ? p.shared->visuals_ : p.visuals->Result();
            next->images = p.shared ? p.shared->images_ : p.images->Result();
            next->image_completed_files = progress.image_completed_files;
            resources::RequireFrontendImages(*p.graph, *next->images);
            auto next_resources = p.shared;
            if (!next_resources && p.mode == FrontendSessionResourcesMode::PermanentMain)
            {
                // Include font pages in the same registration bounds as the
                // graphics owner. Image/font hash collisions are not aliases.
                std::map<std::uint32_t, const resources::Texture*> textures;
                std::size_t bytes = 0;
                const auto add = [&](const resources::Texture& texture)
                {
                    const auto [entry, inserted] = textures.emplace(texture.id, &texture);
                    resources::Require(inserted || entry->second == &texture, "Shared frontend texture hashes collide");
                    if (!inserted) return;
                    const auto size = texture.pixels.size() + texture.palette.size();
                    resources::Require(textures.size() <= 1024 && size <= 64 * 1024 * 1024 - bytes,
                        "Shared frontend resources exceed 1024 textures or 64 MiB");
                    bytes += size;
                };
                for (const auto& [hash, texture] : next->images->textures) add(*texture);
                for (const auto& font : {next->visuals->text, next->visuals->heading})
                    for (const auto& page : font->pages) add(page);
                auto verified = std::shared_ptr<FrontendSessionResources>(new FrontendSessionResources);
                verified->language_ = p.request.language;
                verified->visuals_ = next->visuals; verified->images_ = next->images;
                next_resources = std::move(verified);
            }
            std::unique_ptr<resources::FrontendAnimationPlayback> next_playback;
            const auto selected = InitialSlide(p);
            if (p.request.animate)
            {
                next_playback = std::make_unique<resources::FrontendAnimationPlayback>(*p.graph, selected);
                next->graph = next_playback->Scene(); next->channels_evaluated = next_playback->ChannelsEvaluated();
            }
            else { next->graph = *p.graph; next->graph.active_slide = selected; }
            next->layout = Layout(*next);
            Drain(); playback = std::move(next_playback); current = std::move(next);
            current_resources = std::move(next_resources); state = FrontendSessionState::Ready;
        }
        catch (...) { error = std::current_exception(); Drain(); state = FrontendSessionState::Failed; }
    }
    template<class F> bool Mutate(F operation, const std::function<void()>& before_publish = {})
    {
        CheckMutation();
        if (!current || !playback) throw std::logic_error("Frontend timeline mutation requires a current animated scene");
        auto next_playback = playback->Clone();
        const bool selected = operation(*next_playback);
        auto next = std::make_shared<FrontendSessionFrame>();
        next->request = current->request; next->visuals = current->visuals; next->images = current->images;
        next->image_completed_files = current->image_completed_files;
        next->graph = next_playback->Scene(); next->channels_evaluated = next_playback->ChannelsEvaluated();
        next->layout = Layout(*next);
        // Admit external handler services only after every fallible scene/layout
        // allocation has succeeded. Publication below consists of noexcept moves.
        if (before_publish) before_publish();
        playback = std::move(next_playback); current = std::move(next);
        return selected;
    }
};
FrontendSession::FrontendSession() : impl_(std::make_unique<Implementation>()) {}
FrontendSession::~FrontendSession() { try { Pop(); } catch (...) { std::terminate(); } }
void FrontendSession::Begin(FrontendSessionRequest request)
{ Begin(std::move(request), FrontendSessionResourcesMode::Scene); }
void FrontendSession::Begin(FrontendSessionRequest request, FrontendSessionResourcesMode mode)
{
    auto& s = *impl_; s.CheckMutation();
    if (mode != FrontendSessionResourcesMode::Scene && mode != FrontendSessionResourcesMode::PermanentMain)
        throw std::invalid_argument("Unknown frontend session resource mode");
    if (mode == FrontendSessionResourcesMode::PermanentMain && request.image_profile != FrontendImageProfile::Main)
        throw std::invalid_argument("Permanent sharing requires the Main image profile");
    s.Drain(); s.error = {}; s.progress = {}; s.state = FrontendSessionState::Loading;
    try { s.pending = std::make_unique<Implementation::Pending>(s, std::move(request)); s.pending->mode = mode; s.pending->Start(); }
    catch (...) { s.error = std::current_exception(); s.Drain(); s.state = FrontendSessionState::Failed; throw; }
}
void FrontendSession::BeginShared(FrontendSessionRequest request, FrontendSessionResources::Handle resources)
{
    auto& s = *impl_; s.CheckMutation();
    if (!resources || request.image_profile != FrontendImageProfile::Main || request.language != resources->language_)
        throw std::invalid_argument("Shared frontend resources require their verified language and Main profile");
    s.Drain(); s.error = {}; s.progress = {}; s.state = FrontendSessionState::Loading;
    try
    {
        s.pending = std::make_unique<Implementation::Pending>(s, std::move(request));
        s.pending->shared = std::move(resources); s.pending->Start();
    }
    catch (...) { s.error = std::current_exception(); s.Drain(); s.state = FrontendSessionState::Failed; throw; }
}
FrontendSessionResources::Handle FrontendSession::SharedResources() const
{
    impl_->CheckThread();
    if (!impl_->current_resources) throw std::logic_error("Current frontend scene has no shared permanent resources");
    return impl_->current_resources;
}
void FrontendSession::Poll() { impl_->CheckMutation(); impl_->Poll(); }
void FrontendSession::Service()
{
    auto& s = *impl_; s.CheckMutation(); s.Poll(); if (!s.pending) return;
    s.servicing = true;
    try { nlServiceFileSystem(); }
    catch (...)
    {
        s.servicing = false; s.error = std::current_exception(); s.Drain(); s.state = FrontendSessionState::Failed;
        throw; // Shared-pump exceptions never publish partial or completed replacements.
    }
    s.servicing = false; s.Poll();
}
void FrontendSession::Cancel()
{
    auto& s = *impl_; s.CheckMutation();
    if (s.pending) { s.Drain(); s.state = FrontendSessionState::Cancelled; }
}
void FrontendSession::Pop()
{
    auto& s = *impl_; s.CheckMutation(); s.Drain(); s.playback.reset(); s.current.reset(); s.current_resources.reset();
    s.error = {}; s.progress = {}; s.state = FrontendSessionState::Idle;
}
FrontendSessionState FrontendSession::State() const { impl_->CheckThread(); return impl_->state; }
FrontendSessionProgress FrontendSession::Progress() const
{ impl_->CheckThread(); return impl_->pending ? impl_->pending->Progress() : impl_->progress; }
FrontendSession::Handle FrontendSession::Current() const { impl_->CheckThread(); return impl_->current; }
FrontendSession::Handle FrontendSession::Result() const
{
    impl_->CheckThread(); if (impl_->error) std::rethrow_exception(impl_->error);
    if (impl_->state != FrontendSessionState::Ready) throw std::logic_error("Frontend scene is pending or cancelled");
    return impl_->current;
}
void FrontendSession::Advance(float delta)
{ impl_->Mutate([&](auto& playback) { playback.Advance(delta); return true; }); }
bool FrontendSession::AdvanceLoadingNotification(float delta,std::uint32_t id)
{ return impl_->Mutate([&](auto& playback) { return playback.AdvanceLoadingNotification(delta,id); }); }
void FrontendSession::Reset()
{ impl_->Mutate([](auto& playback) { playback.Reset(); return true; }); }
bool FrontendSession::SelectPresentation(std::string_view name, bool reset)
{ return impl_->Mutate([&](auto& playback) { return playback.SelectPresentation(name, reset); }); }
bool FrontendSession::SelectComponent(std::uint32_t id, std::string_view name, bool reset, bool preserve)
{ return impl_->Mutate([&](auto& playback) { return playback.SelectComponent(id, name, reset, preserve); }); }
void FrontendSession::Apply(const Handle& expected, std::span<const resources::FrontendInstanceChange> changes)
{
    auto& s = *impl_; s.CheckMutation();
    if (!expected || expected != s.current) throw std::logic_error("Frontend mutation requires the current retained snapshot");
    if (s.playback)
    {
        s.Mutate([&](auto& playback) { playback.Apply(changes); return true; });
        return;
    }
    auto next = std::make_shared<FrontendSessionFrame>(*s.current);
    resources::ApplyFrontendInstanceChanges(next->graph, changes);
    next->layout = Implementation::Layout(*next); s.current = std::move(next);
}
resources::FrontendLoadingSetup FrontendSession::SetupLoadingScene(const Handle& expected, bool widescreen)
{
    auto& s = *impl_; s.CheckMutation();
    if (!expected || expected != s.current) throw std::logic_error("Loading setup requires the current retained snapshot");
    resources::FrontendLoadingSetup result;
    s.Mutate([&](auto& playback) { result = playback.SetupLoadingScene(widescreen); return true; });
    return result;
}
void FrontendSession::HandlerTransaction(const Handle& expected,
    const std::function<void(resources::FrontendAnimationPlayback&)>& operation,
    const std::function<void()>& before_publish)
{
    auto& s=*impl_;s.CheckMutation();
    if (!expected || expected!=s.current) throw std::logic_error("Frontend handler requires the visible current snapshot");
    s.Mutate([&](auto& playback){operation(playback);return true;}, before_publish);
}

}
