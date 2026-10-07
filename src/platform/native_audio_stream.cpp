#include "platform/native_audio_stream.h"
#include "platform/game_allocation_ownership.h"
#include "Game/Audio/AudioSource.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

namespace mscharged::platform {
namespace {
constexpr std::size_t HeaderBytes = 96;
static_assert(std::is_trivially_copyable_v<AudioStreamHeader>);
static_assert(sizeof(AudioStreamHeader) == HeaderBytes);
static_assert(offsetof(AudioStreamHeader, num_samples) == 0);
static_assert(offsetof(AudioStreamHeader, num_adpcm_nibbles) == 4);
static_assert(offsetof(AudioStreamHeader, sample_rate) == 8);
static_assert(offsetof(AudioStreamHeader, loop_flag) == 12);
static_assert(offsetof(AudioStreamHeader, format) == 14);
static_assert(offsetof(AudioStreamHeader, sa) == 16);
static_assert(offsetof(AudioStreamHeader, ea) == 20);
static_assert(offsetof(AudioStreamHeader, ca) == 24);
static_assert(offsetof(AudioStreamHeader, coef) == 28);
static_assert(offsetof(AudioStreamHeader, gain) == 60);
static_assert(offsetof(AudioStreamHeader, ps) == 62);
static_assert(offsetof(AudioStreamHeader, yn1) == 64);
static_assert(offsetof(AudioStreamHeader, yn2) == 66);
static_assert(offsetof(AudioStreamHeader, lps) == 68);
static_assert(offsetof(AudioStreamHeader, lyn1) == 70);
static_assert(offsetof(AudioStreamHeader, lyn2) == 72);
static_assert(offsetof(AudioStreamHeader, pad) == 74);
std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
         | std::uint32_t(p[2]) << 8 | p[3];
}
std::uint16_t Half(const unsigned char* p) {
    return std::uint16_t(std::uint16_t(p[0]) << 8 | p[1]);
}
}
void PrepareNativeAudioStreamHeader(void* header) {
    GameCompletedSpan completed{};
    if (!header || !FindGameCompletedSpan(header, HeaderBytes, completed))
        throw std::invalid_argument("Audio stream header requires 96 completed original NL bytes");
    const auto domain = FindGameByteDomain(header, HeaderBytes);
    if (domain == GameByteDomain::NativeHeader) return;
    if (domain != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Audio stream header has an incompatible byte domain");
    unsigned char native[HeaderBytes];
    const auto* raw = static_cast<const unsigned char*>(header);
    for (const auto at : {std::size_t(0), std::size_t(4), std::size_t(8),
                          std::size_t(16), std::size_t(20), std::size_t(24)}) {
        const auto value = Word(raw + at);
        std::memcpy(native + at, &value, sizeof value);
    }
    for (const auto at : {std::size_t(12), std::size_t(14)}) {
        const auto value = Half(raw + at);
        std::memcpy(native + at, &value, sizeof value);
    }
    for (std::size_t at = 28; at < HeaderBytes; at += 2) {
        const auto value = Half(raw + at);
        std::memcpy(native + at, &value, sizeof value);
    }
    GameByteWriteReservation conversion(header, HeaderBytes);
    std::memcpy(header, native, HeaderBytes);
    conversion.Complete(GameByteDomain::NativeHeader);
}
}
