#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
class nlChunk;
namespace mscharged::platform
{
// Address-bearing cells retain the actual four-byte Wii cached-address ABI.
// Numeric source fields, chunk geometry, colour bytes and ownership are separate.
std::uint32_t EncodeEffectsAddress(const void* address);
void* DecodeEffectsAddress(std::uint32_t word);

template<class T> struct EffectsAddress32
{
    std::uint32_t word;
    T* Get() const { return static_cast<T*>(DecodeEffectsAddress(word)); }
    operator T*() const { return Get(); }
    T* operator->() const { return Get(); }
    T& operator[](std::size_t index) const { return Get()[index]; }
    EffectsAddress32& operator=(T* address)
    {
        word = EncodeEffectsAddress(address);
        return *this;
    }
};
static_assert(sizeof(EffectsAddress32<unsigned char>) == 4);
static_assert(alignof(EffectsAddress32<unsigned char>) == 4);
static_assert(std::is_trivially_copyable_v<EffectsAddress32<unsigned char>>);
static_assert(std::is_standard_layout_v<EffectsAddress32<unsigned char>>);

// Convert explicitly represented scalar cells in the actual completed source
// allocation. Original loaders still write every pointer, choose every resource,
// resolve templates and parse user effects. All chunk headers/colour bytes stay
// raw; the authored220-byte template's colour25 remains its next raw header.
void PrepareEffectsBundle(nlChunk* bundle);
}
