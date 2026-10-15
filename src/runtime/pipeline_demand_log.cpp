#include "runtime/pipeline_demand_log.h"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mscharged::runtime {
namespace {
std::string Base64(const unsigned char* data, std::size_t size)
{
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t chunk = std::uint32_t(data[i]) << 16 | (i + 1 < size ? std::uint32_t(data[i + 1]) << 8 : 0) |
                                    (i + 2 < size ? data[i + 2] : 0);
        text += digits[chunk >> 18 & 63];
        text += digits[chunk >> 12 & 63];
        text += i + 1 < size ? digits[chunk >> 6 & 63] : '=';
        text += i + 2 < size ? digits[chunk & 63] : '=';
    }
    return text;
}

// A GX configuration is 2772 bytes, mostly zeros: about 300 characters.
std::string EncodedConfig(const void* config, std::uint32_t size)
{
    uLongf length = compressBound(size);
    std::vector<unsigned char> compressed(length);
    if (compress2(compressed.data(), &length, static_cast<const Bytef*>(config), size, Z_BEST_COMPRESSION) != Z_OK)
        return {};
    return Base64(compressed.data(), length);
}
} // namespace

std::string PipelineDemandMissLine(const AuroraPipelineDemandMiss& miss)
{
    // The pipeline_cache.db types; the catalogue ABI names them gx17 and clear5.
    const char* type = miss.shaderType == 1 ? "gx" : miss.shaderType == 0 ? "clear" : "other";
    char text[192];
    std::snprintf(text, sizeof text,
                  "Graphics: %s pipeline compiled while drawing in %.1f ms: pipeline=%s%u-%016llx frame=%u",
                  miss.newConfiguration ? "new" : "known", double(miss.compileNanoseconds) / 1e6, type,
                  unsigned(miss.configVersion), static_cast<unsigned long long>(miss.configKey), unsigned(miss.frame));
    std::string line = text;
    // A known configuration is already in a catalogue or this PC's cache.
    if (miss.newConfiguration && miss.config && miss.configSize) {
        const auto config = EncodedConfig(miss.config, miss.configSize);
        if (!config.empty()) line += " config=" + config;
    }
    return line;
}
} // namespace mscharged::runtime
