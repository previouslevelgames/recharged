#include "runtime/camera_batch.h"
#include "NL/nlFileGC.h"
#include <stdexcept>

namespace mscharged
{
void CameraAssetBatch::CheckThread() const
{
    if (thread_ != std::this_thread::get_id())
        throw std::logic_error("Camera batch requires its NL servicing thread");
}
CameraAssetBatch::CameraAssetBatch(CameraAssetLibrary& library, std::span<const CameraBatchRequest> requests)
    : library_(library)
{
    if (requests.empty() || requests.size() > MaximumCameraBatchRequests)
        throw std::invalid_argument("Camera batch requires 1..64 named requests");
    std::vector<std::string> aliases;
    aliases.reserve(requests.size());entries_.reserve(requests.size());
    for (const auto& request : requests)
    {
        if (request.filename.empty() || request.filename.size() > 2048
            || request.filename.find('\0') != std::string::npos)
            throw std::invalid_argument("Camera batch filename must contain 1..2048 non-NUL bytes");
        aliases.push_back(CanonicalCameraAlias(request.alias));
        entries_.push_back({{aliases.back(), request.filename},{},{}});
    }
    library_.CheckAvailableAliases(aliases); // Validate the whole transaction before I/O.
    progress_.requested = progress_.pending = entries_.size();
    Start();
}
CameraAssetBatch::~CameraAssetBatch()
{
    try { Cancel(); } catch (...) { std::terminate(); }
}
void CameraAssetBatch::Collect()
{
    // Inspect all completions before aborting so already delivered callbacks
    // count consistently regardless of their position in the request list.
    for (auto& entry : entries_)
    {
        if (entry.state != EntryState::Pending || !entry.load || !entry.load->Ready()) continue;
        --progress_.active;--progress_.pending;++progress_.completed;
        try
        {
            entry.asset = entry.load->Result();
            if (!entry.asset) throw std::runtime_error("Camera request completed without an asset");
            entry.state = EntryState::Succeeded;++progress_.succeeded;
        }
        catch (...)
        {
            entry.state = EntryState::Failed;++progress_.failed;
            if (!error_) { error_ = std::current_exception();failed_alias_ = entry.request.alias; }
        }
        entry.load.reset();
    }
}
void CameraAssetBatch::Abort()
{
    for (auto& entry : entries_)
    {
        if (entry.state == EntryState::Pending)
        {
            if (entry.load) { entry.load->Cancel();entry.load.reset();--progress_.active; }
            entry.state = EntryState::Cancelled;--progress_.pending;++progress_.cancelled;
        }
        entry.asset.reset();
    }
}
void CameraAssetBatch::Start()
{
    while (next_ < entries_.size() && progress_.active < MaximumActiveCameraBatchRequests)
    {
        auto& entry = entries_[next_++];
        try
        {
            entry.load = std::make_unique<CameraAssetLoad>(entry.request.filename.c_str(), entry.request.alias);
            ++progress_.active;
        }
        catch (...)
        {
            entry.state = EntryState::Failed;--progress_.pending;++progress_.completed;++progress_.failed;
            error_ = std::current_exception();failed_alias_ = entry.request.alias;
        }
        Collect(); // Includes the original synchronous empty-file completion.
        if (error_) { state_ = CameraBatchState::Failed;Abort();return; }
    }
    if (!progress_.pending) state_ = CameraBatchState::Ready;
}
void CameraAssetBatch::Poll()
{
    CheckThread();
    if (state_ != CameraBatchState::Loading) return;
    Collect();
    if (error_) { state_ = CameraBatchState::Failed;Abort();return; }
    Start();
}
void CameraAssetBatch::Service()
{
    CheckThread();Poll();
    if (state_ != CameraBatchState::Loading) return;
    try { nlServiceFileSystem(); }
    catch (...)
    {
        const auto service_error = std::current_exception();
        Poll();
        // A shared pump can fail for someone else's request, including while
        // our own callbacks finish. Never hide that external error signal.
        std::rethrow_exception(service_error);
    }
    Poll();
}
void CameraAssetBatch::Cancel()
{
    CheckThread();
    if (state_ == CameraBatchState::Failed || state_ == CameraBatchState::Cancelled
        || state_ == CameraBatchState::Published) return;
    Collect();
    state_ = error_ ? CameraBatchState::Failed : CameraBatchState::Cancelled;
    Abort();
}
void CameraAssetBatch::Publish()
{
    CheckThread();
    if (state_ == CameraBatchState::Published) return;
    if (error_) std::rethrow_exception(error_);
    if (state_ == CameraBatchState::Cancelled) throw std::runtime_error("Camera batch cancelled");
    if (state_ != CameraBatchState::Ready) throw std::logic_error("Camera batch is still loading");
    try
    {
        std::vector<CameraAsset::Handle> assets;
        assets.reserve(entries_.size());
        for (const auto& entry : entries_) assets.push_back(entry.asset);
        library_.InsertAll(assets); // Rechecks conflicts before the transactional swap.
    }
    catch (...) { error_ = std::current_exception();state_ = CameraBatchState::Failed;Abort();throw; }
    state_ = CameraBatchState::Published;
    for (auto& entry : entries_) entry.asset.reset();
}
CameraBatchState CameraAssetBatch::State() const { CheckThread();return state_; }
CameraBatchProgress CameraAssetBatch::Progress() const { CheckThread();return progress_; }
const std::string& CameraAssetBatch::FailedAlias() const { CheckThread();return failed_alias_; }
}
