#pragma once

namespace mscharged::diagnostic {

// Selected original Credits/MoviePlayer diagnostic only. The original source
// owns THPSimple initialization, reads, decoding, mixing and playback decisions.
// This endpoint owns the one host AI device and its owner-thread safe points.
void InitializeCreditsMovieHardware();
// Optional borrowed input endpoint, prepared on the same owner first. The
// existing AI endpoint remains the sole SDK registration and services AI first.
// Caller must retire this SDK owner before freeing/retiring borrowed input.
void InitializeCreditsMovieHardware(void (*service_input)());
// Register the same genuine SDK owner without AIInit; original Backend
// issues the first AI/AX/MIX initialization. No initialized source flag follows.
void InitializeOriginalGameAudioHardware(void (*service_input)());
// Borrow one live native processor into the existing owner. Bind/unbind only
// on the owner outside a service callback; unbind before device/owner retirement.
void BindCreditsMovieDeviceService(void (*service_device)(void*), void* context);
void UnbindCreditsMovieDeviceService(void* context);
void ServiceCreditsMovieHardware();
void ShutdownCreditsMovieHardware();

} // namespace mscharged::diagnostic
