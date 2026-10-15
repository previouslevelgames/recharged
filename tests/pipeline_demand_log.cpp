// The session log line for a draw that waited for a pipeline compile, and the
// round trip of a new configuration's bytes (base64 of zlib) that
// tools/pipeline_catalog.py --import reads back.
#include "runtime/pipeline_demand_log.h"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "pipeline_demand_log: %s\n", message);
        std::exit(1);
    }
}

std::vector<unsigned char> Base64Decode(const std::string& text)
{
    static const std::string digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Check(text.size() % 4 == 0, "base64 length is not a multiple of 4");
    std::vector<unsigned char> bytes;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::uint32_t chunk = 0;
        int padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                Check(i + 4 == text.size() && j >= 2, "misplaced base64 padding");
                ++padding;
                chunk <<= 6;
                continue;
            }
            Check(padding == 0, "base64 digit after padding");
            const auto value = digits.find(c);
            Check(value != std::string::npos, "invalid base64 digit");
            chunk = chunk << 6 | std::uint32_t(value);
        }
        bytes.push_back(chunk >> 16 & 0xFF);
        if (padding < 2) bytes.push_back(chunk >> 8 & 0xFF);
        if (padding < 1) bytes.push_back(chunk & 0xFF);
    }
    return bytes;
}

// The configuration bytes of a line, or nothing without a config field.
std::vector<unsigned char> LineConfig(const std::string& line, std::size_t size, char* padding)
{
    const auto at = line.find(" config=");
    if (at == std::string::npos) return {};
    const auto token = line.substr(at + 8);
    *padding = token.size() >= 2 && token[token.size() - 2] == '=' ? 2 : !token.empty() && token.back() == '=' ? 1 : 0;
    const auto compressed = Base64Decode(token);
    std::vector<unsigned char> config(size + 1);
    uLongf length = config.size();
    Check(uncompress(config.data(), &length, compressed.data(), uLong(compressed.size())) == Z_OK,
          "config is not a zlib stream");
    config.resize(length);
    return config;
}

AuroraPipelineDemandMiss Miss(const std::vector<unsigned char>& config, std::uint32_t type, std::uint32_t version,
                              std::uint64_t key, std::uint32_t frame, std::uint64_t wait, bool learned)
{
    AuroraPipelineDemandMiss miss{};
    miss.shaderType = type;
    miss.configVersion = version;
    miss.configKey = key;
    miss.config = config.data();
    miss.configSize = std::uint32_t(config.size());
    miss.frame = frame;
    miss.compileNanoseconds = wait;
    miss.newConfiguration = learned;
    return miss;
}
} // namespace

int main()
{
    using mscharged::runtime::PipelineDemandMissLine;
    char padding = 0;

    // A GX v17 configuration: mostly zeros, as the real ones are.
    std::vector<unsigned char> gx(2772);
    gx[0] = 17;
    for (std::size_t i = 8; i < gx.size(); i += 97) gx[i] = static_cast<unsigned char>(i * 7 + 1);
    auto line = PipelineDemandMissLine(Miss(gx, 1, 17, 0x1f2e3d4c5b6a7980ull, 18234, 45'230'000, true));
    const std::string gxPrefix =
        "Graphics: new pipeline compiled while drawing in 45.2 ms: pipeline=gx17-1f2e3d4c5b6a7980 frame=18234 config=";
    Check(line.compare(0, gxPrefix.size(), gxPrefix) == 0, "new GX line prefix");
    Check(LineConfig(line, gx.size(), &padding) == gx, "GX config does not round-trip");
    Check(line.size() < 600, "GX line is not compressed");
    Check(line.find('\n') == std::string::npos && line.find(' ', gxPrefix.size()) == std::string::npos,
          "line breaks or spaces inside the config");

    // A known configuration: the line has no config.
    line = PipelineDemandMissLine(Miss(gx, 1, 17, 0x00000000000000abull, 7, 3'100'000, false));
    Check(line == "Graphics: known pipeline compiled while drawing in 3.1 ms: pipeline=gx17-00000000000000ab frame=7",
          "known GX line");

    // A clear v5 configuration, with a full 64-bit key and frame.
    const std::vector<unsigned char> clear{5, 0, 0, 0, 1, 0, 1, 0};
    line = PipelineDemandMissLine(Miss(clear, 0, 5, 0xffffffffffffffffull, 0xffffffffu, 240'000, true));
    const std::string clearPrefix =
        "Graphics: new pipeline compiled while drawing in 0.2 ms: pipeline=clear5-ffffffffffffffff frame=4294967295 config=";
    Check(line.compare(0, clearPrefix.size(), clearPrefix) == 0, "new clear line prefix");
    Check(LineConfig(line, clear.size(), &padding) == clear, "clear config does not round-trip");

    // Every base64 padding case round-trips.
    std::set<char> paddings;
    std::uint32_t seed = 1;
    for (int n = 0; n < 64 && paddings.size() < 3; ++n) {
        std::vector<unsigned char> config(2772);
        for (auto& byte : config) {
            seed = seed * 1103515245u + 12345u;
            byte = (seed >> 16) % 5 == 0 ? static_cast<unsigned char>(seed >> 24) : 0;
        }
        line = PipelineDemandMissLine(Miss(config, 1, 17, n, n, 1'000'000, true));
        Check(LineConfig(line, config.size(), &padding) == config, "random config does not round-trip");
        paddings.insert(padding);
    }
    Check(paddings.size() == 3, "not every base64 padding case was produced");

    std::printf("Pipeline demand log passed (%u checks).\n", checks);
    return 0;
}
