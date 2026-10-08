#include "platform/filesystem_device.h"
#include "platform/ios_device.h"
#include "platform/ipc_boot_buffer.h"
#include "platform/interrupts.h"
#include "platform/thread_registry_abi.h"
#include <revolution/nand.h>
#include <revolution/fs.h>
#include <revolution/os/OSIpc.h>
#include <revolution/os/OSReset.h>
#include <dolphin/os.h>
#include <aurora/hardware.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
using namespace mscharged::platform;
using Clock = std::chrono::steady_clock;
unsigned checks{}, clock_completions{};
std::thread::id owner;
OSContext* original_context{};
void Check(bool okay, const char* error) {
    ++checks;
    if (!okay) throw std::runtime_error(error);
}
void HardwareService() {
    if (ServiceNativeIOSRequests()) ++clock_completions;
}
BOOL NeverInvoked(BOOL, u32) {
    throw std::logic_error("Registration sentinels are not shutdown services");
}
// These caller-owned markers only expose the genuine intrusive registration.
// Their functions are never used in place of an original shutdown provider.
OSShutdownFunctionInfo before{NeverInvoked, 100}, after{NeverInvoked, 500};
struct Receipt {
    unsigned calls{};
    s32 result{-999};
    static void FS(s32 result, void* context) {
        auto& self = *static_cast<Receipt*>(context);
        Check(NativeInterruptDispatchActive() && !NativeInterruptsEnabled()
            && std::this_thread::get_id() == owner,
            "Original filesystem completion escaped its actual owner IRQ");
        Check(OSGetCurrentContext() != original_context,
            "Filesystem completion did not borrow the real IRQ context");
        self.result = result;
        ++self.calls;
    }
    static void NAND(s32 result, NANDCommandBlock* block) {
        FS(result, block->userData);
    }
};
void CheckDrained(unsigned completed, const char* error) {
    const auto status = GetNativeIOSStatus();
    Check(clock_completions == completed && !status.pending && !status.active, error);
    Check(NativeInterruptsEnabled() && OSGetCurrentContext() == original_context,
        "Source callback completion changed caller mask or context");
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(bool(input), "Disposable persistent file cannot be read");
    return {std::istreambuf_iterator<char>(input), {}};
}
void Run(const std::filesystem::path& root, bool masked_hold) {
    OSInit();
    owner = std::this_thread::get_id();
    original_context = OSGetCurrentContext();
    alignas(32) static std::array<unsigned char, 32768> boot;
    InstallNativeIPCBootBuffer(boot.data(), boot.size());
    __OSInitIPCBuffer();
    IPCInit();
    Check(NANDInit() == NAND_RESULT_NOEXISTS && !nandIsInitialized(),
        "Absent physical FS/ES invented original initialization success");
    OSRegisterShutdownFunction(&before);
    OSRegisterShutdownFunction(&after);
    // Explicit disposable virtual title fixture, not game readiness or a
    // supplied game save. The original ES/NAND path still owns its home.
    constexpr u64 title = 0x0001000052345145ULL;
    InitializeNativeFilesystem({root, title, 0x1000, 0x3031});
    Check(aurora_register_hardware_service(HardwareService),
        "Actual SDK clock service already has an owner");
    Check(NANDInit() == NAND_RESULT_OK && nandIsInitialized(),
        "Whole original NANDInit cannot register its real FS shutdown owner");
    auto* hook = before.next;
    Check(hook && hook != &after && hook->priority == 255
        && hook->prev == &before && hook->next == &after && after.prev == hook,
        "Actual NAND shutdown registration is absent or reordered");
    const auto mask = OSDisableInterrupts();
    auto* active = ChargedNativeActiveThreadQueue();
    Check(active->head == OSGetCurrentThread() && active->tail == active->head
        && active->head->state == OS_THREAD_STATE_RUNNING,
        "This scoped source NAND leaf unexpectedly created an SDK worker");
    OSRestoreInterrupts(mask);

    if (masked_hold) {
        OSDisableInterrupts();
        const auto start = Clock::now();
        const auto result = hook->func(FALSE, OS_SHUTDOWN_SHUTDOWN);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        Check(result && elapsed >= 499 && elapsed < 2000,
            "Masked original hook did not preserve its literal 500ms timeout");
        Check(!NativeInterruptsEnabled() && !clock_completions
            && GetNativeIOSStatus().pending == 1 && !GetNativeIOSStatus().active,
            "Masked original callback unexpectedly ran or lost pending ownership");
        std::printf("Original NAND masked HOLD: %u checks, %lldms, one pending source callback; no delivery/unload/arena retirement after the source stack-flag lifetime ended.\n",
            checks, static_cast<long long>(elapsed));
        std::fflush(nullptr);
        std::_Exit(0); // Preserve the measured source timeout quirk terminally.
    }

    Check(hook->func(TRUE, OS_SHUTDOWN_SHUTDOWN), "Original final hook result changed");
    for (u32 event = 0; event != 8; ++event)
        if (event != OS_SHUTDOWN_SHUTDOWN)
            Check(hook->func(FALSE, event), "Original non-shutdown hook result changed");
    CheckDrained(0, "Final/non-shutdown source branches issued host IO");

    Check(NANDCreate("shutdown", NAND_PERM_RUSR | NAND_PERM_WUSR, 5) == NAND_RESULT_OK,
        "Original NAND create did not establish a real writable file");
    NANDFileInfo file{};
    Check(NANDOpen("shutdown", &file, NAND_ACCESS_RW) == NAND_RESULT_OK,
        "Original NAND open cannot borrow the genuine physical file");
    alignas(32) std::array<unsigned char, 128> bytes;
    for (unsigned n = 0; n != bytes.size(); ++n) bytes[n] = (n * 31 + 9) & 255;
    Check(NANDWrite(&file, bytes.data(), bytes.size()) == s32(bytes.size()),
        "Original NAND write changed source bytes/count");
    const auto physical = root / "data/title/00010000/52345145/data/shutdown";
    Check(Read(physical) == std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        "Real file bytes differ from original NAND output");

    Receipt flush;
    Check(std::uintptr_t(&flush) > UINT32_MAX
        && ISFS_ShutdownAsync(Receipt::FS, &flush) == IPC_RESULT_OK
        && !flush.calls && GetNativeIOSStatus().pending == 1,
        "Whole filesystem shutdown failed delayed full-width context submission");
    const auto prior = OSDisableInterrupts();
    Check(!ServiceNativeIOSRequests() && !flush.calls && GetNativeIOSStatus().pending == 1,
        "Masked filesystem request consumed its source callback");
    OSRestoreInterrupts(prior);
    (void)OSGetTime();
    Check(flush.calls == 1 && flush.result == IPC_RESULT_OK,
        "Actual FS13 flush did not complete through the original SDK clock");
    CheckDrained(1, "Filesystem completion retains source callback ownership");

    // Existing source IO must complete in submission order. The genuine hook
    // first calls OSGetTime, then submits FS13 and polls its own stack flag.
    Check(NANDSeek(&file, 0, NAND_SEEK_BEG) == 0, "Original file seek failed");
    for (auto& byte : bytes) byte ^= 0xa5;
    Receipt write;
    NANDCommandBlock block{};
    block.userData = &write;
    Check(NANDWriteAsync(&file, bytes.data(), bytes.size(), Receipt::NAND, &block) == NAND_RESULT_OK
        && !write.calls && GetNativeIOSStatus().pending == 1,
        "Original asynchronous write did not retain its actual caller-owned bytes");
    Check(OSDisableScheduler() == 0, "Source shutdown scheduler prerequisite changed");
    Check(hook->func(FALSE, OS_SHUTDOWN_SHUTDOWN), "Original registered NAND hook result changed");
    Check(OSEnableScheduler() == 1, "Original scheduler prior-count release changed");
    Check(write.calls == 1 && write.result == s32(bytes.size()),
        "Source prior write did not finish before its shutdown flush");
    CheckDrained(3, "Literal NAND hook returned with a still-pending stack callback");
    Check(Read(physical) == std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        "Real flush changed source-written data");
    (void)OSGetTime();
    CheckDrained(3, "A shutdown callback was delivered again after hook return");

    // Deterministic physical OS error on this disposable catalog's temporary
    // path. No callback result, source flags, descriptor or game path is edited.
    const auto obstruction = root / "metadata.txt.tmp";
    Check(std::filesystem::create_directory(obstruction), "Cannot establish real catalog IO error");
    Receipt failed;
    Check(ISFS_ShutdownAsync(Receipt::FS, &failed) == IPC_RESULT_OK && !failed.calls,
        "Erroring filesystem operation was not genuinely asynchronous");
    (void)OSGetTime();
    Check(failed.calls == 1 && failed.result == IPC_RESULT_INVALID,
        "Physical EISDIR was replaced with a successful FS13 result");
    CheckDrained(4, "Real filesystem failure did not drain its original callback");
    Check(hook->func(FALSE, OS_SHUTDOWN_SHUTDOWN),
        "Original NAND hook's error-ignored result quirk changed");
    CheckDrained(5, "Error completion escaped the original stack-flag lifetime");
    Check(std::filesystem::remove(obstruction), "Cannot retire disposable catalog obstruction");
    Check(hook->func(FALSE, OS_SHUTDOWN_SHUTDOWN), "Actual flush did not recover after physical error removal");
    CheckDrained(6, "Recovered source shutdown still owns a pending completion");
    Check(NANDClose(&file) == NAND_RESULT_OK, "Original source file close did not retire its descriptor");
    const auto catalog = Read(root / "metadata.txt");
    Check(aurora_unregister_hardware_service(HardwareService), "Actual clock service retirement failed");
    ShutdownNativeFilesystem();
    InitializeNativeFilesystem({root, title, 0x1000, 0x3031});
    Check(Read(root / "metadata.txt") == catalog && Read(physical)
        == std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        "Real reopened persistence changed source bytes or virtual ownership catalog");
    ShutdownNativeFilesystem();
    std::printf("Original NAND shutdown PASS: %u checks, %u actual clock-delivered callbacks, delayed/ordered/physical-error/reopen; registered hook only, full OSShutdown/ResetTask held.\n",
        checks, clock_completions);
    std::fflush(nullptr);
    std::_Exit(0); // Full module/static/SDK arena lifetime is not this leaf.
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 3) throw std::invalid_argument("Expected disposable root and optional --masked-hold");
        const bool masked = argc == 3 && std::strcmp(argv[2], "--masked-hold") == 0;
        if (argc == 3 && !masked) throw std::invalid_argument("Unknown fixture mode");
        const auto unique = "owned-" + std::to_string(getpid()) + "-"
            + std::to_string(Clock::now().time_since_epoch().count());
        Run(std::filesystem::path(argv[1]) / unique, masked);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NAND shutdown source boundary: %s (%u checks)\n", error.what(), checks);
        std::fflush(nullptr);
        std::_Exit(1);
    }
}
