#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace mscharged::resources
{
struct ThpMovieInfo
{
    std::uint32_t width=0,height=0,video_type=0,frame_count=0;
    float frame_rate=0;
    std::uint32_t channels=0,sample_rate=0,total_audio_samples=0,max_audio_samples=0;
    std::uint32_t max_frame_bytes=0,first_frame_bytes=0,data_offset=0,data_bytes=0,last_frame_offset=0;
    std::array<std::uint8_t,2> components{};
    unsigned component_count=0;
};
// THP1.1 metadata (original Charged profile): one video and optional one audio
// track. Pass a prefix covering the full descriptor table and actual file size.
ThpMovieInfo ReadThpMovieInfo(std::span<const std::uint8_t> prefix,std::uint64_t file_size);
struct ThpMovieFrame
{
    std::uint32_t index=0,file_offset=0,encoded_bytes=0,next_frame_bytes=0;
    std::uint32_t width=0,height=0,sample_rate=0;
    double video_seconds=0;
    std::uint64_t first_audio_sample=0;
    std::uint32_t audio_samples=0;
    // GX I8 8x4 tiled planes, with separate half-resolution chroma planes.
    std::vector<std::uint8_t> y,u,v;
    // Actual SDK interleaved RIGHT,LEFT channel order; mono is duplicated.
    // Playback transport must explicitly map this to its channel convention.
    std::vector<std::int16_t> pcm_right_left;
};
using ThpMovieFrameHandle=std::shared_ptr<const ThpMovieFrame>;
// Caller supplies a sequential frame's exact original range and prior size.
// No output is published on bounds/decoder failure. Decoding is not playback.
ThpMovieFrameHandle DecodeThpMovieFrame(const ThpMovieInfo&,std::span<const std::uint8_t>,
    std::uint32_t index,std::uint32_t offset,std::uint32_t previous_size,std::uint64_t first_audio_sample);
}
