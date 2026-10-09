#include "platform/stm_device.h"
#include "platform/stm_hardware_abi.h"
#include "platform/ios_device.h"
#include "platform/interrupts.h"
#include "platform/instruction_cache.h"
#include "platform/thread_queues.h"
#include "platform/thread_registry_abi.h"
#include <aurora/video.h>
#include <revolution/ipc.h>

#include <array>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr s32 RegisterEvent = 0x1000, UnregisterEvent = 0x3002;
constexpr s32 PowerOff = 0x2003;
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
    mscharged::platform::NativeSTMPowerRemoval power_removal{};
    std::optional<mscharged::platform::NativeSTMPowerRequest> power_request;
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
        state.power_removal = {};
        state.power_request.reset();
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
        if (state.power_request) throw std::logic_error("Cannot retire received STM power request");
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
    state.power_removal = {};
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

void ConfigureNativeSTMPowerRemoval(NativeSTMPowerRemoval policy) {
    if (!policy.context || !policy.verify_quiescent)
        throw std::invalid_argument("STM power removal needs a retained native owner verification");
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("STM power policy cannot be installed in a retained guard or IRQ");
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    if (state.active || state.power_request || state.power_removal.verify_quiescent)
        throw std::logic_error("STM power policy already owns a live request or callback");
    state.power_removal = policy;
}

bool IsNativeSTMPowerRemovalConfigured(StmInput input) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    return input.generation == state.generation && state.power_removal.context &&
        state.power_removal.verify_quiescent;
}

std::optional<NativeSTMPowerRequest> GetNativeSTMPowerRequest() {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    return state.power_request;
}
}

extern "C" bool mscharged_stm_reset_button_pressed() {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    return state.reset_down;
}

extern "C" void mscharged_stm_terminal_wait(void) {
    using namespace mscharged::platform;
    if (NativeInterruptsEnabled() || !NativeInterruptWaitAllowed())
        throw std::logic_error("STM terminal wait requires the source masked caller, outside guards and IRQs");
    NativeSTMPowerRequest request;
    NativeSTMPowerRemoval policy;
    {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        RequireOwner(state);
        if (state.active || state.event.callback || !state.power_request || !state.power_removal.verify_quiescent)
            throw std::logic_error("STM terminal power removal is unsupported or still owns a source callback");
        request = *state.power_request;
        policy = state.power_removal;
    }
    // The actual IOS call has returned. Neither its routing mutex nor the STM
    // latch is held here; this is a hardware wait in the original LockUp loop.
    const auto ios = GetNativeIOSStatus();
    bool enabled;
    if (ios.pending || ios.active || !aurora_get_video_display_enabled(&enabled) || enabled)
        throw std::logic_error("STM power removal requires drained IOS receivers and real disabled VI output");
    if (NativeInstructionCacheSequence() <= request.instruction_cache_sequence)
        throw std::logic_error("STM terminal caller omitted original instruction-cache synchronization");
    (void)ValidateNativeThreadsForPowerRemoval();
    policy.verify_quiescent(policy.context, request);
    if (policy.before_removal) policy.before_removal(policy.context);
    // Actual configured removal of this native game instance. Retain source
    // storage until the OS ends the process; never run destructors or return
    // success into the original infinite loop. Host restart is not supplied.
    std::fflush(nullptr);
    std::_Exit(0);
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
    if (type == UnregisterEvent) {
        state.event = {};
        return IPC_RESULT_OK;
    }
    if (type != PowerOff || !state.power_removal.verify_quiescent) return IPC_RESULT_INVALID;
    // This implementation accepts only the exact source shutdown-to-SBY
    // request. Its remaining input words and output bytes stay untouched.
    u32 first;
    std::memcpy(&first, in, sizeof(first));
    if (first || reinterpret_cast<std::uintptr_t>(in) % 32 ||
        reinterpret_cast<std::uintptr_t>(out) % 32) return IPC_RESULT_INVALID;
    if (state.power_request) return IPC_RESULT_BUSY_INTERNAL;
    if (state.active || state.event.callback || !mscharged::platform::NativeInterruptWaitAllowed())
        throw std::logic_error("STM power request still owns an active or registered source callback");
    const auto ios = mscharged::platform::GetNativeIOSStatus();
    if (ios.pending || ios.active)
        throw std::logic_error("STM power request retains undelivered IOS source receivers");
    mscharged::platform::NativeSTMPowerRequest request{state.generation, {},
        mscharged::platform::NativeInstructionCacheSequence()};
    std::memcpy(request.input.data(), in, request.input.size());
    state.power_request = request;
    return IPC_RESULT_OK; // Genuine queued device command, not completed shutdown.
}
}
