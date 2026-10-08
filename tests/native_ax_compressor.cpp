// Owned AX compressor conformance: the native decision and per-sample ramp
// arithmetic against firmware 057B..0609 executed on controlled inputs by
// tools/native_ax_compressor_oracle.py (original __AXCompressorTable source).
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
        if (at + sizeof(T) > bytes.size()) throw std::runtime_error("truncated compressor oracle");
        T value;
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    NativeAXChannel96 Channel()
    {
        NativeAXChannel96 channel{};
        for (auto& sample : channel) sample = Get<std::int32_t>();
        return channel;
    }
};
} // namespace

int main(int argc, char** argv)
{
    try {
        Check(argc == 2, "usage: native_ax_compressor_tests ORACLE");
        std::ifstream in(argv[1], std::ios::binary);
        Reader r{{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}};
        Check(r.bytes.size() > 8 && std::memcmp(r.bytes.data(), "AXCMP001", 8) == 0, "compressor oracle magic differs");
        r.at = 8;
        std::array<std::uint16_t, 2016> table{};
        for (auto& gain : table) gain = r.Get<std::uint16_t>();
        const auto count = r.Get<std::uint32_t>();
        unsigned attacks = 0, releases = 0, untouched = 0;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto counter = r.Get<std::uint16_t>();
            const auto threshold = r.Get<std::uint16_t>();
            const auto release = r.Get<std::uint16_t>();
            const auto left = r.Channel(), right = r.Channel();
            const auto after_counter = r.Get<std::uint16_t>();
            const auto offset = r.Get<std::uint32_t>();
            const auto expected_left = r.Channel(), expected_right = r.Channel();
            const auto decision = NativeAXCompressorStep(counter, threshold, release, left, right);
            Check(decision.apply == (offset != 0xffffffffu), "compressor table DMA decision differs from firmware");
            Check(decision.counter == after_counter, "compressor release counter differs from firmware 0CE4");
            auto out_left = left, out_right = right;
            if (decision.apply) {
                Check(decision.offset == offset, "compressor ramp offset differs from firmware DMA");
                Check(offset % 2 == 0 && offset / 2 + 96 <= table.size(), "compressor ramp outside the original table");
                for (unsigned i = 0; i < 96; ++i) {
                    out_left[i] = NativeAXCompressSample(left[i], table[offset / 2 + i]);
                    out_right[i] = NativeAXCompressSample(right[i], table[offset / 2 + i]);
                }
                (offset < 0x840 ? attacks : releases) += 1;
            } else {
                ++untouched;
            }
            for (unsigned i = 0; i < 96; ++i) {
                Check(out_left[i] == expected_left[i], "compressed left bus differs from firmware");
                Check(out_right[i] == expected_right[i], "compressed right bus differs from firmware");
            }
        }
        Check(r.at == r.bytes.size(), "compressor oracle has trailing data");
        Check(attacks >= 20 && releases >= 10 && untouched >= 3, "compressor oracle lacks attack/release/untouched coverage");
        std::cout << "Native AX compressor conformance PASS: " << checks << " checks, " << count << " firmware cases ("
                  << attacks << " attack, " << releases << " release, " << untouched << " untouched)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
