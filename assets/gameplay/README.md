# Gameplay data

## Robot goalie map

`robot_goalie.map` serves the gameplay tweak *Robot goalies*. Mario Strikers
Charged's disc still holds the robot goalie texture of the Super Team from
Super Mario Strikers (GameCube), unused, in
`art/characters/superteamgoalie/superteamgoalie.rlt`. It is laid out for the
GameCube game's goalie, whose model Charged replaced, so it does not fit
Charged's goalie as it is.

The map lists, for each texel of Charged's goalie kit (256x256, the layout all
teams' kits share) and of its eye texture (128x64), up to four points on that
robot texture. At match loading the game module takes the robot texture from
the player's disc, averages its colour at those points and writes the result
over the goalie textures (`src/platform/robot_goalie.cpp`); the goalie's shine
mask follows from the made kit. The map holds texture coordinates only: no
picture, model or other game data.

It was made once from both games' goalie models with
`tools/robot_goalie/make_map.py`: each Charged surface point is moved into the
GameCube goalie's body bone by bone (the two goalies share their skeleton's
bones but not their proportions) and takes the texture coordinates of the
nearest GameCube surface facing the same way. The eyeball is placed on the
robot's crosshair lens. Rebuilding it needs both games' discs, extracted:

```sh
python3 tools/robot_goalie/make_map.py --charged CHARGED/files \
    --gamecube STRIKERS/files --out assets/gameplay/robot_goalie.map
```

Format, little endian: `RGM1`, u32 version (1), u32 robot texture name hash
(`0x8C2BD57C`), u32 target count; per target: u32 texture name hash (the
layout: `0x69A26240` goalie kit, `0xFE561F0E` eye), u16 width, u16 height,
u32 sample count, one u8 per texel (1 to 4 samples, rows top to bottom), then
the samples as u16 s, u16 t (65535 = 1).
