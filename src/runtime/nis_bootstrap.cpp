#include "runtime/nis_bootstrap.h"
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
constexpr std::array paths{
    "art/Scripts/nis_triggers.byte_code", "art/Scripts/nis_anim_proxy.byte_code",
    "art/nis/do_not_mirror.txt", "art/nis/do_not_showPiP.txt", "art/nis/nis_dict.txt"};
struct FreeBuffer { void operator()(void* data) const noexcept { nlFree(data); } };
void CheckFile(const char* filename)
{
    std::unique_ptr<nlFile> file(nlOpen(filename));
    if (!file) throw std::runtime_error(std::string("NIS bootstrap file is missing: ") + filename);
    if (nlFileSize(file.get(), nullptr) > resources::MaximumAssetBytes)
        throw std::length_error("NIS bootstrap file exceeds the asset limit");
}
}
void NisBootstrapLoad::CheckThread() const
{
    if (thread_ != std::this_thread::get_id())
        throw std::logic_error("NIS bootstrap requires its NL servicing thread");
}
NisBootstrapLoad::NisBootstrapLoad() : assets_(std::make_shared<NisBootstrapAssets>())
{
    if (!gMemoryInitialized || !nlFileSystemReady())
        throw std::logic_error("NIS bootstrap requires initialized memory and NL files");
    // Validate every size/path before submitting any work.
    for (const char* path : paths) CheckFile(path);
    try
    {
        for (unsigned i = 0; i < requests_.size(); ++i)
        {
            auto& request = requests_[i];
            request.owner = this; request.index = i; request.started = true;
            request.token = nlLoadEntireFileAsync(paths[i], Complete, &request,
                32, AllocateEnd, nullptr, 0, &VirtualAllocator);
            if (!request.token && !request.complete)
                throw std::runtime_error("NIS bootstrap read was not queued");
            if (error_) std::rethrow_exception(error_);
        }
    }
    catch (...) { Drain(); throw; }
}
NisBootstrapLoad::~NisBootstrapLoad()
{
    try { Cancel(); } catch (...) { std::terminate(); }
}
void NisBootstrapLoad::Complete(void* data, unsigned long size, void* context)
{
    std::unique_ptr<void, FreeBuffer> buffer(data);
    auto& request = *static_cast<Request*>(context);
    auto& owner = *request.owner;
    owner.CheckThread();
    request.complete = true; request.token = 0;
    try
    {
        const resources::Bytes bytes{static_cast<const std::uint8_t*>(data), size};
        resources::Require(size <= resources::MaximumAssetBytes, "NIS bootstrap file exceeds the asset limit");
        switch (request.index)
        {
        case 0:
        case 1:
        {
            // Bytecode execution and relocation have a separate native contract.
            // Retain the complete original file; never patch addresses in place.
            resources::Require(size >= 72 && resources::U32(bytes, 0) == 0xe11c2112,
                               "Invalid NIS bytecode header");
            auto& destination = request.index ? owner.assets_->animation_proxy : owner.assets_->triggers;
            destination.assign(bytes.begin(), bytes.end());
            break;
        }
        case 2: owner.assets_->no_mirror = resources::ReadNisNameList(bytes); break;
        case 3: owner.assets_->no_picture_in_picture = resources::ReadNisNameList(bytes); break;
        case 4: owner.assets_->dictionary = resources::ReadNisDictionary(bytes); break;
        default: throw std::logic_error("Unknown NIS bootstrap resource");
        }
        owner.completed_mask_ |= 1u << request.index;
    }
    catch (...) { if (!owner.error_) owner.error_ = std::current_exception(); }
}
void NisBootstrapLoad::Drain()
{
    for (auto& request : requests_)
    {
        if (request.token) nlCancelEntireFileLoad(request.token, nullptr);
        request.token = 0;
    }
}
void NisBootstrapLoad::Poll()
{
    CheckThread();
    if (terminal_) return;
    for (const auto& request : requests_)
        if (request.started && !request.complete && !WholeFileLoadPending(request.token) && !error_)
            error_ = std::make_exception_ptr(std::runtime_error("NIS bootstrap read failed or file services stopped"));
    if (error_)
    {
        Drain(); assets_.reset(); terminal_ = true;
    }
    else if (completed_mask_ == 31) terminal_ = true;
}
void NisBootstrapLoad::Service()
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
void NisBootstrapLoad::Cancel()
{
    CheckThread();
    if (terminal_) return;
    Poll();
    if (terminal_) return;
    Drain(); assets_.reset(); cancelled_ = terminal_ = true;
}
bool NisBootstrapLoad::Ready() const { CheckThread(); return terminal_; }
unsigned NisBootstrapLoad::CompletedMask() const { CheckThread(); return completed_mask_; }
std::shared_ptr<const NisBootstrapAssets> NisBootstrapLoad::Result() const
{
    CheckThread();
    if (error_) std::rethrow_exception(error_);
    if (cancelled_) throw std::runtime_error("NIS bootstrap was cancelled");
    if (!terminal_) throw std::logic_error("NIS bootstrap reads are still pending");
    return assets_;
}
}
