#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged::launcher
{
// A release's save banner: the 192 x 64 picture the Wii (and Dolphin's game
// list) shows beside the game's save. The game copies it from its own disc
// into the save (SaveLoad::IconLoadsComplete); the launcher reads it from the
// player's image in the same way, so Recharged ships no game artwork.
constexpr int kBannerWidth = 192;
constexpr int kBannerHeight = 64;

// The disc file with the banner of a Charged release, by disc ID:
// Art/fe/us_banner.tpl for the USA and Korea (the Korean game takes the
// game's default, USA, branch), eu_banner.tpl for Europe (both revisions) and
// jp_banner.tpl for Japan. Every disc has all three files, but only its own
// region's file holds its artwork. Empty for any other ID.
std::string ReleaseBannerFile(std::string_view game_id);

// The RGBA pixels (kBannerWidth x kBannerHeight, top row first) of a TPL with
// one RGB5A3 image of the banner's size. Throws std::runtime_error otherwise.
std::vector<std::uint8_t> DecodeBannerTpl(const std::uint8_t* data, std::size_t size);

// Reads and decodes the banner of a Charged image's own release. Throws
// std::runtime_error when the image has none or it cannot be read.
std::vector<std::uint8_t> ReadReleaseBanner(const std::filesystem::path& disc, const std::string& game_id);
}
