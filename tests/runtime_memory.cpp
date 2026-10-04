#include "runtime/startup.h"
#include "platform/path.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <revolution/os/OS_fwd.h>
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
struct Session
{
    bool live = false;
    ~Session() { if (live) { mscharged::ResetStartupMemory(); aurora_shutdown(); } }
};
}

int main(int argc, char** argv)
{
    try
    {
        const char* base = SDL_GetBasePath();
        Require(base != nullptr, "Cannot find test data directory");
        const std::string path = std::string(base) + "memory-test-data";
        std::filesystem::create_directories(mscharged::PathFromUtf8(path));
        // The 128 MiB branch leaves the original 64 MiB reserve. Repeated
        // initialization must restore arenas and remove stale SDK heap pointers.
        for (std::uint32_t mem2 : {64u*1024*1024, 128u*1024*1024, 64u*1024*1024})
        {
            AuroraConfig config{};
            config.appName = "Charged native memory check";
            config.userPath = config.cachePath = path.c_str();
            config.resourcesPath = base;
            config.desiredBackend = BACKEND_NULL;
            config.windowWidth = 320; config.windowHeight = 240;
            config.windowPosX = config.windowPosY = -1;
            config.logLevel = LOG_WARNING;
            config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = mem2;
            Session session;
            const auto host = aurora_initialize(argc, argv, &config);
            session.live = true;
            Require(host.window != nullptr, "Aurora window initialization failed");
            mscharged::InitializeStartupOS();
            Require(OSGetConsoleSimulatedMem2Size() == mem2, "MEM2 capacity is not its real allocation size");
            const auto low = reinterpret_cast<std::uintptr_t>(OSGetMEM2ArenaLo());
            const auto high = reinterpret_cast<std::uintptr_t>(OSGetMEM2ArenaHi());
            Require(high-low == mem2, "MEM2 arena was not reset on initialization");
            Require(OSAllocFromMEM2ArenaLo(UINT32_MAX, 32) == nullptr, "Arena overflow was accepted");
            Require(OSAllocFromMEM2ArenaLo(32, 0) == nullptr && OSAllocFromMEM2ArenaLo(32, 3) == nullptr,
                    "Invalid arena alignment was accepted");
            Require(reinterpret_cast<std::uintptr_t>(OSGetMEM2ArenaLo()) == low, "Failed arena request changed its cursor");
            void* probe = OSAllocFromMEM2ArenaLo(43, 256);
            Require(probe && reinterpret_cast<std::uintptr_t>(probe)%256 == 0, "MEM2 arena alignment failed");
            OSSetMEM2ArenaLo(reinterpret_cast<void*>(low));
            nlInitMemory(); // Actual original platform memory setup, without the later GX stage.
            Require(gMemoryInitialized && StandardAllocator.m_memory && VirtualAllocator.m_memory, "Original allocators did not initialize");
            Require(VirtualAllocator.m_memory_size == mem2-(mem2 == 128u*1024*1024 ? 64u*1024*1024 : 8192),
                    "Original MEM2 reserve branch changed");
            const auto std_free = StandardAllocator.TotalFreeMemory();
            const auto virtual_free = VirtualAllocator.TotalFreeMemory();
            Require(__OSCurrHeap >= 0 && OSCheckHeap(__OSCurrHeap) > 0, "Reserved SDK heap is invalid");
            const auto sdk_free = OSCheckHeap(__OSCurrHeap);
            void* sdk = OSAllocFromHeap(__OSCurrHeap, 128);
            Require(sdk != nullptr, "Reserved SDK heap cannot allocate");
            std::memset(sdk, 0x45, 128);
            OSFreeToHeap(__OSCurrHeap, sdk);
            Require(OSCheckHeap(__OSCurrHeap) == sdk_free, "SDK heap did not recover after free");

            CurrentAllocator = &VirtualAllocator;
            void* external = nlMalloc(73, 128, true);
            CurrentAllocator = &StandardAllocator;
            void* internal = nlMalloc(91, 64, false);
            std::memset(external, 0x56, 73); std::memset(internal, 0x67, 91);
            nlFree(external); // Its owner is MEM2 even when the selected allocator is MEM1.
            CurrentAllocator = &VirtualAllocator;
            nlFree(internal);
            nlFree(nullptr);
            Require(StandardAllocator.TotalFreeMemory() == std_free
                && VirtualAllocator.TotalFreeMemory() == virtual_free, "Explicit game frees used the wrong arena");
            nlInitMemory(); // Original guard must not reinitialize live arenas.
            Require(StandardAllocator.TotalFreeMemory() == std_free, "Repeated nlInitMemory changed the allocator");
            std::cout << "Original memory setup and SDK/game allocations passed for MEM2 " << mem2/(1024*1024) << " MiB\n";
        }
        Require(OSGetConsoleSimulatedMem2Size() == 0 && OSGetMEM2ArenaLo() == nullptr
            && OSGetMEM2ArenaHi() == nullptr && __OSCurrHeap == -1 && !gMemoryInitialized,
                "Shutdown left memory or heap state live");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
