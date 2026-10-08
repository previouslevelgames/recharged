#pragma once

// Retail public SDK headers may include this C++ transport inside extern "C".
extern "C++" {

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mscharged::platform
{
// TPL fields remain the original four-byte cached-address cells. The native
// decoder checks the actual allocation and completed byte domain before a
// source pointer expression can borrow their native address.
void* DecodeNativeTPLAddress(std::uint32_t word, std::size_t bytes,
                             std::size_t alignment, bool header);

// The original layout compares a raw relative descriptor cell to the Wii
// cached-address boundary before TPLBind. Read that cell in its recorded byte
// domain, without decoding/dereferencing its not-yet-relocated target.
bool NativeTPLAddressLessThan(const void* cell, std::uintptr_t boundary);

template<class T> struct NativeTPLAddress32
{
    std::uint32_t word;
    T* Get() const
    {
        if constexpr (std::is_void_v<T>)
            return static_cast<T*>(DecodeNativeTPLAddress(word, 1, 1, false));
        else
            return static_cast<T*>(DecodeNativeTPLAddress(
                word, sizeof(T), alignof(T), !std::is_same_v<std::remove_cv_t<T>, char>));
    }
    bool operator<(T* boundary) const
    {
        return NativeTPLAddressLessThan(this, reinterpret_cast<std::uintptr_t>(boundary));
    }
    // Original TPLBind tests relative address cells against NULL before their
    // relocation. A zero test must not decode a not-yet-bound relative word.
    bool operator==(T* other) const { return other ? Get() == other : word == 0; }
    bool operator!=(T* other) const { return !(*this == other); }
    operator T*() const { return Get(); }
    T* operator->() const { return Get(); }
};
static_assert(sizeof(NativeTPLAddress32<char>) == 4);
static_assert(alignof(NativeTPLAddress32<char>) == 4);
static_assert(std::is_trivially_copyable_v<NativeTPLAddress32<char>>);
static_assert(std::is_standard_layout_v<NativeTPLAddress32<char>>);

// Temporary native scalar/header views beneath whole original TPLBind.
// Original source decides null/unpacked branches and owns the relocation loop.
// Relative words become real cached SDK addresses; pixel bytes remain raw.
// Commit publishes only source-visited structural records, after all bounds
// and host metadata reservations succeed. The raw source owner stays retained.
class NativeTPLBinding
{
public:
    explicit NativeTPLBinding(void* palette);
    ~NativeTPLBinding();
    NativeTPLBinding(const NativeTPLBinding&) = delete;
    NativeTPLBinding& operator=(const NativeTPLBinding&) = delete;
    void* Palette() const noexcept;
    std::uint32_t RelocateDescriptors(std::uint32_t relative, std::uint32_t count);
    std::uint32_t RelocateTexture(std::uint32_t relative);
    std::uint32_t RelocatePixels(std::uint32_t relative, const void* header);
    std::uint32_t RelocateClut(std::uint32_t relative);
    std::uint32_t RelocateClutPixels(std::uint32_t relative);
    void Commit();
private:
    void* pending_;
};
}

} // C++ linkage
