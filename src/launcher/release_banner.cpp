#include "launcher/release_banner.h"

#include "platform/disc.h"

#include <stdexcept>

namespace mscharged::launcher
{
namespace
{
constexpr std::uint32_t kTplMagic = 0x0020AF30;
constexpr std::uint32_t kFormatRgb5a3 = 5;
constexpr std::size_t kBannerBytes = std::size_t(kBannerWidth) * kBannerHeight * 2;
// The banner files are 24,640 bytes; anything far larger is not one.
constexpr std::size_t kMaxBannerFile = 256 * 1024;

std::uint32_t Be32(const std::uint8_t* data, std::size_t size, std::size_t offset)
{
    if (offset > size || size - offset < 4) throw std::runtime_error("The banner file is truncated.");
    return std::uint32_t(data[offset]) << 24 | std::uint32_t(data[offset + 1]) << 16
        | std::uint32_t(data[offset + 2]) << 8 | data[offset + 3];
}

std::uint16_t Be16(const std::uint8_t* data, std::size_t size, std::size_t offset)
{
    if (offset > size || size - offset < 2) throw std::runtime_error("The banner file is truncated.");
    return std::uint16_t(data[offset] << 8 | data[offset + 1]);
}
}

std::string ReleaseBannerFile(std::string_view game_id)
{
    if (game_id.size() != 6 || game_id.substr(0, 3) != "R4Q" || game_id.substr(4) != "01") return {};
    switch (game_id[3])
    {
    case 'E':
    case 'K': return "Art/fe/us_banner.tpl";
    case 'P': return "Art/fe/eu_banner.tpl";
    case 'J': return "Art/fe/jp_banner.tpl";
    default: return {};
    }
}

std::vector<std::uint8_t> DecodeBannerTpl(const std::uint8_t* data, std::size_t size)
{
    if (Be32(data, size, 0) != kTplMagic) throw std::runtime_error("The banner file is not a TPL texture.");
    if (Be32(data, size, 4) < 1) throw std::runtime_error("The banner file has no image.");
    const std::size_t table = Be32(data, size, 8);
    const std::size_t image = Be32(data, size, table);
    const std::uint16_t height = Be16(data, size, image), width = Be16(data, size, image + 2);
    if (width != kBannerWidth || height != kBannerHeight || Be32(data, size, image + 4) != kFormatRgb5a3)
        throw std::runtime_error("The banner file does not hold a 192 x 64 RGB5A3 image.");
    const std::size_t pixels = Be32(data, size, image + 8);
    if (pixels > size || size - pixels < kBannerBytes) throw std::runtime_error("The banner file is truncated.");

    // RGB5A3 in 4 x 4 texel tiles, big-endian: with the top bit set RGB555,
    // else 3-bit alpha and RGB444.
    std::vector<std::uint8_t> rgba(std::size_t(kBannerWidth) * kBannerHeight * 4);
    const std::uint8_t* texel = data + pixels;
    for (int tile_y = 0; tile_y < kBannerHeight; tile_y += 4)
        for (int tile_x = 0; tile_x < kBannerWidth; tile_x += 4)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x, texel += 2)
                {
                    const unsigned value = unsigned(texel[0]) << 8 | texel[1];
                    std::uint8_t* out = &rgba[(std::size_t(tile_y + y) * kBannerWidth + std::size_t(tile_x + x)) * 4];
                    if (value & 0x8000)
                    {
                        const auto five = [](unsigned v) { return std::uint8_t(v << 3 | v >> 2); };
                        out[0] = five(value >> 10 & 31);
                        out[1] = five(value >> 5 & 31);
                        out[2] = five(value & 31);
                        out[3] = 255;
                    }
                    else
                    {
                        const unsigned alpha = value >> 12 & 7;
                        out[0] = std::uint8_t((value >> 8 & 15) * 17);
                        out[1] = std::uint8_t((value >> 4 & 15) * 17);
                        out[2] = std::uint8_t((value & 15) * 17);
                        out[3] = std::uint8_t(alpha << 5 | alpha << 2 | alpha >> 1);
                    }
                }
    return rgba;
}

std::vector<std::uint8_t> ReadReleaseBanner(const std::filesystem::path& disc, const std::string& game_id)
{
    const std::string file = ReleaseBannerFile(game_id);
    if (file.empty()) throw std::runtime_error("Recharged knows no banner for " + game_id + ".");
    const auto tpl = ReadDiscFile(disc, file, kMaxBannerFile);
    return DecodeBannerTpl(tpl.data(), tpl.size());
}
}
