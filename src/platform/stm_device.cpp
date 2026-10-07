#include "platform/stm_device.h"
#include "platform/stm_hardware_abi.h"
#include "platform/ios_device.h"
#include "platform/interrupts.h"
#include <revolution/ipc.h>

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr s32 RegisterEvent = 0x1000, UnregisterEvent = 0x3002;
constexpr u32 ResetEvent = 0x20000, PowerEvent = 0x800;
struct Request {
    void* output{};
    IPCAsyncCallback callback{};
    void* context{};
};
struct Device {
    std::mutex mutex;
    std::thread::id owner;
    bool ready{}, active{}, reset_down{};
    std::uint64_t generation{};
    s32 next_fd{1}, immediate{-1}, eventhook{-1};
    mscharged::platform::NativeIOSDeviceLease ios{};
    Request event;
    std::array<u32, 8> events{};
    unsigned head{}, count{};
};
Device& State() { static Device state; return state; }
s32 DeviceOpen(const char* path, IPCOpenMode mode);
s32 DeviceClose(s32 fd);
s32 DeviceIoctl(s32 fd, s32 type, void* in, s32 inSize, void* out, s32 outSize);
s32 DeviceIoctlAsync(s32 fd, s32 type, void* in, s32 inSize, void* out, s32 outSize,
    IPCAsyncCallback callback, void* context);
bool OwnsPath(void*, const char* path) {
    return path && (std::strcmp(path, "/dev/stm/immediate") == 0 ||
                    std::strcmp(path, "/dev/stm/eventhook") == 0);
}
s32 Execute(void*, const IPCRequest& request) {
    switch (request.type) {
    case IPC_REQ_OPEN: return DeviceOpen(request.open.path, request.open.mode);
    case IPC_REQ_CLOSE: return DeviceClose(request.fd);
    case IPC_REQ_IOCTL:
        return DeviceIoctl(request.fd, request.ioctl.type, request.ioctl.in,
            request.ioctl.inSize, request.ioctl.out, request.ioctl.outSize);
    default: return IPC_RESULT_INVALID;
    }
}
s32 Submit(void*, const IPCRequest& request, IPCAsyncCallback callback, void* context) {
    return DeviceIoctlAsync(request.fd, request.ioctl.type, request.ioctl.in,
        request.ioctl.inSize, request.ioctl.out, request.ioctl.outSize, callback, context);
}

void RequireOwner(const Device& state) {
    if (!state.ready || state.owner != std::this_thread::get_id())
        throw std::logic_error("Native STM SDK operation requires initialized owner");
}
bool Push(Device& state, u32 word) {
    if (state.count == state.events.size()) return false;
    state.events[(state.head + state.count) % state.events.size()] = word;
    ++state.count;
    return true;
}
bool Buffers(void* in, s32 inSize, void* out, s32 outSize) {
    return in && out && inSize == 32 && outSize == 32;
}
}

