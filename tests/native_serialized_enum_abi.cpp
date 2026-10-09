// Declaration/serialized-scalar ABI only. No original audio, particle, game
// settings or challenge object is constructed by this leaf. Source conditions,
// callbacks, factories and simulation are outside its qualification.
#include "Game/Audio/AudioRpc.h"
#include "Game/Audio/AudioResourceBundle.h"
#include "Game/Audio/AudioSequenceEvent.h"
#include "Game/Effects/EffectsGroup.h"
#include "Game/DB/GameProgress.h"
#include "Game/DB/UserOptions.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace {
unsigned checks;
void Check(bool value) {
    if (!value) throw std::runtime_error("Original signed serialized enum ABI failed");
    ++checks;
}

std::array<unsigned char, 4> Wire(std::uint32_t value) {
    return {static_cast<unsigned char>(value >> 24), static_cast<unsigned char>(value >> 16),
            static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
}
std::uint32_t Word(const std::array<unsigned char, 4>& bytes) {
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16)
         | (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

template<class Record, class Enum> void Field(Enum Record::* member, std::uint32_t value) {
    // The fixed underlying type establishes representation of all 32-bit
    // patterns, including unnamed values. Samples cover each bit and high-bit
    // negative/sentinel values; this does not enumerate every 2^32 input.
    static_assert(std::is_same_v<std::underlying_type_t<Enum>, s32>);
    static_assert(sizeof(Enum) == 4 && alignof(Enum) == alignof(s32));
    static_assert(std::is_trivially_copyable_v<Record>);
    const auto raw = Wire(value);
    const auto before = raw;
    Record record{};
    record.*member = static_cast<Enum>(std::bit_cast<s32>(Word(raw)));
    Check(std::bit_cast<std::uint32_t>(record.*member) == value);
    Check(static_cast<s32>(record.*member) == std::bit_cast<s32>(value));
    std::uint32_t representation;
    std::memcpy(&representation, &(record.*member), sizeof(representation));
    Check(representation == value);
    Check(Wire(std::bit_cast<std::uint32_t>(static_cast<s32>(record.*member))) == raw);
    Check(raw == before);
}
template<class Enum> void Scalar(std::uint32_t value) {
    static_assert(std::is_same_v<std::underlying_type_t<Enum>, s32>);
    static_assert(sizeof(Enum) == 4 && alignof(Enum) == alignof(s32));
    const auto raw = Wire(value);
    const auto before = raw;
    // Same signed scalar ingress as the original tweak int and BE32 fields.
    const Enum scalar = static_cast<Enum>(std::bit_cast<s32>(Word(raw)));
    Check(std::bit_cast<std::uint32_t>(scalar) == value);
    Check(static_cast<s32>(scalar) == std::bit_cast<s32>(value));
    std::uint32_t representation;
    std::memcpy(&representation, &scalar, sizeof(representation));
    Check(representation == value);
    Check(Wire(std::bit_cast<std::uint32_t>(static_cast<s32>(scalar))) == raw);
    Check(raw == before);
}
using ForwardAxis = decltype(EffectsSpec::m_nForwardAxis);
using ChallengeCondition = decltype(StrikerChallenge::mCondition);
using GameLimit = decltype(GameplaySettings::GameLimitType);
static_assert(std::is_same_v<ForwardAxis, eFXForwardAxis>);
static_assert(std::is_same_v<ChallengeCondition, eChallengeCondition>);
static_assert(std::is_same_v<GameLimit, eGameLimitType>);
void Fields(std::uint32_t value) {
    Field(&AudioRpcDefinition::kind, value);
    Field(&AudioCueDefinition::selectionMode, value);
    Field(&AudioSequenceEventDefinition::type, value);
    Scalar<ForwardAxis>(value);
    Scalar<ChallengeCondition>(value);
    Scalar<GameLimit>(value);
}
}

int main() {
    try {
        static_assert(sizeof(s32) == 4 && std::is_signed_v<s32>);
        static_assert(AUDIO_RPC_VOLUME == 0 && AUDIO_RPC_PITCH == 1);
        static_assert(AUDIO_CUE_DISABLED == -1 && AUDIO_CUE_SEQUENTIAL == 0
            && AUDIO_CUE_RANDOM_START == 1 && AUDIO_CUE_RANDOM == 2
            && AUDIO_CUE_RANDOM_NO_REPEAT == 3 && AUDIO_CUE_SHUFFLE == 4);
        static_assert(AUDIO_EVENT_SOUND == 1 && AUDIO_EVENT_PARAMETER == 2 && AUDIO_EVENT_MARKER == 3);
        static_assert(FX_FORWARD_DIRECTION == 0 && FX_FORWARD_POSITIVE_X == 1
            && FX_FORWARD_POSITIVE_Y == 2 && FX_FORWARD_POSITIVE_Z == 3
            && FX_FORWARD_NEGATIVE_X == 4 && FX_FORWARD_NEGATIVE_Y == 5
            && FX_FORWARD_NEGATIVE_Z == 6);
        static_assert(CHALLENGE_WIN == 0 && CHALLENGE_WIN_BY_MARGIN == 1
            && CHALLENGE_SHUTOUT == 2 && CHALLENGE_WIN_WITH_MINIMUM_GOALS == 3);
        static_assert(GAME_LIMIT_TIME == 0 && GAME_LIMIT_GOALS == 1);
        for (auto value : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 0x7FFFFFFFu,
                           0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu})
            Fields(value);
        for (unsigned bit = 0; bit < 32; ++bit) {
            Fields(std::uint32_t(1) << bit);
            Fields(~(std::uint32_t(1) << bit));
        }
        std::printf("Original serialized enum ABI: %u checks; signed Wii32 representations/raw bytes only\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
