#include "original_game_audio_hardware.h"
#include "credits_movie_hardware.h"
#include "platform/ai.h"
#include "platform/dsp_control_abi.h"
#include <stdexcept>

namespace mscharged::diagnostic {
OriginalGameAudioHardware::OriginalGameAudioHardware(platform::NativeAXModuleMemory& memory)
    : memory_(memory) {
    using namespace platform;
    const auto owner = memory_.Status();
    if (!owner.loaded || !owner.reserved || owner.retired || (owner.spans != CHARGED_AX_BASE_STORAGE_COUNT && owner.spans != CHARGED_AX_HBM_STORAGE_COUNT) ||
        owner.before.memory_initialized || !owner.after.memory_initialized)
        throw std::logic_error("Original audio attachment requires actual prestatic AX source spans");
    if (GetNativeAIStatus().initialized)
        throw std::logic_error("Original Backend must own the first native AIInit");
    const auto mail = AttachNativeDSPMailboxes();
    try {
        const auto control = AttachNativeDSPControl(mail);
        try {
            NativeAXFunctionalBindings bindings{};
            for (unsigned i = 0; i != 13; ++i)
                bindings.addresses[i] = memory_.PhysicalAddress(i);
            device_ = std::make_unique<NativeAXFunctionalDevice>(memory_.Endpoint(),
                mail, control, bindings, NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1);
            try {
                BindCreditsMovieDeviceService(ServiceDevice, this);
                service_bound_ = true;
            } catch (...) {
                // Cold device attach failed to borrow the existing owner; no
                // source request has run. Retire the actual processor safely.
                ChargedDSPControlWrite(ChargedDSPControlRead() | 4u);
                device_->Close();
                device_.reset();
                throw;
            }
        } catch (...) { DetachNativeDSPControl(); throw; }
    } catch (...) { DetachNativeDSPMailboxes(); throw; }
}

OriginalGameAudioHardware::~OriginalGameAudioHardware() = default;

platform::NativeAXFunctionalStatus OriginalGameAudioHardware::Status() const {
    if (!device_) throw std::logic_error("Original audio device has retired");
    return device_->Status();
}

void OriginalGameAudioHardware::ServiceDevice(void* context) {
    auto& hardware = *static_cast<OriginalGameAudioHardware*>(context);
    if (!hardware.device_ || !hardware.service_bound_)
        throw std::logic_error("Native audio service borrowed a retired device");
    hardware.device_->ServiceOwner();
}

void OriginalGameAudioHardware::UnbindOwnerService() {
    if (!device_ || !service_bound_)
        throw std::logic_error("Native audio device service is already unbound");
    UnbindCreditsMovieDeviceService(this);
    service_bound_ = false;
}

void OriginalGameAudioHardware::CloseAfterAIStop(void (*retire_silence_pin)(),
    void (*unload_idle_banks)(), void (*shutdown_source)()) {
    using namespace platform;
    if (!device_ || !retire_silence_pin || service_bound_)
        throw std::logic_error("Original audio retirement requires a live source pin receiver");
    if (bool(unload_idle_banks) != bool(shutdown_source))
        throw std::logic_error("Original bank retirement and source shutdown must remain paired");
    const auto ai = GetNativeAIStatus();
    if (ai.initialized || ai.running || ai.callback_active || ai.retained_blocks || ai.queued_input_bytes)
        throw std::logic_error("Actual native AI must stop and drain before audio retirement");
    device_->ServiceOwner();
    // Real host hardware fence, not a source task/initialized flag replacement.
    ChargedDSPControlWrite(ChargedDSPControlRead() | 4u);
    device_->Close();
    device_.reset();
    // Source allocations and the module are still owned after processor close.
    // Retire original bank callbacks/allocations before device-memory detach.
    if (unload_idle_banks) {
        unload_idle_banks();
        shutdown_source();
    }
    retire_silence_pin();
    memory_.ReleaseAfterDeviceDrain();
    DetachNativeDSPControl();
    DetachNativeDSPMailboxes();
    DetachNativeDSPMEM1();
}
} // namespace mscharged::diagnostic
