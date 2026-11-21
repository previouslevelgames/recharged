#pragma once

#include "bootstrap/advanced_choices.h"
#include "bootstrap/game_tweaks.h"
#include "bootstrap/key_actions.h"
#include "bootstrap/sound_groups.h"
#include "bootstrap/texts.h"

#include <string>
#include <string_view>

// The texts of the tables the launcher and the game share (common.json):
// gameplay options, sound groups, keyboard actions and hotkeys, setting
// choices. The tables keep their English (log lines, INI comments); the
// texts test keeps the two the same.
namespace mscharged::texts {

inline const char* TableText(std::string_view group, std::string_view key, std::string_view field)
{
    std::string id = "common.";
    id.append(group).append(".").append(key);
    if (!field.empty()) id.append(".").append(field);
    return T(id);
}

inline const char* TweakName(const GameTweakInfo& info) { return TableText("tweaks", info.key, "name"); }
inline const char* TweakAbout(const GameTweakInfo& info) { return TableText("tweaks", info.key, "about"); }
inline const char* SoundGroupName(const SoundGroupInfo& info) { return TableText("sound_groups", info.key, "name"); }
inline const char* SoundGroupAbout(const SoundGroupInfo& info)
{
    return TableText("sound_groups", info.key, "about");
}
// A keyboard & mouse action ([keyboard]) and its note (null for none).
inline const char* KeyActionName(const KeyAction& action) { return TableText("key_actions", action.key, "name"); }
inline const char* KeyActionAbout(const KeyAction& action)
{
    return action.note ? TableText("key_actions", action.key, "about") : nullptr;
}
// A hotkey ([hotkeys]) and its note (null for none).
inline const char* HotkeyActionName(const KeyAction& hotkey) { return TableText("hotkeys", hotkey.key, "name"); }
inline const char* HotkeyActionAbout(const KeyAction& hotkey)
{
    return hotkey.note ? TableText("hotkeys", hotkey.key, "about") : nullptr;
}
// A setting's choice: "launcher_sizes" (kLauncherSizes), "log_levels" (kLogLevels).
inline const char* ChoiceLabel(std::string_view group, const SettingChoice& choice)
{
    return TableText(group, choice.value, {});
}

} // namespace mscharged::texts
