#include "platform/thp_data_abi.h"
#include "RVL_SDK/thp/THPSimple.h"
#include <dolphin/thp.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
std::uint64_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

Bytes Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Check(bool(stream), "fixture file missing");
    return Bytes(std::istreambuf_iterator<char>(stream), {});
}

std::vector<std::uint32_t> Expected(const std::filesystem::path& path) {
    std::ifstream stream(path);
    std::vector<std::uint32_t> values;
    std::uint32_t value;
    while (stream >> std::hex >> value) values.push_back(value);
    return values;
}

bool Guard(const Bytes& storage, std::size_t begin, std::size_t size) {
    return std::all_of(storage.begin(), storage.begin() + begin, [](auto b) { return b == 0xa5; }) &&
           std::all_of(storage.begin() + begin + size, storage.end(), [](auto b) { return b == 0xa5; });
}

void Metadata(const std::filesystem::path& folder) {
    using namespace mscharged::platform;
    auto raw = Read(folder / "metadata.bin");
    auto words = Expected(folder / "metadata.txt");
    Check(raw.size() == 160 && words.size() == 51, "metadata oracle size differs");
    for (std::size_t alignment = 0; alignment < 8; ++alignment) {
        Bytes input(raw.size() + 16, 0xa5);
        const auto offset = alignment + 1;
        std::copy(raw.begin(), raw.end(), input.begin() + offset);
        auto* bytes = input.data() + offset;
        if constexpr (sizeof(std::uintptr_t) > 4)
            Check(reinterpret_cast<std::uintptr_t>(bytes) > UINT32_MAX, "fixture did not exercise native high pointers");

        Bytes native(64, 0xa5);
        ExpandTHPHeader(native.data() + offset, bytes);
        Check(Guard(native, offset, 48), "header conversion changed guard bytes");
        Check(std::equal(native.begin() + offset, native.begin() + offset + 4, bytes), "magic bytes changed");
        for (unsigned n = 0; n < 11; ++n) {
            std::uint32_t word;
            std::memcpy(&word, native.data() + offset + 4 + n * 4, 4);
            Check(word == words[n], "header word/float bits differ from independent oracle");
        }
        THPFrameCompInfo components{};
        ExpandTHPComponents(components, bytes + 48);
        Check(components.numComponents == words[11], "component count differs");
        for (unsigned n = 0; n < 16; ++n)
            Check(components.frameComp[n] == words[12 + n], "component order/type changed");
        THPVideoInfo video{};
        ExpandTHPVideo(video, bytes + 68);
        Check(video.xSize == words[28] && video.ySize == words[29] && video.videoType == words[30], "video fields differ");
        THPAudioInfo audio{};
        ExpandTHPAudio(audio, bytes + 80);
        Check(audio.sndChannels == words[31] && audio.sndFrequency == words[32] &&
              audio.sndNumSamples == words[33] && audio.sndNumTracks == words[34], "audio fields differ");
        const auto* frameWord = bytes + 96;
        for (unsigned n = 0; n < 16; ++n, frameWord += sizeof(u32)) {
            Check(ReadTHPWord(frameWord) == words[35 + n], "four-byte frame word stride differs");
            Check(ReadTHPSignedWord(frameWord) == std::bit_cast<std::int32_t>(words[35 + n]), "signed frame size bits differ");
        }
        Check(Guard(input, offset, raw.size()), "record expansion modified the raw source");
    }
}

std::uint64_t Hash(const Bytes& bytes) {
    std::uint64_t value = 14695981039346656037ull;
    for (auto byte : bytes) value = (value ^ byte) * 1099511628211ull;
    return value;
}

