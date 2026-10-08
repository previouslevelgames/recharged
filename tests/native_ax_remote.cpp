// Owned AX remote speaker conformance: native remote voice processing, 24-bit
// accumulation and remote output against firmware 02CC..0311, 03A2..0446 and
// 067C..06A9 executed on controlled inputs by tools/native_ax_remote_oracle.py.
// The coefficient bank is the oracle's explicitly synthetic conformance bank.
#include "platform/ax_active_voice.h"
#include "platform/ax_frame_commands.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace mscharged::platform;

namespace {
unsigned checks = 0;
void Check(bool value, const char* text)
{
    ++checks;
    if (!value) throw std::runtime_error(text);
}
struct Reader {
    std::vector<unsigned char> bytes;
    std::size_t at = 0;
    template <class T> T Get()
    {
        if (at + sizeof(T) > bytes.size()) throw std::runtime_error("truncated remote oracle");
        T value;
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
};
using Accumulators = std::array<std::array<std::int32_t, 18>, 8>;
Accumulators ReadAccumulators(Reader& r)
{
    Accumulators result{};
    for (auto& channel : result)
        for (auto& value : channel) value = r.Get<std::int32_t>();
    return result;
}
// PB bytes 0xD6..0x127 are DRAM words 033B..0363 in big-endian order.
constexpr std::size_t RemoteBytes = 0xd6, RemoteWords = 41;
} // namespace

int main(int argc, char** argv)
{
    try {
        Check(argc == 2, "usage: native_ax_remote_tests ORACLE");
        std::ifstream in(argv[1], std::ios::binary);
        Reader r{{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}};
        Check(r.bytes.size() > 8 && std::memcmp(r.bytes.data(), "AXRMT001", 8) == 0, "remote oracle magic differs");
        r.at = 8;
        NativeAXRemoteBank bank{};
        for (auto& word : bank) word = r.Get<std::int16_t>();
        const auto count = r.Get<std::uint32_t>();
        unsigned voices = 0, skipped = 0, filtered = 0, ramps = 0, wrapped = 0;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto voice_count = r.Get<std::uint32_t>();
            auto accumulators = ReadAccumulators(r);
            for (std::uint32_t v = 0; v < voice_count; ++v, ++voices) {
                std::array<std::int16_t, 96> pcm{};
                for (auto& sample : pcm) sample = r.Get<std::int16_t>();
                std::array<std::uint16_t, RemoteWords> before{}, expected{};
                for (auto& word : before) word = r.Get<std::uint16_t>();
                for (auto& word : expected) word = r.Get<std::uint16_t>();
                std::array<unsigned char, 320> pb{};
                for (std::size_t i = 0; i < RemoteWords; ++i) {
                    pb[RemoteBytes + i * 2] = static_cast<unsigned char>(before[i] >> 8);
                    pb[RemoteBytes + i * 2 + 1] = static_cast<unsigned char>(before[i]);
                }
                const auto prior = pb;
                // 03A2..03A5: TST of the remote flag skips the whole branch.
                if (before[0]) {
                    const auto remote = NativeAXProcessRemoteVoice(pcm, pb.data(), bank);
                    for (unsigned c = 0; c < 8; ++c) {
                        const bool selected = ((before[1] >> (c * 2)) & 3) != 0;
                        Check(selected == ((remote.mixed >> c) & 1), "remote handler selection differs from 0DB3 table");
                        if (!selected) continue;
                        for (unsigned i = 0; i < 18; ++i) {
                            const auto sum = std::int64_t(accumulators[c][i]) + remote.channels[c][i];
                            accumulators[c][i] = NativeAXMixAccumulate(accumulators[c][i], remote.channels[c][i]);
                            wrapped += sum != accumulators[c][i];
                        }
                        ramps += ((before[1] >> (c * 2)) & 3) >= 2;
                    }
                    filtered += before[31] != 0;
                } else {
                    ++skipped;
                }
                for (std::size_t i = 0; i < RemoteWords; ++i) {
                    const auto word = std::uint16_t((pb[RemoteBytes + i * 2] << 8) | pb[RemoteBytes + i * 2 + 1]);
                    Check(word == expected[i], i < 2 ? "remote flag/control changed" :
                          i < 18 ? "remote ramp volume store differs from firmware" :
                          i < 26 ? "remote depop store differs from firmware" :
                          i < 31 ? "remote resampler history/fraction differs from firmware" :
                          "remote IIR state differs from firmware");
                }
                for (std::size_t i = 0; i < pb.size(); ++i)
                    if (i < RemoteBytes || i >= RemoteBytes + RemoteWords * 2)
                        Check(pb[i] == prior[i], "remote processing touched a PB byte outside its fields");
            }
            const auto final = ReadAccumulators(r);
            for (unsigned c = 0; c < 8; ++c)
                for (unsigned i = 0; i < 18; ++i)
                    Check(accumulators[c][i] == final[c][i], "remote accumulator differs from firmware 0CB3/0CD0");
            for (unsigned speaker = 0; speaker < 4; ++speaker) {
                const auto output = NativeAXRemoteOutput(final[speaker * 2]);
                for (unsigned i = 0; i < 18; ++i)
                    Check(output[i] == r.Get<std::int16_t>(), "remote speaker PCM differs from firmware 067C");
            }
        }
        Check(r.at == r.bytes.size(), "remote oracle has trailing data");
        Check(voices >= 90 && skipped >= 2 && filtered >= 10 && ramps >= 40 && wrapped > 0,
              "remote oracle lacks skip/filter/ramp/wrap coverage");
        std::cout << "Native AX remote conformance PASS: " << checks << " checks, " << count << " firmware cases, "
                  << voices << " voices (" << skipped << " remote-off, " << filtered << " filtered, " << ramps
                  << " ramp channels, " << wrapped << " 24-bit wraps)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
