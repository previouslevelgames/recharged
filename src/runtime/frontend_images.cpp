#include "runtime/frontend_images.h"
#include "runtime/whole_file.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <algorithm>
#include <array>
#include <exception>
#include <thread>

namespace mscharged
{
struct FrontendImageLoad::Implementation
{
    struct Request { Implementation* owner; unsigned index, token = 0; bool complete = false; };
    const std::thread::id thread = std::this_thread::get_id();
    resources::FrontendScene selection{};
    std::array<const char*, 2> paths{};
    std::array<Request, 2> requests{{{this, 0}, {this, 1}}};
    std::array<std::size_t, 2> sizes{};
    std::array<std::vector<std::uint8_t>, 2> bytes;
    unsigned count = 0;
    resources::FrontendImageCatalog::Handle current;
    std::exception_ptr error;
    FrontendImageState state = FrontendImageState::Idle;
    bool servicing = false;
    auto Requests() { return std::span(requests).first(count); }
    void CheckThread() const
    {
        if (thread != std::this_thread::get_id()) throw std::logic_error("Frontend images require their NL servicing thread");
    }
    void CheckMutation() const
    {
        CheckThread();
        if (servicing) throw std::logic_error("Frontend image mutation during NL service is not supported");
    }
    void Drain()
    {
        for (auto& request : requests)
        {
            if (request.token) nlCancelEntireFileLoad(request.token, nullptr);
            request.token = 0;
        }
        for (auto& file : bytes) std::vector<std::uint8_t>().swap(file);
    }
    void Start()
    {
        if (!gMemoryInitialized || !nlFileSystemReady())
            throw std::logic_error("Frontend image reads require initialized memory and NL files");
        std::size_t total = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            std::unique_ptr<nlFile> file(nlOpen(paths[i]));
            if (!file) throw std::runtime_error(std::string("Frontend image bundle is missing: ") + paths[i]);
            sizes[i] = nlFileSize(file.get(), nullptr);
            if (sizes[i] < 16 || sizes[i] > resources::MaximumAssetBytes || sizes[i] > 32 * 1024 * 1024 - total)
                throw std::length_error("Frontend image bundle read exceeds its limits");
            total += sizes[i];
        }
        for (auto& request : Requests())
        {
            request.token = nlLoadEntireFileAsync(paths[request.index], Complete, &request,
                32, AllocateEnd, nullptr, 0, &VirtualAllocator);
            if (!request.token && !request.complete) throw std::runtime_error("Frontend image read was not queued");
            if (error) std::rethrow_exception(error);
        }
    }
    static void Complete(void* data, unsigned long size, void* context)
    {
        std::unique_ptr<void, void(*)(void*)> buffer(data, nlFree);
        auto& request = *static_cast<Request*>(context);
        auto& s = *request.owner;
        s.CheckThread(); request.token = 0; request.complete = true;
        if (s.error) return;
        try
        {
            if (size != s.sizes[request.index]) throw std::runtime_error("Frontend image bundle changed size during its read");
            const auto* first = static_cast<const std::uint8_t*>(data);
            s.bytes[request.index].assign(first, first + size);
        }
        catch (...) { s.error = std::current_exception(); }
    }
};
FrontendImageLoad::FrontendImageLoad() : impl_(std::make_unique<Implementation>()) {}
FrontendImageLoad::~FrontendImageLoad() { try { Cancel(); } catch (...) { std::terminate(); } }
void FrontendImageLoad::Begin(const resources::FrontendScene& scene, FrontendImageProfile profile)
{
    auto& s = *impl_; s.CheckMutation(); s.Drain();
    s.error = {}; s.count = 0; s.state = FrontendImageState::Loading;
    for (auto& request : s.requests) request.complete = false;
    try
    {
        if (profile != FrontendImageProfile::Main && profile != FrontendImageProfile::InGame)
            throw std::invalid_argument("Unknown frontend image profile");
        resources::Require(scene.resources.size() <= 16384, "Frontend resource request exceeds its limits");
        s.selection.resources = scene.resources;
        bool have_static = false;
        for (const auto& resource : s.selection.resources)
        {
            resources::Require(resource.type <= 2, "Unknown frontend resource type");
            have_static |= resource.type == 0 && !resources::IsDynamicFrontendImage(resource.hash);
        }
        if (!have_static)
        {
            s.current = resources::ReadFrontendImages(s.selection, {});
            s.state = FrontendImageState::Ready;
            return;
        }
        s.count = profile == FrontendImageProfile::Main ? 1 : 2;
        s.paths = profile == FrontendImageProfile::Main ? std::array{"art/fe/MainUI.Dmn", ""}
            : std::array{"art/fe/InGameUI.Res", "art/fe/InGameUI.Dmn"};
        s.Start();
    }
    catch (...) { s.error = std::current_exception(); s.Drain(); s.state = FrontendImageState::Failed; throw; }
}
void FrontendImageLoad::Poll()
{
    auto& s = *impl_; s.CheckMutation();
    if (s.state != FrontendImageState::Loading) return;
    try
    {
        if (s.error) std::rethrow_exception(s.error);
        for (const auto& request : s.Requests())
            if (!request.complete && !WholeFileLoadPending(request.token))
                throw std::runtime_error(std::string("Frontend image read failed or file services stopped: ") + s.paths[request.index]);
        if (std::all_of(s.Requests().begin(), s.Requests().end(), [](const auto& r) { return r.complete; }))
        {
            const std::array bundles{
                resources::FrontendImageBundle{s.bytes[0], resources::FrontendImageBundleKind::Permanent},
                resources::FrontendImageBundle{s.bytes[1], resources::FrontendImageBundleKind::OnDemand}};
            auto next = resources::ReadFrontendImages(s.selection, std::span(bundles).first(s.count));
            s.current = std::move(next); s.Drain(); s.state = FrontendImageState::Ready;
        }
    }
    catch (...) { s.error = std::current_exception(); s.Drain(); s.state = FrontendImageState::Failed; }
}
void FrontendImageLoad::Service()
{
    Poll(); if (impl_->state != FrontendImageState::Loading) return;
    impl_->servicing = true;
    try { nlServiceFileSystem(); }
    catch (...) { impl_->servicing = false; Poll(); throw; }
    impl_->servicing = false;
    Poll();
}
void FrontendImageLoad::Cancel()
{
    auto& s = *impl_; s.CheckMutation();
    if (s.state == FrontendImageState::Loading) { s.Drain(); s.state = FrontendImageState::Cancelled; }
}
void FrontendImageLoad::Unload()
{ Cancel(); auto& s = *impl_; s.current.reset(); s.error = {}; s.state = FrontendImageState::Idle; }
FrontendImageState FrontendImageLoad::State() const { impl_->CheckThread(); return impl_->state; }
unsigned FrontendImageLoad::CompletedFiles() const
{
    impl_->CheckThread();
    return std::count_if(impl_->requests.begin(), impl_->requests.end(), [](const auto& r) { return r.complete; });
}
resources::FrontendImageCatalog::Handle FrontendImageLoad::Current() const { impl_->CheckThread(); return impl_->current; }
resources::FrontendImageCatalog::Handle FrontendImageLoad::Result() const
{
    impl_->CheckThread(); if (impl_->error) std::rethrow_exception(impl_->error);
    if (impl_->state != FrontendImageState::Ready) throw std::logic_error("Frontend images are not ready");
    return impl_->current;
}
}
