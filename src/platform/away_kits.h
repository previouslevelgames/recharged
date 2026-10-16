#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Generated away kits for the tweak "Mirror match" (one captain on both
// teams). The game has away kits only for the captains it dresses in one
// against a captain of their colours; Mario, Luigi, Wario and Waluigi have
// none on the disc. Their away team gets one made from the player's own game
// files: the team colour of the captain's kit texture and of the sidekicks'
// and goalie's team textures is recoloured, everything else keeps its texels.
namespace mscharged::platform
{
struct AwayKit;

// The generated away kit of a captain (CharacterInfo name), or null.
const AwayKit* FindGeneratedAwayKit(const char* captain);
// The kit's team colour, 0xRRGGBB (HUD rings and overlays).
std::uint32_t AwayKitColour(const AwayKit& kit);
// The captain's kit texture (the diffuse texture of his model).
std::uint32_t AwayKitSourceTexture(const AwayKit& kit);
// The two teams whose version of a sidekick or goalie texture tells its
// team-coloured texels apart: those differ between teams.
const char* AwayKitReference(const AwayKit& kit, int index);

enum class AwayKitTexture
{
    Kit,  // the captain's kit texture: texels in the kit's colour
    Team, // a sidekick or goalie team texture: texels that differ from the references
};

// Recolours one Wii texture in place: its 32-byte header and CMPR levels as a
// bundle stores them (big-endian). Team textures compare with the same
// texture of the two reference teams (same size and format). False when the
// texture is not CMPR or the references do not match it.
bool RecolourAwayKit(const AwayKit& kit, AwayKitTexture kind, std::span<std::uint8_t> texture,
    std::span<const std::uint8_t> reference0 = {}, std::span<const std::uint8_t> reference1 = {});

// The texture `hash` of a texture bundle (.rlt), or empty.
std::span<const std::uint8_t> FindBundleTexture(std::span<const std::uint8_t> bundle, std::uint32_t hash);
}
