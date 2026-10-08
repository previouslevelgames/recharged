#include "platform/hbm_debug_abi.h"
#include "platform/alarms.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "platform/thread.h"
#include "platform/video_device.h"
#include <aurora/aurora.h>
#include <aurora/video.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <revolution/hbm/HBMAssert.hpp>
#include <revolution/hbm/nw4hbm/db/assert.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace aurora { extern AuroraConfig g_config; }
// Actual Aurora memory owner operation, not a fixture replacement.
extern void AuroraOSShutdownMemory();

namespace {
unsigned checks{}, assertions{};
alignas(32) std::array<unsigned char, 128> source_static;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class Function> void Reject(Function function, const char* message) {
    bool failed{};
    try { function(); } catch (const std::exception&) { failed = true; }
    Check(failed, message);
}
void Pre(u32) {}
void Post(u32) {}

void QualifyRegions() {
    alignas(32) std::array<unsigned char, 128> stack;
    Check(!ChargedNativeHBMPointerValid(nullptr), "Native assertion accepted NULL");
    Check(!ChargedNativeHBMPointerValid(reinterpret_cast<void*>(1)), "Native assertion accepted an unmapped address");
    Check(ChargedNativeHBMPointerValid(stack.data()), "Actual current stack rejected");
    Check(ChargedNativeHBMPointerValid(source_static.data()), "Actual retained static storage rejected");
    auto* crt = std::malloc(64);
    Check(crt && ChargedNativeHBMPointerValid(crt), "Actual CRT mapping rejected");
    std::free(crt); // No claim about this address's object lifetime follows.

    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    Check(page != 0, "Actual host page size absent");
    auto* mapping = static_cast<unsigned char*>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(mapping != MAP_FAILED, "Actual readable foreign mapping failed");
    Check(ChargedNativeHBMPointerValid(mapping) && ChargedNativeHBMPointerValid(mapping + page - 1),
        "Actual foreign mapping endpoints rejected");
    Check(mprotect(mapping, page, PROT_NONE) == 0, "Actual protection retirement failed");
    Check(!ChargedNativeHBMPointerValid(mapping), "Native predicate cached a retired read permission");
    Check(mprotect(mapping, page, PROT_READ) == 0, "Actual read permission restore failed");
    Check(ChargedNativeHBMPointerValid(mapping), "Restored actual read permission rejected");
    Check(munmap(mapping, page) == 0, "Actual mapping retirement failed");
    Check(!ChargedNativeHBMPointerValid(mapping), "Native predicate accepted retired foreign storage");

    Check(ChargedNativeHBMMapCursorWord(reinterpret_cast<void*>(std::uintptr_t(0x80000000))) == 0x80000000,
        "Original DVD cursor marker was changed");
    Check(ChargedNativeHBMMapCursorWord(reinterpret_cast<void*>(std::uintptr_t(0xffffffff))) == 0xffffffff,
        "Original DVD cursor high bits were lost");
    Reject([&] { (void)ChargedNativeHBMMapCursorWord(stack.data()); },
        "A full native pointer was truncated to a DVD file cursor");
    NW4HBMAssertMessage(++assertions == 1, "Original expression side effect was skipped or repeated");
    NW4HBMCheckMessage(++assertions == 2, "Original check expression side effect was skipped or repeated");
    Check(assertions == 2, "Retail assert/check expressions were omitted");
    NW4HBMAssertPointerValid(stack.data());
    NW4HBMAssertPointerValid(source_static.data());
    NW4HBMAlign2_Line(stack.data(), 101);
    NW4HBMAlign32_Line(stack.data(), 102);
    NW4HBMAssertAligned_Line(103, stack.data(), 32);
    NW4HBMAssertAligned_Line(104, std::uintptr_t(0x100000020), 32);
    Check(NW4HBM_IS_ALIGNED_(std::uintptr_t(0x100000020), 32)
        && !NW4HBM_IS_ALIGNED_(std::uintptr_t(0x100000021), 32),
        "Native full-width alignment bit operation changed");
    std::array<ChargedNativeHBMFrame, 16> frames;
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    const auto count = ChargedNativeHBMCaptureStack(caller, frames.data(), frames.size());
    const auto limits = mscharged::CurrentThreadStackLimits();
    Check(count && count <= frames.size(), "Native unwind did not report genuine frames");
    for (std::size_t i=0; i<count; ++i)
        Check(frames[i].stack_address >= caller && frames[i].stack_address < limits.high
            && frames[i].instruction_address, "Native unwind published a fabricated/out-of-stack frame");
    Reject([&] { ChargedNativeHBMCaptureStack(0, frames.data(), frames.size()); }, "Native unwind accepted no caller");

    // A bounded cost observation, not a frame-rate or game performance claim.
    constexpr unsigned iterations = 10000;
    auto begin = std::chrono::steady_clock::now();
    for (unsigned i=0; i<iterations; ++i)
        if (!ChargedNativeHBMPointerValid(stack.data())) throw std::runtime_error("Live stack changed during retained access");
    auto end = std::chrono::steady_clock::now();
    std::printf("Retained stack region: %u checks in %lld microseconds\n", iterations,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(end-begin).count()));
}

