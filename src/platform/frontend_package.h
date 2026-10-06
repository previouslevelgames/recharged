#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mscharged::platform
{
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
    std::vector<std::byte> bytes_;
    std::vector<Record> records_;
    std::vector<std::size_t> animation_offsets_;
};

// The input domain is explicitly untouched Wii FENL/version1 bytes.
PreparedFrontendPackage PrepareFrontendPackage(const void*, std::size_t);
}
