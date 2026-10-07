#include "platform/interrupts.h"
#include <revolution/ipc.h>
#include <dolphin/os.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct alignas(256) Arena {
    std::array<unsigned char, 8192> bytes;
};
void Run() {
    // These live CPU heaps deliberately reside above the Wii address width.
    // Each remains alive until process exit: original SDK has no destroy API.
    static std::array<Arena, 8> arenas;
    using mscharged::platform::NativeInterruptsEnabled;
    Check(NativeInterruptsEnabled(), "Initial native interrupt state differs");
    for (unsigned i = 0; i < arenas.size(); ++i) {
        auto& bytes = arenas[i].bytes;
        Check(reinterpret_cast<std::uintptr_t>(bytes.data()) > UINT32_MAX,
              "IPC heap qualification requires actual high native addresses");
        bytes.fill(0xa7);
        Check(iosCreateHeap(bytes.data() + 1, bytes.size() - 1) == IPC_RESULT_INVALID_INTERNAL,
              "Original IPC heap accepted an unaligned base");
        const auto heap = iosCreateHeap(bytes.data(), bytes.size());
        Check(heap == static_cast<s32>(i), "Original IPC heap handle sequence differs");
        Check(!iosAllocAligned(heap, 0, 32) && !iosAllocAligned(heap, 32, 0) &&
                  !iosAllocAligned(heap, 32, 48), "Original IPC invalid size/alignment accepted");
        std::array<void*, 4> blocks{};
        for (unsigned j = 0; j < blocks.size(); ++j) {
            const unsigned alignment = 32u << j;
            blocks[j] = iosAllocAligned(heap, 96, alignment);
            const auto address = reinterpret_cast<std::uintptr_t>(blocks[j]);
            Check(blocks[j] && address % alignment == 0,
                  "Original IPC native allocation alignment differs");
            Check(address >= reinterpret_cast<std::uintptr_t>(bytes.data()) &&
                      address + 96 <= reinterpret_cast<std::uintptr_t>(bytes.data() + bytes.size()),
                  "Original IPC allocation escaped its owning heap");
            std::memset(blocks[j], 0x31 + j, 96);
        }
        // Exercise a nonempty free-list and alignment helper while preserving
        // neighboring caller data. No source metadata or flag is test-written.
        for (unsigned j : {1u, 3u, 0u, 2u}) {
            for (unsigned k = 0; k < blocks.size(); ++k) {
                if (!blocks[k]) continue;
                const auto* p = static_cast<const unsigned char*>(blocks[k]);
                Check(std::all_of(p, p + 96, [k](unsigned char value) { return value == 0x31 + k; }),
                      "Original IPC free corrupted a live payload");
            }
            Check(iosFree(heap, blocks[j]) == IPC_RESULT_OK,
                  "Original IPC owning free failed");
            blocks[j] = nullptr;
        }
        void* large = iosAllocAligned(heap, 8000, 32);
        Check(large != nullptr, "Original IPC freed chunks did not coalesce");
        std::memset(large, 0x53, 8000);
        Check(!iosAllocAligned(heap, 256, 32), "Original IPC exhausted heap accepted allocation");
        Check(iosFree(heap, large) == IPC_RESULT_OK, "Original IPC coalesced owning free failed");
        const auto previous = OSDisableInterrupts();
        void* masked = iosAllocAligned(heap, 32, 32);
        Check(masked && !NativeInterruptsEnabled(), "Original IPC allocation lost caller interrupt mask");
        Check(iosFree(heap, masked) == IPC_RESULT_OK && !NativeInterruptsEnabled(),
              "Original IPC free lost caller interrupt mask");
        OSRestoreInterrupts(previous);
        Check(NativeInterruptsEnabled(), "Original IPC operation did not restore interrupt state");
    }
    Check(iosCreateHeap(arenas[0].bytes.data(), arenas[0].bytes.size()) == IPC_RESULT_CONN_MAX_INTERNAL,
          "Original IPC heap descriptor exhaustion differs");
    Check(!iosAllocAligned(-1, 32, 32) && !iosAllocAligned(8, 32, 32),
          "Original IPC allocation accepted invalid handles");
    Check(iosFree(-1, arenas[0].bytes.data() + 32) == IPC_RESULT_INVALID_INTERNAL &&
              iosFree(0, nullptr) == IPC_RESULT_INVALID_INTERNAL,
          "Original IPC invalid free result differs");
    std::cout << "original_ipc_memory: " << checks
              << " checks; whole source allocator, native pointers, original interrupt exclusion\n";
}
}
int main() {
    try { Run(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
