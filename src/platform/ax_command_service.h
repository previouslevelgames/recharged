#pragma once
#include "platform/dsp_memory.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace mscharged::platform {
// The command numbers and lengths are the single Wii list written by the
// original AXCL.c. No old firmware layout detection or unknown-command fallback.
enum class NativeAXOpcode : std::uint16_t {
    Setup=0, AddToLR=1, SubToLR=2, AddSubToLR=3, Process=4,
    MixAuxA=5, MixAuxB=6, MixAuxC=7, UploadAuxAMixLRSC=8,
    UploadAuxBMixLRSC=9, Compressor=10, Output=11, OutputDPL2=12,
    RemoteOutput=13, End=14
};
enum class NativeAXFailure {
    InvalidListExtent, UnknownCommand, TruncatedCommand, MissingEnd,
    UnsupportedSequence, NonzeroStudio, NonzeroSurround,
    ActiveVoice, InvalidVoiceChain, AuxiliaryProcessing,
    UnsupportedCompressor
};
class NativeAXCommandError : public std::runtime_error {
public:
    NativeAXCommandError(NativeAXFailure failure, std::size_t word,
                         const char* message);
    NativeAXFailure failure() const noexcept { return failure_; }
    std::size_t word() const noexcept { return word_; }
private:
    NativeAXFailure failure_;
    std::size_t word_;
};
struct NativeAXCommand {
    NativeAXOpcode opcode{};
    std::uint16_t word_offset{}, argument_count{};
    std::array<std::uint16_t,13> arguments{};
};
struct NativeAXCommandList {
    std::array<NativeAXCommand,64> commands{};
    std::uint16_t command_count{}, consumed_words{};
};
NativeAXCommandList ReadNativeAXCommandList(NativeDSPMemoryEndpoint endpoint,
                                          std::uint32_t address, std::size_t bytes);
struct NativeAXZeroFrameResult {
    std::uint16_t consumed_words{}, stopped_voices{};
    std::uint16_t stereo_frames{}, remote_samples_per_channel{};
    std::uint32_t written_bytes{};
    bool compressor_present{}, dpl2{};
};
// This is an explicitly bounded device conformance slice, not a complete AX
// kernel or a boot endpoint. It accepts only verified zero contributors and
// stopped PBs; unsupported nonzero/voice/aux work is an error, never silence.
// The caller retains all pins and excludes source CPU mutation throughout this
// operation using the original request/completion boundary. No raw pointers
// escape the checked bus, no source callbacks/flags/mails are changed, and no
// kernel-ready operation is exposed. A zero input leaves fresh compressor
// attenuation history at zero; arbitrary prior compressor history is unsupported.
NativeAXZeroFrameResult ExecuteNativeAXZeroInputFrame(NativeDSPMemoryEndpoint endpoint,
                                                     std::uint32_t address,
                                                     std::size_t bytes);
} // namespace mscharged::platform
