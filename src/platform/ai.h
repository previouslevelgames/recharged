#pragma once

#include <cstdint>

namespace mscharged::platform {

struct NativeAIStatus {
    bool initialized{};
    bool running{};
    bool interrupt_pending{};
    bool callback_active{};
    std::uintptr_t source_address{};
    std::uint32_t dma_bytes{};
    std::uint32_t dsp_rate{};
    int input_frequency{};
    int device_frequency{};
    int device_frames{};
    int queued_input_bytes{};
    std::uint32_t device_id{};
    std::uint64_t submitted_blocks{};
    std::uint64_t consumed_blocks{};
    std::uint64_t cancelled_blocks{};
    std::uint64_t dispatched_callbacks{};
    std::uint64_t last_input_hash{};
    std::uint64_t retained_blocks{};
    bool maps_right_left_to_left_right{};
};

// SDL consumes copied native PCM and latches hardware completion. Original
// callbacks execute only when the initialization/game thread services it.
bool ServiceNativeAI();
NativeAIStatus GetNativeAIStatus();
void ShutdownNativeAI();

} // namespace mscharged::platform
