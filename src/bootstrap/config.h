#pragma once

#include "bootstrap/frame_rate.h"
#include "bootstrap/game_tweaks.h"
#include "bootstrap/gamepad_actions.h"
#include "bootstrap/key_actions.h"
#include "bootstrap/sound_groups.h"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <set>
#include <vector>

namespace mscharged
{
struct Settings
{
    // game.disc: the image the game starts with (the launcher's pinned image).
    // A relative path counts from the settings file.
    std::string disc;
    // game.folder: the game folder, whose Mario Strikers Charged images the
    // launcher lists (`game` beside the settings file by default; relative
    // paths count from the settings file, like the disc), and
    // game.folder_subfolders: whether it also looks in its subfolders.
    std::string folder = "game";
    bool folder_subfolders = true;
    // game.save_folder: where the game keeps its Wii storage (saves). Empty
    // uses the default folder next to the executable; a relative path
    // counts from the settings file, like the disc.
    std::string save_folder;
    // game.gamecube_disc: a Super Mario Strikers image (GameCube), for the
    // Super Team tweak: its robot model and textures come from that disc. A
    // relative path counts from the settings file, like the disc.
    std::string gamecube_disc;
    std::string language = "auto";
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    bool vsync = true;
    // display.frame_rate: original, display, unlimited or 30-1000 frames per
    // second (bootstrap/frame_rate.h). Above the original the host paces and
    // presents every frame; the game keeps its own speed.
    std::string frame_rate = "original";
    std::string backend = "auto";
    // display.gpu: auto (the fastest graphics card) or a card's PCI ID, such as 10de:1f99.
    std::string gpu = "auto";
    std::string aspect = "16:9";
    // display.picture: soft (the TV signal, as on a Wii), clean (one smooth
    // pass to the window, no TV flicker filter) or sharp (square pixels).
    std::string picture = "sharp";
    // display.performance_mode: lighter matches. Turns off multisampling, and
    // the game module skips purely decorative work (crowd, world NPCs, ambient
    // effects, bloom, heat distortion, depth of field, stadium shadow volumes);
    // the 3D render size stays as chosen.
    bool performance_mode = false;
    // display.antialiasing: off, fxaa, 4x (multisampling of the game's frame),
    // 4x_fxaa or 4x_ssaa (supersampling); see kAntialiasingChoices.
    std::string antialiasing = "4x";
    // display.resolution: the game's 3D render size. window renders one pixel
    // per window pixel of the picture; native the Wii's 640x448; 2x-4x fixed
    // multiples of it. Viewport and projection stay the game's own.
    std::string resolution = "window";
    int master_volume = 100;
    // [audio] <group>_volume: each group of the game's sounds (kSoundGroups
    // order), 0-100 % of the levels the game sets itself.
    SoundVolumes sound_volumes = DefaultSoundVolumes();
    bool mute = false;
    // controls.player1-4: keyboard, remote1-remote4 (Wii Remote in that
    // DolphinBar slot), remote-<Bluetooth address> (a Wii Remote paired over
    // Bluetooth, e.g. remote-9ce635c69076), gamepad1-gamepad4 or off.
    // Players fill in order; each device plays once.
    std::array<std::string, 4> players{"keyboard", "off", "off", "off"};
    // [keyboard]: keys of each keyboard & mouse action ("Return | Space").
    std::array<std::string, KeyActionCount> keys = DefaultKeys();
    static std::array<std::string, KeyActionCount> DefaultKeys()
    {
        std::array<std::string, KeyActionCount> keys;
        for (std::size_t n = 0; n < KeyActionCount; ++n) keys[n] = kKeyActions[n].defaults;
        return keys;
    }
    // [hotkeys]: keys of the game window, whoever plays, one SDL key name each
    // (no modifier; no [keyboard] action takes them): hotkeys.screenshot saves
    // a screenshot, hotkeys.hud shows or hides the match HUD. hotkeys.show_hud
    // = false starts every match with the HUD hidden (No HUD, issue #58;
    // formerly gameplay.no_hud = true).
    std::string screenshot_key = "P";
    std::string hud_key = "H";
    bool show_hud = true;
    // Gamepad profiles ([gamepad_profile1], [gamepad_profile2], ...): a name
    // and the inputs of each action ("a | start"); swap_sticks puts the
    // Nunchuk stick on the right stick and the pointer on the left. The
    // built-in "Default" profile is not among them.
    using PadInputs = std::array<std::string, GamepadActionCount>;
    struct PadProfile
    {
        std::string name = "Default";
        PadInputs inputs = DefaultPadInputs();
        bool swap_sticks = false;
        // The controllers it is made for: xbox (Xbox and PlayStation, whose
        // buttons sit in the same places), nintendo or gamecube.
        std::string controller = "xbox";
        // inner_deadzone, outer_deadzone (both sticks) and trigger_press, in
        // percent of travel (see gamepad_actions.h).
        int inner_deadzone = kDefaultInnerDeadzone;
        int outer_deadzone = kDefaultOuterDeadzone;
        int trigger_press = kDefaultTriggerPress;
        // gyro_pointer: a controller with a gyro points by turning it, at
        // gyro_speed percent (issue #14); recenter_pointer centres it.
        bool gyro_pointer = false;
        int gyro_speed = kDefaultGyroSpeed;
        // Spelled out: charged_host is C++17.
        bool operator==(const PadProfile& other) const
        {
            return name == other.name && inputs == other.inputs && swap_sticks == other.swap_sticks
                && controller == other.controller && inner_deadzone == other.inner_deadzone
                && outer_deadzone == other.outer_deadzone && trigger_press == other.trigger_press
                && gyro_pointer == other.gyro_pointer && gyro_speed == other.gyro_speed;
        }
        bool operator!=(const PadProfile& other) const { return !(*this == other); }
    };
    std::vector<PadProfile> pad_profiles;
    // controls.gamepad1_profile-gamepad4_profile: the profile each gamepad
    // plays with; "Default" or a missing profile is the built-in one, which
    // follows the layout of the connected controller's family (Xbox,
    // PlayStation, Nintendo, GameCube).
    std::array<std::string, kGamepadSlots> pad_profile_names{"Default", "Default", "Default", "Default"};
    static PadInputs DefaultPadInputs()
    {
        PadInputs inputs;
        for (std::size_t n = 0; n < GamepadActionCount; ++n) inputs[n] = kGamepadActions[n].defaults;
        return inputs;
    }
    // The profile gamepad `slot` (0-3) plays with: the index of its named
    // profile, or -1 for the built-in Default.
    int PadProfileIndex(std::size_t slot) const;
    const PadProfile& PadProfileOf(std::size_t slot) const;
    static const PadProfile& DefaultPadProfile();
    // controls.rumble: the Wii's rumble setting (Wii Remotes and gamepads);
    // controls.rumble_strength (0-100 %) scales gamepad motors only, as a
    // Wii Remote's motor is either on or off.
    bool rumble = true;
    int rumble_strength = 100;
    // controls.ir_sensitivity: the Wii's sensor bar sensitivity (1-5), the
    // camera settings a Wii Remote gets.
    int ir_sensitivity = 3;
    // controls.speaker_volume: Wii Remote speaker volume (0-100 %, 70 = the
    // Wii's default); 0 sends no sound to the Remotes.
    int speaker_volume = 70;
    // The mouse also points for player 1 on a Wii Remote or gamepad playing alone.
    bool mouse_pointer = true;
    // controls.bluetooth_search: on Windows the launcher's Controls page
    // connects Wii Remotes over Bluetooth when 1+2 or SYNC is pressed.
    bool bluetooth_search = true;
    std::string sensor_bar = "bottom"; // controls.sensor_bar: bottom | top (sensor bar / DolphinBar position)
    // Host presentation and diagnostics (never game behaviour).
    bool fps_overlay = false;         // display.fps_overlay: FPS and system usage over the game picture
    // display.fps_overlay_corner: top-left | top-right | bottom-left | bottom-right
    std::string fps_overlay_corner = "top-right";
    int monitor = 0;                  // display.monitor: 0 = default display, N = Nth connected display
    bool graphics_validation = false; // advanced.graphics_validation: backend graphics debug layers (off: fastest)
    std::string log_level = "info";   // advanced.log_level: error | warning | info | debug
    bool verbose_console = false;     // advanced.verbose_console: development traces and game debug output
    bool frame_logging = false;       // advanced.frame_logging: CSV timings and summary in logs/ beside the executable
    // advanced.log_window (Windows and macOS): a separate window that follows the log while the program runs.
    bool log_window = false;
    std::string ui_scale = "auto";    // launcher.ui_scale: auto or a percentage (75 to 200)
    // launcher.check_updates: look for a newer build each time the launcher starts.
    bool check_updates = true;
    // launcher.reopen_after_game: open the launcher again when a game started
    // from it closes normally.
    bool reopen_after_game = false;
    // launcher.update_channel: release (official releases), test (also test
    // builds of pull requests) or auto (where this build comes from).
    std::string update_channel = "auto";
    // launcher.test_build: the pull request whose test builds the test channel
    // follows, chosen on the Play page; 0 for the running test build's own.
    int test_build = 0;
    // discord.rich_presence: show what is being played as the Discord status
    // (issue #71), when the Discord app runs on this computer.
    bool discord_status = true;
    // discord.application_id: the Discord application to show it under
    // instead of the build's own (testing; the ID is public, not a secret).
    // Read only: saving never writes it.
    std::string discord_application_id;
    // [mods] HD texture packs (Dolphin's format): mods.texture_packs uses the
    // packs (folders and .zip files) in mods.texture_folder (`texture_packs`
    // beside the settings file by default; relative paths count from the
    // settings file, like the disc). mods.texture_pack_order lists pack names
    // first to last ("A | B"; the first one wins where two replace the same
    // texture; packs not listed follow by name), mods.texture_packs_off the
    // packs switched off. mods.dump_textures saves the game's textures to
    // data/texture_dumps for pack makers.
    bool texture_packs = true;
    std::string texture_folder = "texture_packs";
    std::string texture_pack_order;
    std::string texture_packs_off;
    bool dump_textures = false;
    // [gameplay]: game tweaks (kGameTweaks order), off by default except the
    // recommended ones.
    std::array<bool, kGameTweaks.size()> tweaks = DefaultGameTweaks();
    // gameplay.super_team_side: the team the Super Team tweak dresses as
    // robots: away, home or both.
    std::string super_team_side = "away";
    unsigned GameTweakMask() const
    {
        unsigned mask = 0;
        for (std::size_t n = 0; n < tweaks.size(); ++n)
            if (tweaks[n]) mask |= GameTweakBit(kGameTweaks[n].tweak);
        return mask;
    }
    // What the game module receives: the tweaks and the HUD's start state.
    unsigned ModuleTweakMask() const
    {
        return GameTweakMask() | (show_hud ? 0u : GameTweakBit(GameTweak::NoHud));
    }
};

struct ConfigFile
{
    std::filesystem::path path;
    std::string contents;
    bool exists = false;
    Settings settings;
    std::set<std::string> configured_keys; // Parsed INI provenance; run overrides never enter this set.
};

// A gamepad profile name: 1-32 characters, no surrounding spaces, quotes,
// brackets, '=', '|' or control characters, and not "Default". Names
// compare case-insensitively.
bool ValidPadProfileName(const std::string& name);
bool SamePadProfileName(const std::string& a, const std::string& b);

ConfigFile LoadConfig(const std::filesystem::path& path, bool allow_missing = false);
void SaveConfig(ConfigFile& file, const Settings& settings);
std::filesystem::path ResolveDiscPath(const Settings& settings, const std::filesystem::path& config);
// game.gamecube_disc's path, empty when none is set.
std::filesystem::path ResolveGameCubeDiscPath(const Settings& settings, const std::filesystem::path& config);
// Saves, shader caches and the game clock, beside the executable: data/, or
// the original-main-credits-data/ of earlier builds until it has been moved.
std::filesystem::path DataFolder(const std::filesystem::path& executable_directory);
// Renames the earlier builds' folder to data/ once. Returns false and leaves
// the old folder in use when there is nothing to move or the rename fails.
bool MoveOldDataFolder(const std::filesystem::path& executable_directory);
// The game's Wii storage folder: game.save_folder, or by default
// <data folder>/nand.
std::filesystem::path DefaultSaveFolder(const std::filesystem::path& executable_directory);
std::filesystem::path ResolveSaveFolder(const Settings& settings, const std::filesystem::path& config,
                                        const std::filesystem::path& executable_directory);
std::filesystem::path LoadDiscPath(const std::filesystem::path& config);
// The game folder's path: a relative one counts from the settings file.
std::filesystem::path ResolveGameFolder(const Settings& settings, const std::filesystem::path& config);
// The texture packs folder's path: a relative one counts from the settings file.
std::filesystem::path ResolveTextureFolder(const Settings& settings, const std::filesystem::path& config);
}
