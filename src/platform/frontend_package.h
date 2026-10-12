#pragma once
#include "platform/host_metadata.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace mscharged::platform
{
// The transport is compiled into the original game module, whose ordinary
// operators allocate from the original game heap. All of its storage, the
// native image included, stays in the host CRT: game-heap allocations here
// would change the original heap's layout and cost a free-list walk each.
template<class T> struct FrontendPackageAllocator
{
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    using value_type = T;
    FrontendPackageAllocator() noexcept = default;
    template<class U> FrontendPackageAllocator(const FrontendPackageAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        void* storage = ChargedNativeMetadataAllocate(count * sizeof(T));
        if (!storage) throw std::bad_alloc();
        return static_cast<T*>(storage);
    }
    void deallocate(T* pointer, std::size_t) noexcept { ChargedNativeMetadataRelease(pointer); }
    template<class U> bool operator==(const FrontendPackageAllocator<U>&) const noexcept { return true; }
};
template<class T> using FrontendPackageVector = std::vector<T, FrontendPackageAllocator<T>>;

// Temporary serialized-data ABI transport. Original FEScene owns the final
// allocation, loading requests, relocation loop and every subsequent decision.
class PreparedFrontendPackage
{
public:
    struct Record
    {
        std::uint32_t wii_offset;
        std::size_t native_offset;
        std::size_t native_size;
        unsigned kind;
    };
    void* Data() noexcept { return bytes_.data(); }
    const void* Data() const noexcept { return bytes_.data(); }
    std::size_t Size() const noexcept { return bytes_.size(); }
    void StartObjectLifetimes(void* original_package_allocation) const;
    // Data-layout inspection for independent fixtures; no game state/owners.
    std::span<const Record> Records() const noexcept { return records_; }
    std::size_t NativeOffset(std::uint32_t wii_offset) const;

private:
    friend PreparedFrontendPackage PrepareFrontendPackage(const void*, std::size_t);
    FrontendPackageVector<std::byte> bytes_;
    FrontendPackageVector<Record> records_;
    FrontendPackageVector<std::size_t> animation_offsets_;
};

// The input domain is explicitly untouched Wii FENL/version1 bytes.
PreparedFrontendPackage PrepareFrontendPackage(const void*, std::size_t);
}
