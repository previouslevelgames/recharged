#pragma once

// recharged://watch/<match>?ticket=<hex>&release=<code>: the WFC server's
// Watch button (its docs/reports.md, "Watching"). Recharged registers itself
// for the scheme (platform/watch_link_handler.h); the system starts it with the
// link (Windows, Linux) or hands it to the running launcher (macOS: a drop).
// The link becomes the same ticket file the dashboard's Ticket button gives
// (platform/match_replay_file.h, WatchTicket), which plays like a replay.

#include <filesystem>
#include <string>
#include <string_view>

namespace mscharged
{
// Whether `text` is a recharged: link (any case).
bool IsWatchLink(std::string_view text);
// The link's ticket written as a file in `folder` (watch-match-<match>.msc,
// replaced whole); its path, or empty with `error` set for a link that is not
// one.
std::filesystem::path WriteWatchLinkTicket(std::string_view link, const std::filesystem::path& folder,
                                           std::string& error);
} // namespace mscharged
