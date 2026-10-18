// Tweak "Robot goalies" inside the original game module: the host passes the
// map (assets/gameplay/robot_goalie.map) before original main, and the hooks of
// tweak patch 0025 pass each goalie texture bundle as it loads, with the disc's
// robot goalie bundle (src/platform/robot_goalie.cpp does the work).
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error The robot goalie hook belongs to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_tweaks.h"
#include "platform/robot_goalie.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>

namespace {
// The host owns the map for the whole run.
std::span<const std::uint8_t> sMap;
} // namespace

extern "C" {
// Host entry: the map, false (and no robot goalies) when it is not a valid one.
__attribute__((visibility("default"))) bool charged_configure_robot_goalie(const void* map, unsigned long size)
{
    const std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t*>(map), map ? size : 0);
    sMap = mscharged::platform::ValidRobotGoalieMap(bytes) ? bytes : std::span<const std::uint8_t>{};
    return !sMap.empty();
}
}

int mscharged_tweak_robot_goalie_textures(void* bundle, unsigned long size, const void* robot, unsigned long robotSize)
{
    if (sMap.empty() || !bundle || !robot) return 0;
    try
    {
        return mscharged::platform::MakeRobotGoalieTextures(sMap,
            {static_cast<const std::uint8_t*>(robot), robotSize}, {static_cast<std::uint8_t*>(bundle), size});
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Robot goalies: the goalie textures keep their look (%s)\n", error.what());
        return 0;
    }
}