void InitCPUOwners() {
    aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
    aurora::g_config.mem2Size = 64u*1024u*1024u;
    OSInit();
    mscharged::platform::InitializeNativeInterruptController();
    mscharged::platform::InitializeNativeAlarms();
    mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false);
    VIInit(); // Genuine CPU VI owner only; no output endpoint/window/GX exists.
    VISetPreRetraceCallback(Pre);
    VISetPostRetraceCallback(Post);
}
}

int main(int argc, char** argv) {
    const rlimit no_core{0,0};
    setrlimit(RLIMIT_CORE, &no_core);
    try {
        const std::string_view mode = argc==2 ? argv[1] : "positive";
        QualifyRegions(); // Real host mappings also work before SDK setup.
        InitCPUOwners();
        auto* mem1 = OSGetArenaLo();
        auto* mem2 = OSGetMEM2ArenaLo();
        Check(mem1 && mem2 && ChargedNativeHBMPointerValid(mem1) && ChargedNativeHBMPointerValid(mem2),
            "Actual initialized SDK banks rejected");
        if (mode == "panic") {
            NW4HBMAssertMessage_FileLine("retail-hbm-assertion", 540, ++assertions == 99,
                "original failing expression, count=%u", assertions);
            throw std::runtime_error("Original Panic returned");
        }
        if (mode == "alignment") {
            NW4HBMAssertAligned_Line(541, std::uintptr_t(0x100000021), 32);
            throw std::runtime_error("Original alignment Panic returned");
        }
        Check(mode == "positive", "Unknown HBM assertion leaf mode");
        nw4hbm::db::Warning("retail-hbm-warning", 542, "original warning %u", checks);
        Check(mscharged::platform::NativeInterruptsEnabled(), "Returning original warning changed caller mask");
        Check(VISetPreRetraceCallback(nullptr) == Pre && VISetPostRetraceCallback(nullptr) == Post,
            "Returning original warning changed real VI callbacks");
        aurora_shutdown_video_hardware();
        mscharged::platform::ShutdownNativeAlarms();
        mscharged::platform::ShutdownNativeInterruptController();
        AuroraOSShutdownMemory();
        Check(!OSBaseAddress && ChargedNativeHBMPointerValid(source_static.data()),
            "Retired SDK ownership poisoned a retained static-region query");
        std::printf("Native HBM retail assertion CPU PASS: %u checks; original expressions/warning, live regions and native unwind; no scene/UI/bitmap claim.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native HBM assertion leaf: %s (%u checks)\n", error.what(), checks);
        return 1;
    }
}
