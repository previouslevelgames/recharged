#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

// Characters of players' names that the fonts Recharged ships lack: Japanese,
// Korean, symbols such as ★♥♪. The text the profile server sends (names of
// players and Miis, leaderboards, chats) is noted here, and the launcher's
// and the game overlay's fonts add these characters from the system's fonts
// (launcher/fallback_fonts) when they build. Thread-safe.
namespace mscharged::platform
{
// Notes the characters of `utf8` the shipped font lacks (up to the first
// 65,536 code points; at most kMaxNameGlyphs in all).
void NoteNameGlyphs(std::string_view utf8);
// Grows by one each time a new character is noted: fonts built for an older
// value miss some.
std::uint64_t NameGlyphsVersion();
// The characters noted, in order.
std::vector<std::uint32_t> NameGlyphs();
// Whether the shipped font has `codepoint` (what the fonts are built with).
bool ShippedFontHas(std::uint32_t codepoint);

inline constexpr std::size_t kMaxNameGlyphs = 4096;
} // namespace mscharged::platform
