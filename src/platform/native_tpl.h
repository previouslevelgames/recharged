#pragma once

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
    operator T*() const { return Get(); }
    T* operator->() const { return Get(); }
};
static_assert(sizeof(NativeTPLAddress32<char>) == 4);
static_assert(alignof(NativeTPLAddress32<char>) == 4);
static_assert(std::is_trivially_copyable_v<NativeTPLAddress32<char>>);
static_assert(std::is_standard_layout_v<NativeTPLAddress32<char>>);

// Known original icon/banner TPL transport. No texture pixels are converted;
// the original source copies those exact GX RGB5A3 bytes into its NAND banner.
void BindNativeTPLImage(void* palette);
}

// The original partial TPL declarations use an opaque native parameter only at
// this library boundary. Their palette/table/header strides stay Wii-sized.
extern "C" void TPLBind(void* palette);
