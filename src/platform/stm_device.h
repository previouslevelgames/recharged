#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace mscharged::platform {
struct StmInput { std::uint64_t generation; };
// Prepare actual native device endpoints before original __OSInitSTM; service
// source callbacks only on the SDK owner. Workers only latch physical/UI input.
void InitializeNativeSTMDevice();
void ShutdownNativeSTMDevice();
StmInput GetNativeSTMInput();
bool SubmitNativeSTMPower(StmInput input);
bool SetNativeSTMResetButton(StmInput input, bool pressed);
bool ServiceNativeSTMDevice();

struct NativeSTMPowerRequest {
    std::uint64_t generation;
    std::array<std::uint8_t, 32> input;
    std::uint64_t instruction_cache_sequence;
};
struct NativeSTMPowerRemoval {
    void* context;
    // Borrowed native owner verification, called outside IOS/STM locks at the
    // source terminal wait with the original IRQ mask retained. Observation
    // only: reject unfinished hardware/readers/jobs; do not service callbacks,
    // wait, join, retire devices/modules, or change source fields. The context
    // remains alive until actual process removal. No implicit policy.
    void (*verify_quiescent)(void*, const NativeSTMPowerRequest&);
};
void ConfigureNativeSTMPowerRemoval(NativeSTMPowerRemoval policy);
// Owner-only observation of this exact live device's configured native policy;
// no source readiness or proof of completed shutdown.
bool IsNativeSTMPowerRemovalConfigured(StmInput input);
// Read-only receipt of genuine /dev/stm/immediate command0x2003, not a source
// initialized flag or proof of full shutdown. Restart0x2001 stays unsupported.
std::optional<NativeSTMPowerRequest> GetNativeSTMPowerRequest();
}
