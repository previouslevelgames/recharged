#include "platform/os_audio_registers.h"
#include "platform/os_audio_boot_abi.h"
#include "platform/interrupts.h"
#include <limits>
#include <stdexcept>

namespace mscharged::platform {
namespace {
NativeOSAudioRegisterOwner* current{};
}
NativeOSAudioRegisterOwner::NativeOSAudioRegisterOwner(void* context, const Operations& operations)
    : context_(context), operations_(operations), owner_(std::this_thread::get_id()) {
    NativeInterruptGuard exclusion;
    if (current || !context || !operations.read_dsp || !operations.write_dsp ||
        !operations.read_dsp_pair || !operations.write_dsp_pair ||
        !operations.read_ipc || !operations.write_ipc || !operations.work_memory)
        throw std::logic_error("OS audio registers require one complete borrowed device owner");
    current = this;
    attached_ = true;
}
NativeOSAudioRegisterOwner::~NativeOSAudioRegisterOwner() {
    if (attached_) std::terminate();
}
void NativeOSAudioRegisterOwner::Close() {
    NativeInterruptGuard exclusion;
    if (!attached_ || current != this || owner_ != std::this_thread::get_id() || active_)
        throw std::logic_error("OS audio register routing must retire on its inactive device owner");
    current = nullptr;
    attached_ = false;
}
struct NativeOSAudioRegisterCall {
    NativeOSAudioRegisterOwner* owner;
    void* context;
    NativeOSAudioRegisterOwner::Operations operations;
    NativeOSAudioRegisterCall() {
        NativeInterruptGuard exclusion;
        if (!current || current->owner_ != std::this_thread::get_id())
            throw std::logic_error("original OS audio MMIO has no matching live native owner");
        if (current->active_ == std::numeric_limits<unsigned>::max())
            throw std::overflow_error("OS audio register call depth exhausted");
        owner = current;
        context = owner->context_;
        operations = owner->operations_;
        ++owner->active_;
    }
    ~NativeOSAudioRegisterCall() {
        NativeInterruptGuard exclusion;
        --owner->active_;
    }
};
} // namespace mscharged::platform

extern "C" std::uint16_t ChargedOSAudioDSPRead(std::uint32_t reg) {
    mscharged::platform::NativeOSAudioRegisterCall call;
    return call.operations.read_dsp(call.context, reg);
}
extern "C" void ChargedOSAudioDSPWrite(std::uint32_t reg, std::uint16_t value) {
    mscharged::platform::NativeOSAudioRegisterCall call;
    call.operations.write_dsp(call.context, reg, value);
}
extern "C" std::uint32_t ChargedOSAudioDSPReadPair(std::uint32_t reg) {
    mscharged::platform::NativeOSAudioRegisterCall call;
    return call.operations.read_dsp_pair(call.context, reg);
}
extern "C" void ChargedOSAudioDSPWritePair(std::uint32_t reg, std::uint32_t value) {
    mscharged::platform::NativeOSAudioRegisterCall call;
    call.operations.write_dsp_pair(call.context, reg, value);
}
extern "C" std::uint32_t ChargedOSAudioIPCRead(std::uint32_t reg) {
    mscharged::platform::NativeOSAudioRegisterCall call;
    return call.operations.read_ipc(call.context, reg);
}
extern "C" void ChargedOSAudioIPCWrite(std::uint32_t reg, std::uint32_t value) {
    mscharged::platform::NativeOSAudioRegisterCall call;
    call.operations.write_ipc(call.context, reg, value);
}
extern "C" void* ChargedOSAudioWorkMemory() {
    mscharged::platform::NativeOSAudioRegisterCall call;
    return call.operations.work_memory(call.context);
}
