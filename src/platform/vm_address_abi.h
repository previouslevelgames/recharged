#pragma once
#include <cstdint>
#include <type_traits>

namespace mscharged::platform
{
// Original VM words and serialized pointer slots remain exactly four bytes.
// Numeric stack words never pass through these address-only transport calls.
std::uint32_t EncodeVMAddress(const void* address);
// Address-only VM producers require a live original allocation byte.
// This does not qualify the pointed object layout or script dispatcher.
std::uint32_t EncodeOwnedVMAddress(const void* address);
void* DecodeVMAddress(std::uint32_t word);
void PrepareVMBytecode(void* bytes);
std::uint32_t ReadVMNativeWord(const void* bytes);

template <class T> struct VMAddress32
{
    std::uint32_t word;
    T* Get() const { return static_cast<T*>(DecodeVMAddress(word)); }
    operator T*() const { return Get(); }
    VMAddress32& operator=(T* address)
    {
        word = EncodeVMAddress(address);
        return *this;
    }
};
static_assert(sizeof(VMAddress32<unsigned char>) == 4);
static_assert(alignof(VMAddress32<unsigned char>) == 4);
static_assert(std::is_standard_layout_v<VMAddress32<unsigned char>>);
static_assert(std::is_trivially_copyable_v<VMAddress32<unsigned char>>);
}
