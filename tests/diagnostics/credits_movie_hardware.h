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
void ServiceCreditsMovieHardware();
void ShutdownCreditsMovieHardware();

} // namespace mscharged::diagnostic
