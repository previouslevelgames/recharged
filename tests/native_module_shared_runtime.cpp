#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include "platform/native_module_loader.h"
#include "platform/path.h"
#include "fixtures/native_module_shared_runtime.h"

using namespace mscharged::platform;
namespace {
unsigned checks{}, events[7]{}, host_scope_destructors{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
struct HostScope { ~HostScope() { ++host_scope_destructors; } };
template<class Function> Function Symbol(void* image, const char* name) {
    auto result = reinterpret_cast<Function>(FindNativeModuleSymbol(image, name));
    Check(result != nullptr, "Actual explicit DLL entry is missing");
    return result;
}
unsigned RuntimeModuleCount(const wchar_t* name) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    Check(snapshot != INVALID_HANDLE_VALUE, "Actual native module snapshot failed");
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    unsigned count = 0;
    if (Module32FirstW(snapshot, &module)) {
        do {
            if (_wcsicmp(module.szModule, name) == 0)
                ++count;
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return count;
}
}

extern "C" __declspec(dllexport) void fixture_runtime_host_event(unsigned event) noexcept {
    // This callback observes real DLL construction/unwind/destruction, with no
    // loader call, allocation, throwing, service or source-state mutation.
    if (event < 7)
        ++events[event];
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "Expected the actual fixture DLL path");
        NativeModuleFile file(mscharged::PathFromUtf8(argv[1]));
        void* primary = LoadNativeModule(file.Path());
        NativeModuleImage image;
        Check(QueryNativeModuleHandle(primary, image) && file.OwnsImage(image),
              "Real DLL handle must match the opened source incarnation");
        void* retained = RetainNativeModuleImage(file.Path(), image.base);
        Check(retained && events[ModuleConstructed] == 1,
              "Exactly one real DLL constructor and retained loader reference required");
        auto create = Symbol<decltype(&fixture_module_create)>(primary, "fixture_module_create");
        auto destroy = Symbol<decltype(&fixture_module_destroy)>(primary, "fixture_module_destroy");
        auto owns = Symbol<decltype(&fixture_module_owns_allocation)>(primary, "fixture_module_owns_allocation");
        auto get_stats = Symbol<decltype(&fixture_module_stats)>(primary, "fixture_module_stats");
        auto standard_throw = Symbol<decltype(&fixture_module_throw_standard)>(primary, "fixture_module_throw_standard");
        auto owned_throw = Symbol<decltype(&fixture_module_throw_owned)>(primary, "fixture_module_throw_owned");
        const ModuleStats* stats = get_stats();
        Check(NativeModuleOwnsReadableExtent(image.base, stats, sizeof(*stats)),
              "DLL statistics must belong to the live selected image");
        Check(stats->allocations == 0 && stats->live_allocations == 0,
              "DLL fixture begins without caller allocations");
        ModuleObject* object = create(0x123456789abcdef0ull);
        Check(object && owns(object) && stats->allocations == 1 && stats->live_allocations == 1,
              "Opaque object must use the DLL's actual new/private heap owner");
        auto host_object = std::make_unique<std::uint64_t>(17);
        Check(!owns(host_object.get()), "Host shared CRT allocation must remain outside the module heap");
        Check(!NativeModuleOwnsReadableExtent(image.base, object, 1),
              "DLL heap allocation must not be fabricated as a PE static image span");

        bool standard_caught = false;
        try {
            HostScope cleanup;
            standard_throw();
        } catch (const std::runtime_error& error) {
            standard_caught = std::strcmp(error.what(), "DLL standard exception") == 0;
        }
        Check(standard_caught && host_scope_destructors == 1 && stats->scope_destructors == 1,
              "Real standard exception must cross DLL frames and clean both source and host scopes");
        std::exception_ptr held;
        try {
            HostScope cleanup;
            owned_throw();
        } catch (const std::runtime_error& error) {
            Check(std::strcmp(error.what(), "retained DLL standard exception") == 0,
                  "DLL-defined standard exception must reach the actual host base handler");
            held = std::current_exception();
        }
        Check(held && host_scope_destructors == 2 && stats->scope_destructors == 2
              && stats->exception_destructors == 0,
              "Retained exception keeps its real DLL destructor pending");
        Check(ReleaseNativeModule(primary) && events[ModuleDestroyed] == 0,
              "Primary handle retirement must not unload the retained object/exception owner");
        Check(NativeModuleOwnsReadableExtent(image.base, stats, sizeof(*stats)) && owns(object),
              "Actual retained DLL reference must preserve source code and data ownership");
        bool rethrow_caught = false;
        try { std::rethrow_exception(held); }
        catch (const std::runtime_error& error) {
            rethrow_caught = std::strcmp(error.what(), "retained DLL standard exception") == 0;
        }
        Check(rethrow_caught && stats->exception_destructors == 0,
              "Shared C++ runtime must rethrow the retained standard exception without early destruction");
        destroy(object);
        object = nullptr;
        Check(stats->object_destructors == 1 && stats->frees == 1 && stats->live_allocations == 0
              && events[ObjectDestroyed] == 1 && events[InvalidAllocationOwner] == 0,
              "Actual module delete must destroy/free only its own allocation");
        held = nullptr;
        Check(stats->exception_destructors == 1 && events[ExceptionDestroyed] == 1,
              "Last host exception reference must run the actual DLL destructor before release");
        for (const auto* name : {L"libc++.dll", L"libunwind.dll"})
            Check(RuntimeModuleCount(name) == 1, "Shared pinned C++ runtime DLL must have one actual loaded owner");
        file.RequireUnchanged();
        Check(ReleaseNativeModule(retained) && events[ModuleDestroyed] == 1,
              "Final reference must unload exactly once after all module-owned cleanup");
        Check(!NativeModuleOwnsReadableExtent(image.base, stats, sizeof(*stats)),
              "Former image data must retire after the actual final unload");
        host_object.reset();
        Check(events[ScopeDestroyed] == 2 && events[InvalidAllocationOwner] == 0,
              "Real unwinding and ownership counters must remain exact");
        std::printf("Shared Win64 C++ runtime PASS %u checks; real loader, standard exception, retained DLL destructor, owner allocation/free and final unload.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Shared Win64 C++ runtime hold: %s\n", error.what());
        return 1;
    }
}
