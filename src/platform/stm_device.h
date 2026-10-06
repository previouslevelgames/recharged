#pragma once
#include <cstdint>

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
}
