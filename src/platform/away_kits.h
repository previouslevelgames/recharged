#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "platform/gx_texture.h"

// Away kits made for the tweak "Mirror match" (one captain on both teams).
// The game has a whole away kit only for the captains it dresses in one
// against a captain of their colours. Mario, Luigi, Wario and Waluigi have
// none on the disc, and Peach's blue kit has no menu pictures or Mega Strike
// hand. Those are made from the player's own game files as they load: the
// team colour of the home versions is recoloured, everything else keeps its
// texels.
namespace mscharged::platform
{
struct AwayKit;

// The made away kit of a captain (CharacterInfo name), or null.
const AwayKit* FindGeneratedAwayKit(const char* captain);
// The kit's team colour, 0xRRGGBB (menus, HUD rings and overlays).
std::uint32_t AwayKitColour(const AwayKit& kit);
// The captain's kit texture (the diffuse texture of his model), 0 when the
// game has his field kit.
std::uint32_t AwayKitSourceTexture(const AwayKit& kit);
// The two teams whose version of a team texture or picture tells its
// team-coloured texels apart: those differ between teams.
const char* AwayKitReference(const AwayKit& kit, int index);
// Whether the game's photos of him (the post-match newspaper) recolour cleanly.
bool AwayKitPhotos(const AwayKit& kit);

enum class AwayKitTexture
{
    Kit,     // the captain's kit texture: texels in the kit's colour
    Team,    // a team texture or picture: texels that differ from the references
    Picture, // a painted picture of him: areas in the kit's colour (not single texels or thin lines)
};

// Recolours one Wii texture in place: its 32-byte header and levels as a
// bundle stores them (big-endian; CMPR, RGB5A3 or RGBA8). Team textures
// compare with the same texture of the two reference teams (same size and
// format). False when the format is another or the references do not match.
bool RecolourAwayKit(const AwayKit& kit, AwayKitTexture kind, std::span<std::uint8_t> texture,
    std::span<const std::uint8_t> reference0 = {}, std::span<const std::uint8_t> reference1 = {});

// Team textures the disc leaves unfinished (issue #176). Shy Guy's blue Peach
// kit (shyguy_peach_alt) is red and yellow with Petey's emblem, a leftover the
// game never shows: no captain shares Peach's colour. Blue Peach and Mirror
// match show it. It is made from his pink Peach kit the way the game's own
// blue kits of her other sidekicks are: her pink turns blue, her light blue
// yellow.
struct UnfinishedKit;

// The unfinished kit a loaded bundle holds with the disc's bytes, or null.
const UnfinishedKit* FindUnfinishedKit(std::span<const std::uint8_t> bundle);
// The unfinished kit of texture `texture` (its name hash), whatever its bytes, or null.
const UnfinishedKit* UnfinishedKitNamed(std::uint32_t texture);
// The bundles it is made from: 0 the one holding the texture it is made from,
// 1 and 2 the reference teams' bundles of that texture.
const char* UnfinishedKitFile(const UnfinishedKit& kit, int index);
// Makes it in `bundle` from those bundles. False, and `bundle` unchanged, when
// it cannot be made: a bundle lacks its texture, or they differ in size or
// format.
bool FinishUnfinishedKit(const UnfinishedKit& kit, std::span<std::uint8_t> bundle, std::span<const std::uint8_t> source,
    std::span<const std::uint8_t> reference0, std::span<const std::uint8_t> reference1);
}
