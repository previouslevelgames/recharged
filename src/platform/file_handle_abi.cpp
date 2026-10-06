#include "platform/file_handle_abi.h"
#include "platform/host_metadata.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace mscharged::platform {
namespace {
struct ReadIdentity {
    ReadIdentity* next;
    void* entry;
    std::uint32_t handle;
};
ReadIdentity* identities;
std::uint64_t nextHandle = 1;
}

struct FileLoadLease {
    FileLoadLease* next;
    void* context;
    void* file;
    FileLoadCleanup cleanup;
    ReadIdentity* reservedIdentity;
};

namespace {
FileLoadLease* loads;

FileLoadLease* Detach(void* context)
{
    auto** link = &loads;
    while (*link && (*link)->context != context) link = &(*link)->next;
    if (!*link) return nullptr;
    auto* lease = *link;
    *link = lease->next;
    return lease;
}

void Release(FileLoadLease* lease)
{
    ChargedNativeMetadataRelease(lease->reservedIdentity);
    ChargedNativeMetadataRelease(lease);
}

void Cleanup(FileLoadLease* lease)
{
    // Detach before invoking original cleanup, which can close pending reads.
    auto cleanup = lease->cleanup;
    auto* context = lease->context;
    Release(lease);
    cleanup(context);
}
}

FileLoadLease* ReserveFileLoad(void* context, void* file, FileLoadCleanup cleanup)
{
    if (!context || !file || !cleanup)
        throw std::invalid_argument("Invalid original whole-file owner");
    for (auto* current = loads; current; current = current->next)
        if (current->context == context)
            throw std::logic_error("Original whole-file owner already registered");
    if (nextHandle > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Native NL read handle identities exhausted");
    auto* lease = static_cast<FileLoadLease*>(ChargedNativeMetadataAllocate(sizeof(FileLoadLease)));
    ReadIdentity* reserved;
    try {
        reserved = static_cast<ReadIdentity*>(ChargedNativeMetadataAllocate(sizeof(ReadIdentity)));
    } catch (...) {
        ChargedNativeMetadataRelease(lease);
        throw;
    }
    *lease = {loads, context, file, cleanup, reserved};
    loads = lease;
    return lease;
}

std::uint32_t BindFileLoad(FileLoadLease* lease, void* actualReadEntry)
{
    if (!lease || !actualReadEntry)
        throw std::invalid_argument("Unrepresented original async read entry");
    for (auto* identity = identities; identity; identity = identity->next) {
        if (identity->entry == actualReadEntry) {
            ChargedNativeMetadataRelease(lease->reservedIdentity);
            lease->reservedIdentity = nullptr;
            return identity->handle;
        }
    }
    // Storage was reserved before submitting the actual read. No allocation
    // or truncation occurs after workers have acquired the destination.
    auto* identity = lease->reservedIdentity;
    if (!identity || nextHandle > std::numeric_limits<std::uint32_t>::max())
        throw std::logic_error("Native NL handle reservation unavailable");
    *identity = {identities, actualReadEntry, static_cast<std::uint32_t>(nextHandle++)};
    identities = identity;
    lease->reservedIdentity = nullptr;
    return identity->handle;
}

void* ResolveFileReadHandle(std::uint32_t handle)
{
    for (auto* identity = identities; identity; identity = identity->next)
        if (identity->handle == handle) return identity->entry;
    throw std::invalid_argument("Unknown native NL whole-file handle");
}

void FinishFileLoad(void* context)
{
    auto* lease = Detach(context);
    if (!lease) throw std::logic_error("Original whole-file callback has no owner");
    Release(lease);
}

void AbortFileLoad(void* context)
{
    if (auto* lease = Detach(context)) Cleanup(lease);
}

void AbortFileLoadsForFile(void* file)
{
    for (;;) {
        auto* lease = loads;
        while (lease && lease->file != file) lease = lease->next;
        if (!lease) return;
        Cleanup(Detach(lease->context));
    }
}

void ShutdownFileLoads()
{
    while (loads) Cleanup(Detach(loads->context));
    while (identities) {
        auto* identity = identities;
        identities = identity->next;
        ChargedNativeMetadataRelease(identity);
    }
    // Keep nextHandle monotonic across raw-pool resets: a previous lifecycle's
    // handle must never become a pointer into a newly allocated raw pool.
}
}
