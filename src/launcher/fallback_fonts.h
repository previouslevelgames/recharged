#pragma once

#include <cstdint>

struct ImFontAtlas;

// The characters of players' names the shipped font lacks
// (platform/name_glyphs), merged into the fonts from the system's fonts:
// Japanese and Korean (Yu Gothic, Meiryo, MS Gothic, Malgun Gothic on
// Windows; Hiragino and Apple SD Gothic Neo on macOS; Noto Sans CJK on
// Linux) and symbols (Segoe UI Symbol, Apple Symbols, Noto Sans Symbols 2,
// DejaVu Sans). A system without them keeps showing "?".
namespace mscharged::launcher
{
// Before a font build: takes the characters noted so far (their version, for
// NameGlyphsBuilt) for the MergeNameGlyphs calls of this build.
void PrepareNameGlyphs();
// Right after adding a font of `pixels` to `atlas`: merges the prepared
// characters into it from the system's fonts (read once, when first needed).
void MergeNameGlyphs(ImFontAtlas& atlas, float pixels);
// The noted characters' version the last PrepareNameGlyphs took.
std::uint64_t NameGlyphsBuilt();
} // namespace mscharged::launcher
