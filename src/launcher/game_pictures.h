#pragma once

// The game's own pictures of the captains and sidekicks, read from the
// player's disc for the launcher's match stats: the HUD's captain icons
// (fe/captain_icons/captain_icons_<captain> in Art/fe/captainiconsui.res,
// 128x128) and the sidekick pictures of Choose Sidekicks in each team's kit
// (fe/sidekick_images/attributes_<sidekick>_<captain>[_alt] in
// Art/fe/sidekicksui.res, 128x256). Recharged ships none of them.
//
// A front-end bundle (the game's BundleFile) is big-endian: a header (sector
// size, file count, the directory's and the data's offsets in sectors), then
// per file its name's hash (nlStringHash), first sector and length. A file
// here is a texture: levels, format (1 RGB5A3, 2 CMPR, 3 RGBA8), width and
// height at 14 and 16, the levels from byte 32.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged::launcher
{
inline constexpr const char* kCaptainIconBundle = "Art/fe/captainiconsui.res";
inline constexpr const char* kSidekickPictureBundle = "Art/fe/sidekicksui.res";

// ByteView of a bundle or a texture, borrowed.
struct ByteView
{
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    bool empty() const { return size == 0; }
};

struct Picture
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba; // width * height texels, top row first
};

// The game's hash of a name (nlStringHash).
std::uint32_t GameNameHash(std::string_view name);

// A bundle's file by its name; empty when the bundle has none of that name or
// is damaged.
ByteView BundleEntry(ByteView bundle, std::string_view name);

// A texture's first level as RGBA; false for another format or a damaged
// texture.
bool DecodePicture(ByteView texture, Picture& out);

// The game's names of a captain (eTeamID) and a sidekick (eSidekickID), null
// for another number.
const char* CaptainName(int team);
const char* SidekickName(int sidekick);

// A captain's HUD icon; a sidekick's picture in a captain's kit (`alt`: the
// away kit of a captain who has one). Empty for unknown numbers.
std::string CaptainIconPath(int team);
std::string SidekickPicturePath(int sidekick, int team, bool alt = false);
} // namespace mscharged::launcher
