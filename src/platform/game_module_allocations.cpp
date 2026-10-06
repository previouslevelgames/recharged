#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Native compiler allocation entries belong only to the original game module
#endif

#include "NL/nlMemory.h"
#include <new>
#include <limits>

// Native compiler entry points missing from MWCC. All dispose through the
// genuine module-local source unsized delete; no CRT fallback for game buffers.
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete[](pointer); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    auto value = static_cast<std::size_t>(alignment);
    if (value > std::numeric_limits<unsigned>::max()) throw std::bad_alloc();
    return nlMalloc(size, static_cast<unsigned>(value), false);
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { ::operator delete[](pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { ::operator delete[](pointer); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }
