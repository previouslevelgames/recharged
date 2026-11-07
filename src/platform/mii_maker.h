#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Making a new Mii for players without one (the launcher's Miis page). Its
// face is the Wii's own random face (RFL_MakeRandomFace.c); save_import.h's
// AddMii puts the record into the game's Mii database.
namespace mscharged::platform {
// Whether `name` (UTF-8) fits a Mii's name: 1 to 10 characters of UTF-16's
// basic plane, none of them control characters.
bool ValidMiiName(const std::string& name);

// The Wii's twelve favourite colours (RFLFavoriteColor: 0 red, 1 orange ...
// 10 white, 11 black).
inline constexpr int kMiiFavoriteColors = 12;

// A new Mii named `name`, as its 74-byte record in RFL_DB.dat. `seed` picks
// the face; `created` (Unix seconds) makes the create ID unique, as the Mii
// Channel does: the game keys each save slot by it. `favorite_color` (0-11)
// replaces the random face's own favourite colour; -1 keeps it. Throws
// std::invalid_argument for a name ValidMiiName rejects or a colour outside
// -1..11.
std::string MakeRandomMii(const std::string& name, std::uint32_t seed, std::int64_t created,
                          int favorite_color = -1);

// A Mii record's create ID (the game keys the Mii's online save slot by it)
// and favourite colour; the record must be 74 bytes.
std::array<unsigned char, 8> MiiRecordCreateId(const std::string& record);
int MiiRecordFavoriteColor(const std::string& record);

// A Mii record's name (UTF-8), and the same record with another name, the
// rest as it was (a Mii editor's rename). Throw std::invalid_argument for a
// record of another size, and SetMiiRecordName for a name ValidMiiName rejects.
std::string MiiRecordName(const std::string& record);
void SetMiiRecordName(std::string& record, const std::string& name);

// A create ID as 16 lowercase hex digits (the settings file's online.mii),
// and back: nullopt for anything but 16 hex digits.
std::string MiiCreateIdText(const std::array<unsigned char, 8>& create_id);
std::optional<std::array<unsigned char, 8>> ParseMiiCreateId(std::string_view text);

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
