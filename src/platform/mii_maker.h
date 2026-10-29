#pragma once

#include <array>
#include <cstdint>
#include <string>

// Making a new Mii for players without one (the launcher's Miis page). Its
// face is the Wii's own random face (RFL_MakeRandomFace.c); save_import.h's
// AddMii puts the record into the game's Mii database.
namespace mscharged::platform {
// Whether `name` (UTF-8) fits a Mii's name: 1 to 10 characters of UTF-16's
// basic plane, none of them control characters.
bool ValidMiiName(const std::string& name);

// A new Mii named `name`, as its 74-byte record in RFL_DB.dat. `seed` picks
// the face; `created` (Unix seconds) makes the create ID unique, as the Mii
// Channel does: the game keys each save slot by it. Throws
// std::invalid_argument for a name ValidMiiName rejects.
std::string MakeRandomMii(const std::string& name, std::uint32_t seed, std::int64_t created);

// A player's own Mii for competitive play, the same on every computer: its
// face and its create ID come from `key` alone (e.g. "discord:<account id>"),
// so the game finds it again by the create ID and keeps its online slot.
// Throws std::invalid_argument for a name ValidMiiName rejects.
std::string MakeKeyedMii(const std::string& name, const std::string& key);

// The create ID MakeKeyedMii gives `key`'s Mii: an ordinary Mii's (neither
// special nor temporary).
std::array<unsigned char, 8> KeyedMiiCreateId(const std::string& key);

// `name` (UTF-8, e.g. a Discord display name) made a valid Mii name: without
// emoji and other characters outside UTF-16's basic plane, control and
// invisible format characters, with runs of spaces as one, trimmed and cut to
// 10 characters. `fallback` the same when nothing is left, else "Player".
std::string FitMiiName(const std::string& name, const std::string& fallback = {});
} // namespace mscharged::platform
