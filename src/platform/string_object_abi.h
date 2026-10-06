#pragma once

#if defined(MSCHARGED_GAME_MODULE) && defined(MSCHARGED_DIAGNOSTIC_VECTORS)
#error An original game module cannot use diagnostic string/vector algorithms
#endif

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace Detail { class TempStringPoolAllocator; }

namespace mscharged::string_abi {
// Original pool payloads are base+4. Carry a native pointer as bytes so the
// original Vector/Data objects retain four-byte alignment and source methods.
template<class T> class alignas(4) PoolPointer {
public:
    PoolPointer() = default;
    PoolPointer(T* pointer) noexcept { Store(pointer); }
    PoolPointer& operator=(T* pointer) noexcept { Store(pointer); return *this; }
    operator T*() const noexcept {
        T* pointer;
        std::memcpy(&pointer, bytes, sizeof(pointer));
        return pointer;
    }
private:
    void Store(T* pointer) noexcept { std::memcpy(bytes, &pointer, sizeof(pointer)); }
    static_assert(sizeof(T*) == 4 || sizeof(T*) == 8,
                  "Original temporary string pointer carrier requires a reviewed native pointer width");
    std::byte bytes[sizeof(T*)];
};

template<class T, class Allocator>
using VectorPointer = std::conditional_t<std::is_same_v<Allocator, Detail::TempStringPoolAllocator>,
                                        PoolPointer<T>, T*>;
} // namespace mscharged::string_abi
