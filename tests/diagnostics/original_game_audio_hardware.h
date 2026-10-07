#pragma once
#include "platform/ax_functional_device.h"
#include "platform/native_ax_module_memory.h"
#include <memory>

namespace mscharged::diagnostic {
// Original source owns AIInit/AXInit/MIXInit. Construct only after the actual
// module's priority101 hook reserved its thirteen spans and ConfirmLoaded ran.
// No boot request, source flag or callback is issued by this attachment.
class OriginalGameAudioHardware {
public:
    explicit OriginalGameAudioHardware(platform::NativeAXModuleMemory& memory);
    ~OriginalGameAudioHardware();
    OriginalGameAudioHardware(const OriginalGameAudioHardware&) = delete;
    OriginalGameAudioHardware& operator=(const OriginalGameAudioHardware&) = delete;
    platform::NativeAXFunctionalStatus Status() const;
    // After original sounds are idle, stop borrowing the hardware owner before
    // that owner stops/drains AI. No source flag/callback or device request follows.
    void UnbindOwnerService();
    // Original THP Quit and actual AI stop/drain must already return. Legacy
    // no-bank callers retire source owners before this call; paired callbacks
    // retain them through HALT and execute original bank/source retirement here.
    // The module first handle and source task/CRT owners remain live at exit.
    // Optional paired original callbacks retire idle banks and source owners
    // after device close, before releasing any sample/static endpoint lifetime.
    // Source active handles must have retired naturally through original tasks.
    void CloseAfterAIStop(void (*retire_silence_pin)(),
        void (*unload_idle_banks)() = nullptr, void (*shutdown_source)() = nullptr);
private:
    static void ServiceDevice(void* context);
    bool service_bound_{};
    platform::NativeAXModuleMemory& memory_;
    std::unique_ptr<platform::NativeAXFunctionalDevice> device_;
};
} // namespace mscharged::diagnostic
