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

// Hardware DMA follows its nominal sample clock and latches source buffer
// selection. SDL consumes each resulting FIFO block once; its pull size and
// buffer-release callback do not determine source interrupts. Original
// callbacks execute only at initialization/game-thread hardware safe points.
bool ServiceNativeAI();
NativeAIStatus GetNativeAIStatus();
void ShutdownNativeAI();

// Separate structure leaves the existing NativeAIStatus return ABI unchanged.
struct NativeAIClockStatus {
    std::uintptr_t active_source_address{};
    std::uint32_t active_dma_bytes{};
    std::uint32_t transferred_current_bytes{};
    std::uint64_t transferred_cells{};
    std::uint64_t latch_edges{};
    std::uint64_t coalesced_edges{};
    std::uint64_t maximum_service_gap_ns{};
    std::uint64_t clock_elapsed_ns{};
    std::uint64_t epoch_transferred_cells{};
};
NativeAIClockStatus GetNativeAIClockStatus();

} // namespace mscharged::platform
