#include "runtime/frontend_world_files.h"
#include "runtime/whole_file.h"
#include "runtime/game_config.h"
#include "resources/compressed_asset.h"
#include "NL/MemAlloc.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"

namespace mscharged
{
namespace
{
constexpr const char* paths[]{"art/fe/environments/main/gameworld.res.zlib",
    "art/fe/environments/main/gameworld.tmp.zlib", "ini/stadiums/FEWorld.ini"};
struct FreeBuffer { void operator()(void* buffer) const noexcept { nlFree(buffer); } };
bool Loading(FrontendWorldFileState state) { return state <= FrontendWorldFileState::Tweaks; }
}
void FrontendWorldFileLoad::CheckThread() const
{
    if (thread_ != std::this_thread::get_id())
        throw std::logic_error("Frontend world reads require their NL servicing thread");
}
FrontendWorldFileLoad::FrontendWorldFileLoad() : files_(std::make_shared<FrontendWorldFiles>())
{
    if (!gMemoryInitialized || !nlFileSystemReady())
        throw std::logic_error("Frontend world reads require initialized memory and NL files");
    try { Start(); } catch (...) { Drain(); throw; }
}
FrontendWorldFileLoad::~FrontendWorldFileLoad()
{ try { Cancel(); } catch (...) { std::terminate(); } }
void FrontendWorldFileLoad::Start()
{
    const auto* path = paths[static_cast<unsigned>(state_)];
    {
        std::unique_ptr<nlFile> file(nlOpen(path));
        if (!file) throw std::runtime_error(std::string("Frontend world file is missing: ") + path);
        const auto size = nlFileSize(file.get(), nullptr);
        if (!size || size > resources::MaximumAssetBytes)
            throw std::length_error("Frontend world file is empty or exceeds its size limit");
    }
    complete_ = false;
    token_ = nlLoadEntireFileAsync(path, Complete, this, 32, AllocateEnd, nullptr, 0, &VirtualAllocator);
    if (!token_ && !complete_) throw std::runtime_error("Frontend world read was not queued");
}
void FrontendWorldFileLoad::Complete(void* data, unsigned long size, void* context)
{
    std::unique_ptr<void, FreeBuffer> buffer(data);
    auto& load = *static_cast<FrontendWorldFileLoad*>(context);
    load.CheckThread(); load.token_ = 0; load.complete_ = true;
    try
    {
        const resources::Bytes bytes{static_cast<const std::uint8_t*>(data), size};
        switch (load.state_)
        {
        case FrontendWorldFileState::Resident: load.files_->resident = resources::InflateAsset(bytes); break;
        case FrontendWorldFileState::Temporary: load.files_->temporary = resources::InflateAsset(bytes); break;
        case FrontendWorldFileState::Tweaks:
            resources::Require(size <= resources::MaximumAssetBytes, "Frontend tweak file exceeds its size limit");
            ValidateConfigInput(static_cast<const char*>(data), static_cast<int>(size));
            load.files_->tweaks.assign(static_cast<const char*>(data), size);
            break;
        default: throw std::logic_error("Frontend world callback has no active stage");
        }
    }
    catch (...) { load.error_ = std::current_exception(); }
}
void FrontendWorldFileLoad::Drain()
{ if (token_) nlCancelEntireFileLoad(token_, nullptr); token_ = 0; }
void FrontendWorldFileLoad::Poll()
{
    CheckThread(); if (!Loading(state_)) return;
    try
    {
        if (error_) std::rethrow_exception(error_);
        if (complete_)
        {
            state_ = static_cast<FrontendWorldFileState>(static_cast<unsigned>(state_) + 1);
            if (Loading(state_)) Start();
        }
        else if (!WholeFileLoadPending(token_))
            throw std::runtime_error("Frontend world read failed or file services stopped");
    }
    catch (...)
    {
        error_ = std::current_exception(); Drain(); files_.reset(); state_ = FrontendWorldFileState::Failed;
    }
}
void FrontendWorldFileLoad::Service()
{
    CheckThread(); Poll(); if (!Loading(state_)) return;
    try { nlServiceFileSystem(); } catch (...) { Poll(); throw; }
    Poll();
}
void FrontendWorldFileLoad::Cancel()
{
    CheckThread(); if (!Loading(state_)) return;
    Drain(); files_.reset(); state_ = FrontendWorldFileState::Cancelled;
}
FrontendWorldFileState FrontendWorldFileLoad::State() const { CheckThread(); return state_; }
std::shared_ptr<const FrontendWorldFiles> FrontendWorldFileLoad::Result() const
{
    CheckThread();
    if (error_) std::rethrow_exception(error_);
    if (state_ != FrontendWorldFileState::Ready)
        throw std::logic_error("Frontend world files are pending or cancelled");
    return files_;
}
}
