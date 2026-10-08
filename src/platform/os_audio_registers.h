#pragma once
#include <cstdint>
#include <thread>

namespace mscharged::platform {
class NativeOSAudioBootDevice;
class NativeAXFunctionalDevice;

// One borrowed register interface to the actual attached audio chip. Binding
// adds no processor, mailbox, ROM, reset image, source flag or callback owner.
// Only the real boot or functional device can supply this platform contract.
class NativeOSAudioRegisterOwner {
public:
    ~NativeOSAudioRegisterOwner();
    NativeOSAudioRegisterOwner(const NativeOSAudioRegisterOwner&) = delete;
    NativeOSAudioRegisterOwner& operator=(const NativeOSAudioRegisterOwner&) = delete;
    // Retire routing before the real device storage/endpoints disappear.
    // Closing from a foreign thread or a register call fails without unbinding.
    void Close();
private:
    struct Operations {
        std::uint16_t (*read_dsp)(void*, std::uint32_t);
        void (*write_dsp)(void*, std::uint32_t, std::uint16_t);
        std::uint32_t (*read_dsp_pair)(void*, std::uint32_t);
        void (*write_dsp_pair)(void*, std::uint32_t, std::uint32_t);
        std::uint32_t (*read_ipc)(void*, std::uint32_t);
        void (*write_ipc)(void*, std::uint32_t, std::uint32_t);
        void* (*work_memory)(void*);
    };
    NativeOSAudioRegisterOwner(void* context, const Operations& operations);
    void* context_;
    Operations operations_;
    std::thread::id owner_;
    unsigned active_{};
    bool attached_{};
    friend class NativeOSAudioBootDevice;
    friend class NativeAXFunctionalDevice;
    friend struct NativeOSAudioRegisterCall;
};
} // namespace mscharged::platform
