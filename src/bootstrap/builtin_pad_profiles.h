#pragma once

namespace mscharged
{
// Gamepad profiles that come with the game beside Default and Classic 1.1,
// such as those of top players (only with their consent), in the format of
// recharged.ini: [gamepad_profile1] to [gamepad_profile16], each with its
// name, its controller (xbox, nintendo or gamecube) and the keys of a saved
// profile. To add one, paste the player's section from their recharged.ini
// and number it after the others. Write every action: one left out takes
// the Default input, which may change between versions. Their names cannot be
// taken by a player's own profile; one that already has such a name gets a
// number added.
inline constexpr const char* kBuiltinPadProfilesIni = R"ini(
[gamepad_profile1]
name = iceVirtue
controller = xbox
a = a
b = righttrigger
one = start
two = back
plus = start
minus = back
home = guide
up = dpup
down = dpdown
left = dpleft
right = b
c = y
z = lefttrigger
shake_remote = x
shake_nunchuk = rightshoulder
recenter_pointer = rightstick
sticks = split
)ini";
} // namespace mscharged
