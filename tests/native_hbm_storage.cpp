#include "platform/native_ax_module_memory.h"
#include <array>
#include <aurora/aurora.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <dolphin/os.h>
#include <stdexcept>
namespace aurora {
extern AuroraConfig g_config;
}
void AuroraOSShutdown();
namespace {
using namespace mscharged::platform;
unsigned checks{};
void Check(bool v, const char *s) {
    ++checks;
    if (!v)
        throw std::runtime_error(s);
}
template <class F> void Reject(F f, const char *s) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception &) {
        threw = true;
    }
    Check(threw, s);
}
void Run(const char *path, unsigned count) {
    aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
    aurora::g_config.mem2Size = 64u * 1024u * 1024u;
    OSInit();
    Check(OSGetArenaLo() && OSGetMEM2ArenaLo(), "actual SDK arenas missing");
    {
        NativeAXModuleMemory owner(path);
        void *image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!image)
            throw std::runtime_error(dlerror());
        owner.ConfirmLoaded(image);
        auto get =
            reinterpret_cast<ChargedAXStorage (*)()>(dlsym(image, "charged_hbm_storage_zero"));
        Check(get != nullptr, "actual source storage observer missing");
        const auto zero = get();
        Check(zero.bytes == 256 && reinterpret_cast<std::uintptr_t>(zero.address) > UINT32_MAX,
              "actual source LP64 silence array missing");
        auto status = owner.Status();
        Check(status.loaded && status.reserved && !status.retired && status.spans == count,
              "wrong genuine source storage owner count");
        Check(!status.before.memory_initialized && !status.after.memory_initialized &&
                  !status.before.standard_address && !status.before.virtual_address &&
                  !status.after.standard_address && !status.after.virtual_address,
              "prestatic reservation initialized/captured original arenas");
        Check(owner.OwnsReadableImageExtent(zero.address, zero.bytes),
              "actual original BSS lost its source image owner");
        Reject([&] { owner.PhysicalAddress(count); }, "out-of-schema index accepted");
        std::array<unsigned char, 256> bytes;
        if (count == CHARGED_AX_HBM_STORAGE_COUNT) {
            const auto physical = owner.PhysicalAddress(CHARGED_AX_BASE_STORAGE_COUNT);
            Check(OSCachedToPhysical(zero.address) == physical &&
                      OSPhysicalToCached(physical) == zero.address,
                  "original silence physical roundtrip failed");
            bytes.fill(0xcd);
            DSPBackendReadMemory(owner.Endpoint(), physical, bytes.data(), bytes.size());
            for (auto byte : bytes)
                Check(byte == 0, "actual original silence BSS changed");
            Reject([&] { DSPBackendWriteMemory(owner.Endpoint(), physical, bytes.data(), 1); },
                   "device wrote read-only original silence");
            Reject([&] { DSPBackendReadMemory(owner.Endpoint(), physical + 255, bytes.data(), 2); },
                   "read exceeded exact original silence extent");
            Reject([&] { PinNativeDSPMemory(zero.address, zero.bytes, false); },
                   "source duplicate DSP pin accepted");
            owner.ReleaseAfterDeviceDrain();
            Reject([&] { DSPBackendReadMemory(owner.Endpoint(), physical, bytes.data(), 1); },
                   "retired source pin remained readable");
        } else {
            Check(count == CHARGED_AX_BASE_STORAGE_COUNT, "unknown test source profile");
            Reject([&] { OSCachedToPhysical(zero.address); },
                   "base13 profile invented an HBM physical owner");
            owner.ReleaseAfterDeviceDrain();
        }
        status = owner.Status();
        Check(status.retired && status.spans == 0, "source device owners did not really retire");
        Reject([&] { OSCachedToPhysical(zero.address); },
               "retired source mapping remained translatable");
        Check(get().address == zero.address,
              "initial loader owner did not retain actual source after device retirement");
        Check(!owner.OwnsReadableImageExtent(zero.address, zero.bytes),
              "retired native owner still authorized source extent");
        DetachNativeDSPMEM1();
        Check(dlclose(image) == 0, "actual source image close failed");
    }
    AuroraOSShutdown();
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("expected source image and count");
        Run(argv[1], std::strtoul(argv[2], nullptr, 10));
        std::printf(
            "Original AX/HBM prestatic owner: %u checks, actual image/arrays, readonly silence, "
            "real source pin retirement; source sound lifecycle remains separate\n",
            checks);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "AX/HBM prestatic owner: %s\n", e.what());
        return 1;
    }
}
