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

// Optional host observations only. All16 fields are fixed64-bit counters/times;
// this new return ABI leaves NativeAIStatus/NativeAIClockStatus unchanged.
// Begin resets observations without resetting source DMA, its epoch or PCM.
// Callback gaps include intentional playback pauses within the sampled window.
struct NativeAIObservationStatus {
    std::uint64_t observation_start_ns{};
    std::uint64_t observed_elapsed_ns{};
    std::uint64_t callbacks_started{};
    std::uint64_t callbacks_completed{};
    std::uint64_t callbacks_failed{};
    std::uint64_t first_callback_start_ns{};
    std::uint64_t last_callback_start_ns{};
    std::uint64_t maximum_callback_start_gap_ns{};
    std::uint64_t total_callback_duration_ns{};
    std::uint64_t maximum_callback_duration_ns{};
    std::uint64_t sdl_pull_calls{};
    std::uint64_t last_sdl_pull_ns{};
    std::uint64_t maximum_sdl_pull_gap_ns{};
    std::uint64_t total_additional_input_bytes_requested{};
    std::uint64_t maximum_additional_input_bytes_requested{};
    std::uint64_t maximum_total_input_bytes_requested{};
};
// Begin/end run on the actual AI initialization owner, outside its callback.
// Observations default off and never supply a source callback or extra PCM.
void BeginNativeAIObservations();
void EndNativeAIObservations();
NativeAIObservationStatus GetNativeAIObservationStatus();

// The hardware DMA clock starts immediately. The separate host device starts
// after already-transferred PCM covers its actual pull quantum plus one DMA
// quantum; no future source PCM or interrupt is generated for this lead.
// Separate return ABI keeps the existing three status structures unchanged.
struct NativeAIOutputStatus {
    std::uint64_t dma_start_ns{};
    std::uint64_t device_start_ns{};
    std::uint64_t required_output_frames{};
    std::uint64_t ready_output_frames_at_start{};
    std::uint64_t transferred_input_frames_at_start{};
};
NativeAIOutputStatus GetNativeAIOutputStatus();

} // namespace mscharged::platform
