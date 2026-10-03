#pragma once
#include "runtime/camera_assets.h"
#include <cstddef>
#include <vector>

namespace mscharged
{
inline constexpr std::size_t MaximumCameraBatchRequests = 64;
inline constexpr std::size_t MaximumActiveCameraBatchRequests = 8;
struct CameraBatchRequest { std::string alias, filename; };
enum class CameraBatchState { Loading, Ready, Published, Failed, Cancelled };
struct CameraBatchProgress
{
    std::size_t requested = 0, completed = 0, succeeded = 0, failed = 0, cancelled = 0;
    std::size_t pending = 0, active = 0;
};

// Owns a bounded transaction on the original NL servicing thread. The library
// and game memory must outlive this owner; release retained handles before arena
// shutdown. Only Publish inserts into the library. No original global factory,
// cAnimCamera registry, simulation update or camera selection is performed here.
class CameraAssetBatch
{
    enum class EntryState { Pending, Succeeded, Failed, Cancelled };
    struct Entry
    {
        CameraBatchRequest request;
        std::unique_ptr<CameraAssetLoad> load;
        CameraAsset::Handle asset;
        EntryState state = EntryState::Pending;
    };
    CameraAssetLibrary& library_;
    std::thread::id thread_ = std::this_thread::get_id();
    std::vector<Entry> entries_;
    CameraBatchState state_ = CameraBatchState::Loading;
    CameraBatchProgress progress_;
    std::exception_ptr error_;
    std::string failed_alias_;
    std::size_t next_ = 0;
    void CheckThread() const;
    void Collect();
    void Abort();
    void Start();
public:
    // Invalid request lists/conflicts throw before I/O. Accepted request failures
    // (including inline completion or allocation failure) become Failed state.
    CameraAssetBatch(CameraAssetLibrary& library, std::span<const CameraBatchRequest> requests);
    ~CameraAssetBatch();
    CameraAssetBatch(const CameraAssetBatch&) = delete;
    CameraAssetBatch& operator=(const CameraAssetBatch&) = delete;
    void Poll(); // Collect externally serviced/inline completions; start queued work.
    // Pumps NL once, then Poll. Reconciles own failures before rethrowing any
    // shared-service exception; unrelated requests retain their error signal.
    void Service();
    void Cancel(); // Collect already completed work, drain workers, discard all staged data.
    void Publish(); // Requires Ready; atomic and idempotent after Published. Rethrows failure.
    CameraBatchState State() const;
    // completed = succeeded + failed; pending + completed + cancelled = requested.
    // Counters advance in construction/Poll/Service/Cancel, never in observers.
    CameraBatchProgress Progress() const;
    const std::string& FailedAlias() const; // Empty for a publication-only failure.
};
}
