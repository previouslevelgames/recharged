#pragma once

#include <aurora/gfx.h>
#include <atomic>
#include <cstdint>

namespace mscharged::platform
{
inline std::uint32_t GetQueuedPipelineCount()
{
    // Aurora exposes a const view of mutable stats. Match its worker's
    // atomic_ref<uint32_t>; older libc++ cannot load atomic_ref<const T>.
    auto& counter = const_cast<std::uint32_t&>(aurora_get_stats()->queuedPipelines);
    return std::atomic_ref<std::uint32_t>(counter).load();
}
}
