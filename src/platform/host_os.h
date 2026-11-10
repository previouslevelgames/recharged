#pragma once

#include <string>

namespace mscharged::platform {
// The operating system and CPU this program runs on, short enough for a
// line of the WFC server's admin pages: "Windows 11 (26100), x86_64",
// "macOS 26.3.1, arm64", "Linux (SteamOS), x86_64". Wine and Rosetta say so
// ("..., x86_64, Wine 9.0 on Linux"). The architecture is the program's own
// (the slice that runs, an emulated one included).
std::string HostOsName();

// The system and CPU this program was built for, lowercase: the game module's
// kind, whose offsets the desync dossiers' callers are. "linux x86_64",
// "windows x86_64" (under Wine too), "macos arm64"; empty when unknown.
std::string HostPlatform();
} // namespace mscharged::platform
