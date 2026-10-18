#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "platform/gx_texture.h"

// Super Team (tweak "Super Team"): a team's captain and sidekicks look like the
// robots of the Super Team from Super Mario Strikers (GameCube). Charged has no
// such model; it comes from the player's own GameCube disc
// (art/characters/SuperTeam), fitted onto each Charged character's skeleton so
// the character's own animations, effects and play stay as they are.
namespace mscharged::platform
{
// The GameCube files: the robot's model (SuperTeam.glg), skeleton (SuperTeam.shier) and textures (SuperTeam.glt).
struct SuperTeamSource
{
    std::span<const std::uint8_t> model, skeleton, textures;
};

// Character classes (eCharacterClass) that keep their own look: Bowser (his
// robot would be far slimmer than his body), Petey and Boo (rigs that fit no
// humanoid robot).
bool SuperTeamKeepsLook(int characterClass);

// The robot's body texture keeps its GameCube name ("superteam/superteam").
inline constexpr std::uint32_t kSuperTeamBody = 0x67C6069Au;

// The id of a character model's robot version: the character keeps its own
// model, so a character on both teams can be a robot on one side only.
std::uint32_t SuperTeamModelId(std::uint32_t characterModelId);

// The robot as a model of a Charged character (its .rlg and .shier as read from
// the disc): the robot's bones in the character's proportions, the character's
// material with the robot's textures, with the id SuperTeamModelId gives. Empty
// when an input does not fit.
gx::HostVector<std::uint8_t> MakeSuperTeamModel(const SuperTeamSource& source,
    std::span<const std::uint8_t> characterModel, std::span<const std::uint8_t> characterSkeleton);

// The textures those models use, as a Charged texture bundle (.rlt). Empty when
// the GameCube textures do not fit.
gx::HostVector<std::uint8_t> MakeSuperTeamTextures(const SuperTeamSource& source);

// Whether the GameCube files are the Super Team's model, skeleton and textures.
bool ValidSuperTeamSource(const SuperTeamSource& source);
} // namespace mscharged::platform
