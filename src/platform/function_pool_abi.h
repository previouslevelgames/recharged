#pragma once

#include <cstddef>
#include <type_traits>

#include "NL/nlFunctionMemory.h"

#ifdef MSCHARGED_DIAGNOSTIC_EVENTS
#error Original typed function-pool ABI cannot use the diagnostic event profile
#endif

template <int N> struct Placeholder;
template <typename R, typename F, typename A> struct BindExp1;
template <typename R, typename F, typename A, typename B> struct BindExp2;
template <typename R, typename F, typename A, typename B, typename C> struct BindExp3;
template <typename R, typename F, typename A, typename B, typename C, typename D> struct BindExp4;
template <typename S> class Function;
template <typename R> class Function0;
template <typename R, typename P1> class Function1;
template <typename R, typename P1, typename P2> class Function2;
template <typename R, typename P1, typename P2, typename P3> class Function3;
namespace Detail { template <typename R, typename Member> struct MemFunImpl; }
struct AudioEffectSoundStartedVisitor;

namespace mscharged::function_abi {

// These describe the original Wii compiler's in-memory ABI, not disc records.
// Unknown class layouts deliberately have no usable original capture footprint.
template <std::size_t Size, std::size_t Alignment>
struct Layout {
    static constexpr bool known = true;
    static constexpr std::size_t size = Size;
    static constexpr std::size_t alignment = Alignment;
};

template <typename T, typename Enable = void>
struct WiiCapture {
    static constexpr bool known = false;
    static constexpr std::size_t size = 0;
    static constexpr std::size_t alignment = 1;
};

template <typename T> struct WiiCapture<const T> : WiiCapture<T> {};
template <typename T> struct WiiCapture<volatile T> : WiiCapture<T> {};
template <typename T> struct WiiCapture<const volatile T> : WiiCapture<T> {};
template <typename T> struct WiiCapture<T*> : Layout<4, 4> {};
template <typename T> struct WiiCapture<T&> : Layout<4, 4> {};

template <> struct WiiCapture<bool> : Layout<1, 1> {};
template <> struct WiiCapture<char> : Layout<1, 1> {};
template <> struct WiiCapture<signed char> : Layout<1, 1> {};
template <> struct WiiCapture<unsigned char> : Layout<1, 1> {};
template <> struct WiiCapture<short> : Layout<2, 2> {};
template <> struct WiiCapture<unsigned short> : Layout<2, 2> {};
template <> struct WiiCapture<int> : Layout<4, 4> {};
template <> struct WiiCapture<unsigned int> : Layout<4, 4> {};
template <> struct WiiCapture<long> : Layout<4, 4> {};
template <> struct WiiCapture<unsigned long> : Layout<4, 4> {};
template <> struct WiiCapture<float> : Layout<4, 4> {};
template <typename T> constexpr bool ReviewedEnum() {
    if constexpr (std::is_enum_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>)
        return std::is_same_v<std::underlying_type_t<T>, int>
            || std::is_same_v<std::underlying_type_t<T>, unsigned int>;
    return false;
}
template <typename T> struct WiiCapture<T, std::enable_if_t<ReviewedEnum<T>()>> : Layout<4, 4> {};

// Actual Runtime/ptmf.c contains the three-word MWCC member-function pointer.
template <typename T>
struct WiiCapture<T, std::enable_if_t<std::is_member_function_pointer_v<T>
    && !std::is_const_v<T> && !std::is_volatile_v<T>>> : Layout<12, 4> {};

constexpr std::size_t Align(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

template <typename... Fields>
struct Record {
    static constexpr bool known = (WiiCapture<Fields>::known && ...);
    static constexpr std::size_t alignment = [] {
        std::size_t result = 1;
        ((result = result < WiiCapture<Fields>::alignment ? WiiCapture<Fields>::alignment : result), ...);
        return result;
    }();
    static constexpr std::size_t size = [] {
        std::size_t result = 0;
        ((result = Align(result, WiiCapture<Fields>::alignment) + WiiCapture<Fields>::size), ...);
        return Align(result, alignment);
    }();
};

template <int N> struct WiiCapture<Placeholder<N>> : Layout<1, 1> {};
template <typename R, typename Member>
struct WiiCapture<Detail::MemFunImpl<R, Member>> : WiiCapture<Member> {};
template <typename R, typename F, typename A>
struct WiiCapture<BindExp1<R, F, A>> : Record<F, A> {};
template <typename R, typename F, typename A, typename B>
struct WiiCapture<BindExp2<R, F, A, B>> : Record<F, A, B> {};
template <typename R, typename F, typename A, typename B, typename C>
struct WiiCapture<BindExp3<R, F, A, B, C>> : Record<F, A, B, C> {};
template <typename R, typename F, typename A, typename B, typename C, typename D>
struct WiiCapture<BindExp4<R, F, A, B, C, D>> : Record<F, A, B, C, D> {};

// Every original Function wrapper contains the four-byte tag and pointer union;
// its source-derived specializations add no fields.
template <typename S> struct WiiCapture<Function<S>> : Layout<8, 4> {};
template <typename R> struct WiiCapture<Function0<R>> : Layout<8, 4> {};
template <typename R, typename P1> struct WiiCapture<Function1<R, P1>> : Layout<8, 4> {};
template <typename R, typename P1, typename P2> struct WiiCapture<Function2<R, P1, P2>> : Layout<8, 4> {};
template <typename R, typename P1, typename P2, typename P3> struct WiiCapture<Function3<R, P1, P2, P3>> : Layout<8, 4> {};

// AudioScriptRuntime.h explicitly records offsets0/4/8 and total Wii size12.
// Its only captured fields are free-function pointer, bool, effect pointer.
template <> struct WiiCapture<::AudioEffectSoundStartedVisitor> : Record<void*, bool, void*> {};

// Physical strides are separate from source categories and counts. The source
// visitor, no-data Queue and data Queue respectively require32,56,64 on the
// qualified ELF64 ABI. Every typed instantiation verifies its actual footprint.
template <std::size_t Category> struct NativeStride;
template <> struct NativeStride<16> : std::integral_constant<std::size_t, 32> {};
template <> struct NativeStride<32> : std::integral_constant<std::size_t, 56> {};
template <> struct NativeStride<64> : std::integral_constant<std::size_t, 64> {};

template <typename Callable, typename Functor>
struct Descriptor {
    static_assert(WiiCapture<Callable>::known,
        "Original function capture has no reviewed Wii field-layout descriptor");
    // Original FunctorBase contributes one Wii vptr; Callable is the sole field.
    static constexpr std::size_t wii_size = Record<void*, Callable>::size;
    static constexpr std::size_t category = wii_size <= 16 ? 16 : (wii_size <= 32 ? 32 : 64);
    static constexpr std::size_t stride = NativeStride<category>::value;
    static_assert(wii_size <= 64,
        "Original capture exceeds the reviewed Wii function-pool storage");
    static_assert(sizeof(Functor) <= stride,
        "Original function capture exceeds the reviewed native pool stride");
    static_assert(alignof(Functor) <= 8 && stride % alignof(Functor) == 0,
        "Original function capture requires unqualified native pool alignment");
};

template <typename Callable, typename Functor>
inline void* Allocate() {
    return AllocateFunctionMemory(Descriptor<Callable, Functor>::wii_size);
}

template <typename Callable, typename Functor>
inline void Free(void* pointer) {
    FreeFunctionMemory(pointer, Descriptor<Callable, Functor>::wii_size);
}

} // namespace mscharged::function_abi
