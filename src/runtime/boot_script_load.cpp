#include "runtime/boot_script_load.h"
#include "runtime/whole_file.h"
#include "NL/MemAlloc.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"

namespace mscharged
{
namespace
{
constexpr const char* ScriptPath = "art/Scripts/async_loading.byte_code";
struct FreeBuffer { void operator()(void* data) const noexcept { nlFree(data); } };
}
void BootScriptLoad::CheckThread() const
{
    if (thread_ != std::this_thread::get_id())
        throw std::logic_error("Boot script requires its NL servicing thread");
}
BootScriptLoad::BootScriptLoad()
{
    if (!gMemoryInitialized || !nlFileSystemReady())
        throw std::logic_error("Boot script requires initialized memory and NL files");
    {
        std::unique_ptr<nlFile> file(nlOpen(ScriptPath));
        if (!file) throw std::runtime_error("Boot loading script is missing");
        const auto size = nlFileSize(file.get(), nullptr);
        if (size < 72 || size > resources::MaximumAssetBytes)
            throw std::length_error("Boot loading script exceeds its file size limits");
    }
    try
    {
        token_ = nlLoadEntireFileAsync(ScriptPath, Complete, this, 32, AllocateEnd,
                                      nullptr, 0, &VirtualAllocator);
        if (!token_ && !completed_) throw std::runtime_error("Boot script read was not queued");
    }
    catch (...) { Drain(); throw; }
}
BootScriptLoad::~BootScriptLoad()
{ try { Cancel(); } catch (...) { std::terminate(); } }
void BootScriptLoad::Complete(void* data, unsigned long size, void* context)
{
    std::unique_ptr<void, FreeBuffer> buffer(data);
    auto& load = *static_cast<BootScriptLoad*>(context);
    load.CheckThread(); load.token_ = 0; load.completed_ = true;
    try
    {
        const resources::Bytes bytes{static_cast<const std::uint8_t*>(data), size};
        auto asset = std::make_shared<BootScriptAsset>();
        asset->script = resources::ReadScriptBytecode(bytes);
        asset->bytes.assign(bytes.begin(), bytes.end());
        load.asset_ = std::move(asset);
    }
    catch (...) { load.error_ = std::current_exception(); }
}
void BootScriptLoad::Drain()
{
    if (token_) nlCancelEntireFileLoad(token_, nullptr);
    token_ = 0;
}
void BootScriptLoad::Poll()
{
    CheckThread();
    if (state_ != BootScriptLoadState::Loading) return;
    if (!completed_ && !WholeFileLoadPending(token_) && !error_)
        error_ = std::make_exception_ptr(std::runtime_error("Boot script read failed or file services stopped"));
    if (error_)
    {
        Drain(); asset_.reset(); state_ = BootScriptLoadState::Failed;
    }
    else if (completed_) state_ = BootScriptLoadState::Ready;
}
void BootScriptLoad::Service()
{
    CheckThread(); Poll();
    if (state_ != BootScriptLoadState::Loading) return;
    try { nlServiceFileSystem(); }
    catch (...) { Poll(); throw; }
    Poll();
}
void BootScriptLoad::Cancel()
{
    CheckThread(); Poll();
    if (state_ != BootScriptLoadState::Loading) return;
    Drain(); asset_.reset(); state_ = BootScriptLoadState::Cancelled;
}
BootScriptLoadState BootScriptLoad::State() const { CheckThread(); return state_; }
std::shared_ptr<const BootScriptAsset> BootScriptLoad::Result() const
{
    CheckThread();
    if (error_) std::rethrow_exception(error_);
    if (state_ != BootScriptLoadState::Ready)
        throw std::logic_error("Boot script is pending or cancelled");
    return asset_;
}
}
