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

[gamepad_profile2]
name = ImSpiker
controller = xbox
a = a
b = lefttrigger
one = start
two = none
plus = rightshoulder
minus = leftshoulder
home = guide
up = dpup | rightstick_up
down = dpdown | rightstick_down
left = dpleft | rightstick_left
right = dpright | rightstick_right
c = rightshoulder
z = righttrigger
shake_remote = y
shake_nunchuk = x | b
recenter_pointer = rightstick
sticks = shared
)ini";
} // namespace mscharged
