#pragma once

// Sound levels: the volume of each group of the game's sounds, [audio] in
// recharged.ini, 0-100 % (issue #59). A group's level scales the levels the
// game itself sets (the music, sound effect and voice settings of its Options
// menu and the levels of its own mixes) and comes before the master volume.
// The game module receives them (charged_configure_sound_volumes) before
// original main starts; its hooks in the tweak series (patch 0020) sort every
// sound into a group by the game's mix category and sound bank.

#include <array>
#include <cstddef>
#include <string>

namespace mscharged
{
// Order of the volumes the game module receives (src/platform/game_tweaks.cpp).
// Append only.
enum class SoundGroup : unsigned
{
    Music = 0,
    Effects = 1,
    Voices = 2,
    Crowd = 3,
    Menus = 4,
    Cutscenes = 5,
    Movies = 6,
};

struct SoundGroupInfo
{
    SoundGroup group;
    const char* key;         // [audio] key
    const char* name;        // launcher label and log name
    const char* description; // launcher description
};

inline constexpr std::array<SoundGroupInfo, 7> kSoundGroups{{
    {SoundGroup::Music, "music_volume", "Music", "Menu and match music."},
    {SoundGroup::Effects, "effects_volume", "Sound effects",
     "Kicks, tackles, items, the electric fence, stadium and weather sounds."},
    {SoundGroup::Voices, "voice_volume", "Voices", "The characters' voice lines, also in the menus."},
    {SoundGroup::Crowd, "crowd_volume", "Crowd", "The stadium crowd's cheering and chants."},
    {SoundGroup::Menus, "menu_volume", "Menus & HUD",
     "Menu clicks and prompts, the studio logo's jingle and the sounds of the match's on-screen display."},
    {SoundGroup::Cutscenes, "cutscene_volume", "Cutscenes",
     "Captain introductions, goal celebrations, Mega Strikes and the scenes after a match, with their own music "
     "and voices."},
    {SoundGroup::Movies, "movie_volume", "Movies", "The intro movie, the stadium previews and the credits."},
}};

using SoundVolumes = std::array<int, kSoundGroups.size()>;

inline constexpr SoundVolumes DefaultSoundVolumes()
{
    SoundVolumes volumes{};
    for (auto& volume : volumes) volume = 100;
    return volumes;
}

// For the log: "original" with every group at 100 %, else the changed groups
// ("Music 20%, Movies 0%").
inline std::string DescribeSoundVolumes(const SoundVolumes& volumes)
{
    std::string text;
    for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
        if (volumes[n] != 100)
            text += (text.empty() ? "" : ", ") + std::string(kSoundGroups[n].name) + " "
                + std::to_string(volumes[n]) + "%";
    return text.empty() ? "original" : text;
}
}
