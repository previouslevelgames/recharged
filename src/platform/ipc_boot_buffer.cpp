#include "platform/ipc_boot_buffer.h"
#include "platform/ipc_hardware_abi.h"
#include "platform/interrupts.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
struct Buffer {
    std::mutex mutex;
    std::thread::id owner;
    void* start{};
    void* end{};
};
Buffer& State() { static Buffer buffer; return buffer; }
void* Endpoint(bool end) {
    auto& buffer = State();
    std::lock_guard lock(buffer.mutex);
    if (!buffer.start || buffer.owner != std::this_thread::get_id())
        throw std::logic_error("IPC boot memory requires the initialized host owner");
    return end ? buffer.end : buffer.start;
}
}

namespace mscharged::platform {
void InstallNativeIPCBootBuffer(void* start, std::size_t size) {
    const auto address = reinterpret_cast<std::uintptr_t>(start);
    if (!start || size < 32 || address % 32 || size % 32 ||
            size > std::numeric_limits<std::uintptr_t>::max() - address)
        throw std::invalid_argument("IPC boot memory must be a live aligned native span");
    NativeInterruptGuard exclusion;
    auto& buffer = State();
    std::lock_guard lock(buffer.mutex);
    auto* end = static_cast<unsigned char*>(start) + size;
    if (buffer.start) {
        if (buffer.owner != std::this_thread::get_id() ||
                buffer.start != start || buffer.end != end)
            throw std::logic_error("Cannot replace IPC memory captured by the original SDK");
        return;
    }
    buffer.start = start;
    buffer.end = end;
    buffer.owner = std::this_thread::get_id();
}
}

extern "C" void* ChargedNativeIPCBufferStart() { return Endpoint(false); }
extern "C" void* ChargedNativeIPCBufferEnd() { return Endpoint(true); }
extern "C" std::uint32_t ChargedNativeIPCReadRegister(std::int32_t) {
    // Native IOS services operate at their SDK interfaces. A Wii ARM mailbox
    // register has no implemented device here; never report successful hardware.
    throw std::runtime_error("Native IPC MMIO read is not implemented");
}
extern "C" void ChargedNativeIPCWriteRegister(std::int32_t, std::uint32_t) {
    throw std::runtime_error("Native IPC MMIO write is not implemented");
}
