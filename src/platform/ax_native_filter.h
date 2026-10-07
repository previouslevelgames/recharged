#pragma once
#include "platform/ax_active_voice.h"

namespace mscharged::platform {
// Explicit native platform signal-processing policy, not Wii ROM coefficients.
// No library/bank input or device/ISA initialization is supplied by this class.
// Windowed sinc/Lanczos2, normalized DC,128 phases and4 signedQ15 taps. Source
// bank0/1/2 selects nominal8/12/16k at32k. The causal interpolation center is
// history index1+phase/128; this native delay/response is not retail parity.
class NativeAXNativeFilter {
public:
    NativeAXNativeFilter();
    NativeAXCoefficientView View() const noexcept;
    std::array<std::int16_t,4> Row(std::uint16_t bank,std::uint16_t fraction) const;
private:
    std::array<std::array<std::array<std::int16_t,4>,128>,3> rows_{};
};
} // namespace mscharged::platform
