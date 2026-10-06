#pragma once

#include <cstddef>
#include <cstdint>

class GLResourcePool;

namespace mscharged::platform
{
// Native representations of explicitly identified Wii pool records. One
// unchanged source allocation charges the Wii footprint/alignment to the real
// pool; the host representation belongs to that exact pool/quota incarnation.
// Unknown record types keep their existing source request, never a guessed
// pool-capacity multiplier. Both metadata and native storage are reserved
// before the source pool mutates its quota.
void* AllocateGameResourceRecord(GLResourcePool* pool, std::size_t wiiBytes,
                                std::size_t nativeBytes);
// Call only after the original inventory callbacks and source usage rewind.
void RetireGameResourceRecords(GLResourcePool* pool, const void* quotaBegin,
                              std::size_t quotaBytes) noexcept;
// The original derived pool frees its arena backing before the base inventory
// callbacks. Retail arena payload remains mapped during those callbacks; native
// representations likewise survive until the actual base destructor finishes.
void ReleaseGameResourceRecords(GLResourcePool* pool) noexcept;

// Read-only storage facts for ABI fixtures; no resource/game readiness signal.
struct GameResourceRecordInfo
{
    const void* native;
    std::size_t nativeBytes;
    const void* quota;
    std::size_t wiiBytes;
    std::uint64_t poolIncarnation;
    std::uint64_t quotaIncarnation;
    std::uint64_t quotaStorageIncarnation;
};
bool FindGameResourceRecord(const void* native, GameResourceRecordInfo& info) noexcept;
}
