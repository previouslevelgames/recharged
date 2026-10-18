#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Robot goalies (tweak "Robot goalies"): the goalies wear the Super Team's robot
// goalie skin from Super Mario Strikers. Charged's disc still has its texture
// (art/characters/superteamgoalie, unused), laid out for the GameCube Kritter.
// The map (assets/gameplay/robot_goalie.map, see its README) says, for each
// texel of Charged's goalie kit and eye, where on that texture its colour lies.
namespace mscharged::platform
{
// The robot texture's name, "superteamgoalie/superteamgoalie".
inline constexpr std::uint32_t kRobotGoalieTexture = 0x8C2BD57Cu;
// Charged's goalie kit layout ("mariogoalie/mariogoalie"), eye and shine mask.
inline constexpr std::uint32_t kGoalieKitLayout = 0x69A26240u;
inline constexpr std::uint32_t kGoalieEye = 0xFE561F0Eu;
inline constexpr std::uint32_t kGoalieShineMask = 0x2E89DEE7u;

// Whether `map` is a complete, consistent robot goalie map.
bool ValidRobotGoalieMap(std::span<const std::uint8_t> map);

// Rewrites, in place, every goalie texture of a loaded texture bundle (.rlt)
// as the robot's: each team's kit and away kit, the eye and the shine mask.
// `robot` is the bundle with the robot texture. Returns how many textures it
// rewrote; 0 when the bundle has none or the inputs do not fit (the bundle is
// then unchanged).
int MakeRobotGoalieTextures(std::span<const std::uint8_t> map, std::span<const std::uint8_t> robot,
    std::span<std::uint8_t> bundle);
} // namespace mscharged::platform
