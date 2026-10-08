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
// While that owner can take the AID cause, a completed block waits for its
// delivery before latching the next registers, as prompt Wii interrupts would;
// an owner inside a masked section still gets the hardware's register replay.
bool ServiceNativeAI();
NativeAIStatus GetNativeAIStatus();
void ShutdownNativeAI();

// Host output volume (0..1) of the desktop speaker stream, like a TV volume
// control: it scales only the device playback after the DMA FIFO. Source PCM,
// DMA timing and callbacks are unchanged. Applies to the current and future
// AIInit streams; values outside 0..1 or non-finite values are rejected.
void SetNativeAIOutputGain(float gain);
float GetNativeAIOutputGain();

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
// after already-transferred PCM covers its actual pull quantum, one DMA quantum
// and a 12 ms owner delivery allowance; no future source PCM or interrupt is
// generated for this lead.
// Separate return ABI keeps the existing three status structures unchanged.
struct NativeAIOutputStatus {
    std::uint64_t dma_start_ns{};
    std::uint64_t device_start_ns{};
    std::uint64_t required_output_frames{};
    std::uint64_t ready_output_frames_at_start{};
    std::uint64_t transferred_input_frames_at_start{};
};
NativeAIOutputStatus GetNativeAIOutputStatus();

// Optional delivery observations inside the Begin/End window above. A separate
// fixed64 return ABI leaves the existing four structures unchanged. These
// classify actual latched source blocks, owner callback delivery and SDL
// demand; they never select a buffer, create PCM or deliver a callback.
struct NativeAIDeliveryStatus {
    // Completed96-frame blocks whose next hardware registers were latched.
    std::uint64_t latched_blocks{};
    // Latches of registers not reprogrammed by AIInitDMA since the previous
    // latch: the DMA replays the previously latched source buffer address.
    std::uint64_t replayed_latches{};
    // Hardware causes raised while an earlier cause was still undelivered.
    std::uint64_t coalesced_causes{};
    // Owner callback delivery measured from the DMA clock time of its cause.
    std::uint64_t dispatched_callbacks{};
    std::uint64_t maximum_dispatch_latency_ns{};
    std::uint64_t total_dispatch_latency_ns{};
    // Callbacks delivered after one/two/four complete DMA block periods.
    std::uint64_t callbacks_after_one_block{};
    std::uint64_t callbacks_after_two_blocks{};
    std::uint64_t callbacks_after_four_blocks{};
    // Exact digital-zero content of transferred blocks (source production).
    std::uint64_t silent_blocks{};
    std::uint64_t zero_tail_blocks{};
    std::uint64_t zero_tail_frames{};
    // SDL demand that already transferred source PCM did not satisfy; SDL
    // supplies silence for this shortfall. Input-format bytes.
    std::uint64_t sdl_short_pulls{};
    std::uint64_t sdl_short_input_bytes{};
    // Device pulls that found the source buffers inside a critical section.
    std::uint64_t sdl_pulls_without_source_access{};
    // Smallest queued input after a device pull, once output has started.
    std::uint64_t minimum_queued_input_bytes{};
    std::uint64_t maximum_queued_input_bytes{};
    // Completed blocks that waited for owner delivery of the previous cause,
    // the longest such wait and DMA clock cells dropped by bounded catch-up.
    std::uint64_t held_boundaries{};
    std::uint64_t maximum_hold_ns{};
    std::uint64_t skipped_cells{};
};
NativeAIDeliveryStatus GetNativeAIDeliveryStatus();

} // namespace mscharged::platform
