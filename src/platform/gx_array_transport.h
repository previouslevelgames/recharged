#pragma once

#include "platform/game_allocation_ownership.h"
#include <dolphin/gx/GXGeometry.h>
#include <limits>
#include <stdexcept>

namespace mscharged::platform
{
// Original source chooses the attribute, address, stride and call order. The
// host API additionally requires a genuinely owned completed extent/endian.
inline void SetGameGraphicsArray(GXAttr attribute, const void* data, u8 stride)
{
    const auto span = ResolveGameGraphicsArray(data);
    if (span.bytes > std::numeric_limits<u32>::max())
        throw std::invalid_argument("Actual GX array extent exceeds the native SDK u32 size ABI");
    GXSetArray(attribute, data, static_cast<u32>(span.bytes), stride, span.little_endian);
}
}
