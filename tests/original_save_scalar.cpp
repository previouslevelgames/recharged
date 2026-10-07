#include "NL/nlMain.h"
#include "Game/GameInfo.h"
#include "Game/DB/SaveLoad.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void Check(bool value, const char* message)
{
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

// Independent bitwise IEEE CRC32 oracle; deliberately does not use the source
// lookup table or its word/batching implementation.
std::uint32_t Oracle(const unsigned char* bytes, std::size_t length)
{
    std::uint32_t result = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        result ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ ((result & 1u) ? 0xEDB88320u : 0u);
    }
    return ~result;
}

void Layout()
{
    Check(sizeof(u32) == 4, "canonical Wii32 scalar width");
    Check(sizeof(UserInfo) == 0x80, "original UserInfo serialized 0x80-byte footprint");
    Check(alignof(UserInfo) == 4, "original pointer-free UserInfo alignment");
    Check(sizeof(((UserInfo*)nullptr)->mSaveID) == 4, "original save-id scalar width");
    Check(std::is_standard_layout_v<UserInfo>, "UserInfo has a defined member-offset layout");
    Check(std::is_trivially_copyable_v<UserInfo>, "UserInfo supports source byte-copy operations");
    Check(offsetof(UserInfo, mSaveID) == 0x00, "retail save-id member offset");
    Check(offsetof(UserInfo, mAudioOptions) == 0x04, "retail audio-settings member offset");
    Check(offsetof(UserInfo, mVisualOptions) == 0x1C, "retail visual-settings member offset");
    Check(offsetof(UserInfo, mGameplayOptions) == 0x24, "retail gameplay-settings member offset");
    Check(offsetof(UserInfo, mCheatOptions) == 0x40, "retail cheat-settings member offset");
    Check(offsetof(UserInfo, mAltGameplayOptions) == 0x4C, "retail alternate-gameplay member offset");
    Check(offsetof(UserInfo, mAltCheatOptions) == 0x68, "retail alternate-cheat member offset");
    Check(offsetof(UserInfo, mNumGamesPlayed) == 0x74, "retail games-played member offset");
    Check(offsetof(UserInfo, mNumGoalsScored) == 0x76, "retail goals-scored member offset");
    Check(offsetof(UserInfo, mNumSTSAttempts) == 0x78, "retail STS-attempts member offset");
    Check(offsetof(UserInfo, mNumPerfectPasses) == 0x7A, "retail perfect-passes member offset");
    Check(offsetof(UserInfo, mNumHits) == 0x7C, "retail hit-count member offset");
    Check(sizeof(SaveFileHeader) == 8, "unchanged original save-file header width");
    std::printf("UserInfo: bytes=%zu align=%zu save-id=%zu audio=%zu last-count=%zu\n",
        sizeof(UserInfo), alignof(UserInfo), sizeof(((UserInfo*)nullptr)->mSaveID),
        offsetof(UserInfo, mAudioOptions), offsetof(UserInfo, mNumHits));
}
}

int main()
{
    Layout();
    alignas(8) unsigned char bytes[96];
    for (std::size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = static_cast<unsigned char>(i * 83 + 19);
    Check(reinterpret_cast<std::uintptr_t>(bytes) > std::numeric_limits<std::uint32_t>::max(),
        "actual checksum input address is above4GiB");
    const unsigned char known[] = "123456789";
    Check(Oracle(known, 9) == 0xCBF43926u, "independent known CRC32 oracle");
    Check(nlChecksum32(known, 9) == 0xCBF43926u, "original whole-source known CRC32 vector");
    RunningChecksum initial;
    Check(initial.m_nChecksum == 0xFFFFFFFFu, "original running-checksum initial state");

    const std::size_t lengths[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32};
    for (std::size_t offset = 0; offset < 4; ++offset) {
        for (std::size_t length : lengths) {
            const auto expected = Oracle(bytes + offset, length);
            Check(nlChecksum32(bytes + offset, length) == expected,
                "whole-source byte CRC preserves original input bytes");
            RunningChecksum streamed;
            streamed.ChecksumData(bytes + offset, length);
            Check(~streamed.m_nChecksum == expected,
                "whole-source prefix/4-byte words/tail preserve original input bytes");
        }
    }
    for (std::size_t offset = 0; offset < 4; ++offset) {
        RunningChecksum streamed;
        streamed.ChecksumData(bytes + offset, 3);
        streamed.ChecksumData(bytes + offset + 3, 7);
        streamed.ChecksumData(nullptr, 0);
        streamed.ChecksumData(bytes + offset + 10, 5);
        streamed.ChecksumData(bytes + offset + 15, 17);
        Check(~streamed.m_nChecksum == Oracle(bytes + offset, 32),
            "original running checksum survives split calls and empty segment");
    }
    Check(nlChecksum32(nullptr, 0) == 0, "original empty checksum result");
    std::printf("Original save scalar/checksum: %u checks, %u failures\n",
        checks, failures);
    return failures ? 1 : 0;
}
