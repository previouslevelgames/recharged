#include "runtime/frontend_visuals.h"
#include "runtime/whole_file.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <cstring>
#include <stdexcept>

namespace mscharged
{
namespace
{
constexpr std::array language_paths{"art/fe/english.loc", "art/fe/nafrench.loc", "art/fe/naspanish.loc"};
constexpr std::array<std::uint32_t, 3> language_hashes{0x7a947b29, 0x30d469c4, 0x2f242024};
struct FreeBuffer { void operator()(void* data) const noexcept { nlFree(data); } };
void CheckFile(const char* filename)
{
    std::unique_ptr<nlFile> file(nlOpen(filename));
    if (!file) throw std::runtime_error(std::string("Frontend visual file is missing: ") + filename);
    const auto size = nlFileSize(file.get(), nullptr);
    if (!size || size > resources::MaximumAssetBytes)
        throw std::length_error("Frontend visual file is empty or exceeds the asset limit");
}
}
void FrontendVisualLoad::CheckThread() const
{
    if (thread_ != std::this_thread::get_id())
        throw std::logic_error("Frontend visual requires its NL servicing thread");
}
FrontendVisualLoad::FrontendVisualLoad(FrontendLanguage language) : assets_(std::make_shared<FrontendVisualAssets>())
{
    if (!gMemoryInitialized || !nlFileSystemReady())
        throw std::logic_error("Frontend visual requires initialized memory and NL files");
    const auto index = static_cast<unsigned>(language);
    if (index >= language_paths.size()) throw std::invalid_argument("Unsupported frontend language");
    language_hash_ = language_hashes[index];
    const std::array paths{language_paths[index], "art/fe/fonts/eurfonttext18.res", "art/fe/fonts/eurfontheading36.res"};
    // Validate every size/path before submitting any work.
    for (const char* path : paths) CheckFile(path);
    try
    {
        for (unsigned i = 0; i < requests_.size(); ++i)
        {
            auto& request = requests_[i];
            request.owner = this; request.index = i;
            request.token = nlLoadEntireFileAsync(paths[i], Complete, &request,
                32, AllocateEnd, nullptr, 0, &VirtualAllocator);
            if (!request.token && !request.complete)
                throw std::runtime_error("Frontend visual read was not queued");
            if (error_) std::rethrow_exception(error_);
        }
    }
    catch (...) { Drain(); throw; }
}
FrontendVisualLoad::~FrontendVisualLoad()
{
    try { Cancel(); } catch (...) { std::terminate(); }
}
void FrontendVisualLoad::Complete(void* data, unsigned long size, void* context)
{
    std::unique_ptr<void, FreeBuffer> buffer(data);
    auto& request = *static_cast<Request*>(context);
    auto& owner = *request.owner;
    owner.CheckThread();
    request.complete = true; request.token = 0;
    try
    {
        const resources::Bytes bytes{static_cast<const std::uint8_t*>(data), size};
        resources::Require(size <= resources::MaximumAssetBytes, "Frontend visual file exceeds the asset limit");
        switch (request.index)
        {
        case 0: owner.assets_->localization = resources::ReadLocalization(bytes, owner.language_hash_); break;
        case 1: owner.assets_->text = resources::ReadFrontendFont(bytes, "fe/fonts/eurfonttext18", "fot-rodinprob18"); break;
        case 2: owner.assets_->heading = resources::ReadFrontendFont(bytes, "fe/fonts/eurfontheading36", "Scratchy36"); break;
        default: throw std::logic_error("Unknown Frontend visual resource");
        }
        owner.completed_mask_ |= 1u << request.index;
    }
    catch (...) { if (!owner.error_) owner.error_ = std::current_exception(); }
}
void FrontendVisualLoad::Drain()
{
    for (auto& request : requests_)
    {
        if (request.token) nlCancelEntireFileLoad(request.token, nullptr);
        request.token = 0;
    }
}
void FrontendVisualLoad::Poll()
{
    CheckThread();
    if (terminal_) return;
    for (const auto& request : requests_)
        if (!request.complete && !WholeFileLoadPending(request.token) && !error_)
            error_ = std::make_exception_ptr(std::runtime_error("Frontend visual read failed or file services stopped"));
    if (error_)
    {
        Drain(); assets_.reset(); terminal_ = true;
    }
    else if (completed_mask_ == 7) terminal_ = true;
}
void FrontendVisualLoad::Service()
{
    CheckThread(); Poll();
    if (terminal_) return;
    try { nlServiceFileSystem(); }
    catch (...)
    {
        const auto error = std::current_exception();
        Poll();
        std::rethrow_exception(error); // Shared service failures must stay visible.
    }
    Poll();
}
void FrontendVisualLoad::Cancel()
{
    CheckThread();
    if (terminal_) return;
    Poll();
    if (terminal_) return;
    Drain(); assets_.reset(); cancelled_ = terminal_ = true;
}
bool FrontendVisualLoad::Ready() const { CheckThread(); return terminal_; }
unsigned FrontendVisualLoad::CompletedMask() const { CheckThread(); return completed_mask_; }
std::shared_ptr<const FrontendVisualAssets> FrontendVisualLoad::Result() const
{
    CheckThread();
    if (error_) std::rethrow_exception(error_);
    if (cancelled_) throw std::runtime_error("Frontend visual was cancelled");
    if (!terminal_) throw std::logic_error("Frontend visual reads are still pending");
    return assets_;
}
}
