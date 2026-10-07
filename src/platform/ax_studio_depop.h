#pragma once
#include <array>
#include <cstdint>
namespace mscharged::platform {
// Literal owned Setup ramp. Source AXSPB alone supplies value/delta and owns
// next-frame accumulation. No source value/visibility/timing is changed here.
// In40-bit mode each stored bus has saturated AC.M and unchanged AC.L. The
// original zero-value fast path ignores delta. These functions supply no
// remote output/mixing, command/device readiness or source callback.
std::array<std::int32_t,96> NativeAXStudioDepop96(std::int32_t value,std::int16_t delta);
std::array<std::int32_t,18> NativeAXStudioDepop18(std::int32_t value,std::int16_t delta);
}
