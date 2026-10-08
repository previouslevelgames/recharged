#pragma once
#include <cstdint>

struct ModuleObject;
struct ModuleStats {
    std::uint32_t allocations;
    std::uint32_t frees;
    std::uint32_t live_allocations;
    std::uint32_t object_destructors;
    std::uint32_t scope_destructors;
    std::uint32_t exception_destructors;
};
enum : unsigned {
    ModuleConstructed = 1,
    ObjectDestroyed = 2,
    ScopeDestroyed = 3,
    ExceptionDestroyed = 4,
    ModuleDestroyed = 5,
    InvalidAllocationOwner = 6,
};

#if defined(FIXTURE_MODULE)
#define MODULE_EXPORT extern "C" __declspec(dllexport)
extern "C" __declspec(dllimport) void fixture_runtime_host_event(unsigned event) noexcept;
#else
#define MODULE_EXPORT extern "C"
#endif

MODULE_EXPORT ModuleObject* fixture_module_create(std::uint64_t value);
MODULE_EXPORT void fixture_module_destroy(ModuleObject* object) noexcept;
MODULE_EXPORT bool fixture_module_owns_allocation(const void* object) noexcept;
MODULE_EXPORT const ModuleStats* fixture_module_stats() noexcept;
MODULE_EXPORT void fixture_module_throw_standard();
MODULE_EXPORT void fixture_module_throw_owned();
