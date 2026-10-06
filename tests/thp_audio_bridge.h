#pragma once
#include <cstdint>
struct ChargedMovieAudioResult {
    unsigned checks, decoded_frames, blocked_ring, waiting_read;
    unsigned width, height, frames, work_bytes, input_rate;
    std::uint64_t callbacks, consumed_blocks, submitted_blocks;
};