void Video(const std::filesystem::path& folder) {
    auto raw = Read(folder / "video.bin");
    auto expected = Expected(folder / "video.txt");
    Check(expected.size() == 5, "video oracle shape differs");
    const auto width = expected[0], height = expected[1];
    const auto tiled = [](unsigned w, unsigned h) { return ((w + 7) / 8) * ((h + 3) / 4) * 32; };
    const auto ySize = tiled(width, height), cSize = tiled((width + 1) / 2, (height + 1) / 2);
    Bytes y(ySize + 2, 0xa5), u(cSize + 2, 0xa5), v(cSize + 2, 0xa5);
    Check(THPVideoDecode(raw.data(), y.data() + 1, u.data() + 1, v.data() + 1, nullptr) == 0, "actual pointer-interface video decode failed");
    Check(Guard(y, 1, ySize) && Guard(u, 1, cSize) && Guard(v, 1, cSize), "video output guard changed");
    if (std::filesystem::exists(folder / "video-y.bin")) {
        const auto expectedY = Read(folder / "video-y.bin");
        Check(expectedY.size() == ySize, "retail luma oracle size differs");
        for (std::size_t n = 0; n < ySize; ++n)
            Check(y[n + 1] == expectedY[n], "retail SDK quarter-IDCT store order differs");
        for (std::size_t n = 1; n <= cSize; ++n) {
            Check(u[n] == expected[3], "independent U DC oracle differs");
            Check(v[n] == expected[4], "independent V DC oracle differs");
        }
    } else if (expected[2] != UINT32_MAX) {
        for (std::size_t n = 1; n <= ySize; ++n) Check(y[n] == expected[2], "independent luma DC oracle differs");
        for (std::size_t n = 1; n <= cSize; ++n) {
            Check(u[n] == expected[3], "independent U DC oracle differs");
            Check(v[n] == expected[4], "independent V DC oracle differs");
        }
    }
    Bytes boundedY(ySize), boundedU(cSize), boundedV(cSize);
    Check(AuroraTHPVideoDecodeBounded(raw.data(), raw.size(), boundedY.data(), ySize,
              boundedU.data(), cSize, boundedV.data(), cSize, width, height) == 0, "bounded decoder rejected complete codec fixture");
    Check(std::equal(boundedY.begin(), boundedY.end(), y.begin() + 1) &&
          std::equal(boundedU.begin(), boundedU.end(), u.begin() + 1) &&
          std::equal(boundedV.begin(), boundedV.end(), v.begin() + 1), "pointer and bounded video interface differ");
    std::cout << folder.filename().string() << " Y=" << Hash(boundedY) << " U=" << Hash(boundedU) << " V=" << Hash(boundedV) << '\n';
}

void Audio(const std::filesystem::path& folder) {
    auto raw = Read(folder / "audio.bin");
    for (s32 layout = 0; layout < 2; ++layout) {
        auto expected = Read(folder / (layout == 0 ? "audio-interleaved.pcm" : "audio-planar.pcm"));
        const auto sampleCount = expected.size() / 4;
        std::vector<s16> result(expected.size() / 2 + 2, 12345);
        Check(THPAudioDecode(result.data() + 1, raw.data(), layout) == sampleCount, "actual pointer-interface audio sample count differs");
        Check(result.front() == 12345 && result.back() == 12345, "audio output guard changed");
        for (std::size_t n = 0; n < expected.size() / 2; ++n) {
            const auto bits = static_cast<std::uint16_t>(expected[n * 2] | (unsigned(expected[n * 2 + 1]) << 8));
            Check(result[n + 1] == std::bit_cast<std::int16_t>(bits), "independent DSP arithmetic/order oracle differs");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "data qualifier needs generated fixture directory");
        static_assert(sizeof(THPSimpleLong) == 4 && sizeof(THPSimpleUnsignedLong) == 4);
        // Only these three original source query sections are linked. The real
        // initialization/open/preload/audio callbacks are deliberately unlinked;
        // this bounded qualifier supplies no successful AI or game providers.
        THPVideoInfo sentinel{0x11223344, 0x55667788, 0x99aabbcc};
        Check(THPSimpleGetVideoInfo(&sentinel) == 0, "original unopened video query fabricated readiness");
        Check(sentinel.xSize == 0x11223344 && sentinel.ySize == 0x55667788 && sentinel.videoType == 0x99aabbcc, "original unopened video query wrote output");
        Check(THPSimpleGetTotalFrame() == 0 && THPSimpleCalcNeedMemory() == 0, "original unopened queries fabricated frames/work memory");
        Check(THPInit() != 0, "actual CPU codec initialization failed");
        for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
            if (!entry.is_directory()) continue;
            if (std::filesystem::exists(entry.path() / "metadata.bin")) Metadata(entry.path());
            if (std::filesystem::exists(entry.path() / "video.bin")) Video(entry.path());
            if (std::filesystem::exists(entry.path() / "audio.bin")) Audio(entry.path());
        }
        std::cout << checks << " original movie data/codec checks passed; positive Open/preload/AI/render readiness unqualified\n";
    } catch (const std::exception& error) {
        std::cerr << "FAILED after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
