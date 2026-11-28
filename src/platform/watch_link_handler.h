#pragma once

#include <filesystem>
#include <string>

// Recharged as the program for recharged:// links (bootstrap/watch_link.h): the
// WFC server dashboard's Watch button opens them. The launcher registers the
// running program each time it starts, for this user only, so a moved or
// updated Recharged keeps them: on Windows under HKEY_CURRENT_USER\Software\
// Classes\recharged, on Linux as a hidden desktop entry (recharged-watch.desktop
// in the user's applications folder) and the scheme's default in mimeapps.list.
// On macOS the app's Info.plist declares the scheme; the system hands the link
// to the launcher as a drop.
namespace mscharged::platform
{
// What changed, for the log ("" when it was registered already); never throws.
std::string RegisterWatchLinks(const std::filesystem::path& program);
} // namespace mscharged::platform