namespace mscharged::platform {
void InitializeNativeSTMDevice() {
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (state.ready) { RequireOwner(state); return; }
        state.owner = std::this_thread::get_id();
        state.reset_down = false;
        state.event = {};
        state.head = state.count = 0;
        state.immediate = state.eventhook = -1;
        ++state.generation;
        state.ready = true;
    }
    try { state.ios = RegisterNativeIOSDevice({&state, OwnsPath, Execute, Submit}); }
    catch (...) {
        std::lock_guard lock(state.mutex);
        state.ready = false;
        state.owner = {};
        throw;
    }
}
void ShutdownNativeSTMDevice() {
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (!state.ready) return;
        RequireOwner(state);
        if (state.active) throw std::logic_error("Cannot retire active STM callback");
    }
    // Remove routing and reject outstanding IOS completions while the actual
    // device still exists. Never acquire the IOS bus under the STM state latch.
    UnregisterNativeIOSDevice(state.ios);
    std::lock_guard lock(state.mutex);
    state.ready = false;
    ++state.generation;
    state.event = {};
    state.ios = {};
    state.head = state.count = 0;
    state.immediate = state.eventhook = -1;
    state.reset_down = false;
    state.owner = {};
}
StmInput GetNativeSTMInput() {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    return {state.generation};
}
bool SubmitNativeSTMPower(StmInput input) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.ready && input.generation == state.generation && Push(state, PowerEvent);
}
bool SetNativeSTMResetButton(StmInput input, bool pressed) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.ready || input.generation != state.generation) return false;
    if (pressed && !state.reset_down && !Push(state, ResetEvent)) return false;
    state.reset_down = pressed;
    return true;
}
bool ServiceNativeSTMDevice() {
    NativeInterruptRead exclusion;
    if (!exclusion) return false;
    auto& state = State();
    Request request;
    u32 word;
    {
        std::lock_guard lock(state.mutex);
        RequireOwner(state);
        if (state.active || !state.count || !state.event.callback || !NativeInterruptsEnabled()) return false;
        request = state.event;
        word = state.events[state.head];
        state.event = {};
        state.head = (state.head + 1) % state.events.size();
        --state.count;
        state.active = true;
    }
    struct Finish { Device& state; ~Finish() { std::lock_guard lock(state.mutex); state.active = false; } } finish{state};
    struct Call {
        Request request;
        u32 word;
        static void Run(void* opaque) {
            auto& call = *static_cast<Call*>(opaque);
            // Original OSStateTM owns native u32 state buffers. Only the device's
            // event word is written; other bytes and original callback behavior stay.
            std::memcpy(call.request.output, &call.word, sizeof(call.word));
            call.request.callback(IPC_RESULT_OK, call.request.context);
        }
    } call{request, word};
    if (!DispatchNativeInterrupt(Call::Run, &call))
        throw std::logic_error("STM owner dispatch lost its enabled exclusion boundary");
    return true;
}
}

extern "C" bool mscharged_stm_reset_button_pressed() {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    return state.reset_down;
}

namespace {
s32 DeviceOpen(const char* path, IPCOpenMode mode) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    if (!path || mode != IPC_OPEN_NONE) return IPC_RESULT_INVALID;
    s32* slot;
    if (std::strcmp(path, "/dev/stm/immediate") == 0) slot = &state.immediate;
    else if (std::strcmp(path, "/dev/stm/eventhook") == 0) slot = &state.eventhook;
    else return IPC_RESULT_NOEXISTS;
    if (*slot >= 0) return IPC_RESULT_OPENFD;
    if (state.next_fd == std::numeric_limits<s32>::max()) return IPC_RESULT_MAXFD;
    *slot = state.next_fd++;
    return *slot;
}
s32 DeviceClose(s32 fd) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    if (fd < 0) return IPC_RESULT_INVALID;
    if (fd == state.eventhook) { state.event = {}; state.eventhook = -1; return IPC_RESULT_OK; }
    if (fd == state.immediate) { state.immediate = -1; return IPC_RESULT_OK; }
    return IPC_RESULT_INVALID;
}
s32 DeviceIoctlAsync(s32 fd, s32 type, void* in, s32 inSize, void* out, s32 outSize,
                               IPCAsyncCallback callback, void* context) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    if (fd < 0 || fd != state.eventhook || type != RegisterEvent || !callback ||
        !Buffers(in, inSize, out, outSize)) return IPC_RESULT_INVALID;
    if (state.event.callback) return IPC_RESULT_BUSY_INTERNAL;
    state.event = {out, callback, context};
    return IPC_RESULT_OK;
}
s32 DeviceIoctl(s32 fd, s32 type, void* in, s32 inSize, void* out, s32 outSize) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    if (fd < 0 || fd != state.immediate || !Buffers(in, inSize, out, outSize)) return IPC_RESULT_INVALID;
    if (type != UnregisterEvent) return IPC_RESULT_INVALID;
    state.event = {};
    return IPC_RESULT_OK;
}
}
