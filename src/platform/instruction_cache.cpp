#include "platform/instruction_cache.h"
#include <dolphin/os/OSIC.h>

#include <atomic>
#include <limits>
#include <stdexcept>

namespace {
thread_local std::uint64_t sequence;
}
namespace mscharged::platform {
std::uint64_t NativeInstructionCacheSequence() noexcept { return sequence; }
}

extern "C" void ICFlashInvalidate(void) {
    if (sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Native instruction ordering serial exhausted");
    // The original terminal caller changes no CPU instruction bytes. All host
    // and original-module code is compiled immutable machine code published by
    // the OS loader, which owns any required platform code-cache maintenance.
    // Preserve compiler and hardware ordering across this instruction boundary
    // on Linux/macOS native CPUs, without pretending to invalidate PPC
    // cache lines or supporting unqualified self-modifying/JIT native code.
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ++sequence;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}
