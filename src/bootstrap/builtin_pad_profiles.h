#pragma once

namespace mscharged
{
// Gamepad profiles that come with the game beside Default and Classic 1.1,
// such as those of top players (only with their consent), in the format of
// recharged.ini: [gamepad_profile1] to [gamepad_profile16], each with its
// name, its controller (xbox, nintendo or gamecube) and the keys of a saved
// profile. To add one, paste the player's section from their recharged.ini
// and number it after the others. Their names cannot be taken by a player's
// own profile; one that already has such a name gets a number added.
inline constexpr const char* kBuiltinPadProfilesIni = R"ini(
)ini";
} // namespace mscharged
