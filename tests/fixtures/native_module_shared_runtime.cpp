// Fixture operators belong to this DLL's real private heap. They are not game
// allocators or providers; std runtime/exception allocation uses shared libc++.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <exception>
#include <new>
#include <stdexcept>
#include "native_module_shared_runtime.h"

namespace {
HANDLE private_heap{};
ModuleStats stats{};
struct Lifetime {
    Lifetime() {
        private_heap = HeapCreate(0, 0, 0);
        if (!private_heap)
            std::terminate();
        fixture_runtime_host_event(ModuleConstructed);
    }
    ~Lifetime() {
        if (stats.live_allocations != 0 || !HeapDestroy(private_heap))
            std::terminate();
        private_heap = nullptr;
        fixture_runtime_host_event(ModuleDestroyed);
    }
} lifetime;
struct Scope {
    ~Scope() {
        ++stats.scope_destructors;
        fixture_runtime_host_event(ScopeDestroyed);
    }
};
class ModuleError final : public std::runtime_error {
public:
    ModuleError() : std::runtime_error("retained DLL standard exception") {}
    ~ModuleError() noexcept override {
        ++stats.exception_destructors;
        fixture_runtime_host_event(ExceptionDestroyed);
    }
};
}

// Deliberately not exported: the executable retains its shared CRT operators.
void* operator new(std::size_t bytes) {
    if (!private_heap)
        throw std::bad_alloc();
    void* object = HeapAlloc(private_heap, 0, bytes ? bytes : 1);
    if (!object)
        throw std::bad_alloc();
    ++stats.allocations;
    ++stats.live_allocations;
    return object;
}
void operator delete(void* object) noexcept {
    if (!object)
        return;
    if (!private_heap || !HeapValidate(private_heap, 0, object)
        || !HeapFree(private_heap, 0, object)) {
        fixture_runtime_host_event(InvalidAllocationOwner);
        std::terminate();
    }
    ++stats.frees;
    --stats.live_allocations;
}
void operator delete(void* object, std::size_t) noexcept { operator delete(object); }
void* operator new[](std::size_t bytes) { return operator new(bytes); }
void operator delete[](void* object) noexcept { operator delete(object); }
void operator delete[](void* object, std::size_t) noexcept { operator delete(object); }

struct ModuleObject {
    std::uint64_t value;
    explicit ModuleObject(std::uint64_t input) : value(input) {}
    ~ModuleObject() {
        ++stats.object_destructors;
        fixture_runtime_host_event(ObjectDestroyed);
    }
};
MODULE_EXPORT ModuleObject* fixture_module_create(std::uint64_t value) { return new ModuleObject(value); }
MODULE_EXPORT void fixture_module_destroy(ModuleObject* object) noexcept { delete object; }
MODULE_EXPORT bool fixture_module_owns_allocation(const void* object) noexcept {
    return object && private_heap && HeapValidate(private_heap, 0, object);
}
MODULE_EXPORT const ModuleStats* fixture_module_stats() noexcept { return &stats; }
MODULE_EXPORT void fixture_module_throw_standard() {
    Scope cleanup;
    throw std::runtime_error("DLL standard exception");
}
MODULE_EXPORT void fixture_module_throw_owned() {
    Scope cleanup;
    throw ModuleError();
}
