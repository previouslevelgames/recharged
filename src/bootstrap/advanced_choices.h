#pragma once

#include <array>

// The choices of the Advanced settings the launcher and the game's menus
// both offer: each setting's value in recharged.ini and the name players see.
namespace mscharged {
struct SettingChoice {
    const char* value;
    const char* label;
};

// launcher.ui_scale: the launcher's size.
inline constexpr std::array kLauncherSizes = {
    SettingChoice{"auto", "Automatic"}, SettingChoice{"75", "75%"},   SettingChoice{"100", "100%"},
    SettingChoice{"125", "125%"},       SettingChoice{"150", "150%"}, SettingChoice{"175", "175%"},
    SettingChoice{"200", "200%"},
};

// advanced.log_level: which messages reach the console and the log (info by
// default).
inline constexpr std::array kLogLevels = {
    SettingChoice{"error", "Errors"},
    SettingChoice{"warning", "Warnings"},
    SettingChoice{"info", "Info"},
    SettingChoice{"debug", "Debug"},
};
inline constexpr int kDefaultLogLevel = 2;
} // namespace mscharged
