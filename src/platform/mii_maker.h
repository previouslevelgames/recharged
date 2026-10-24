#pragma once

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
} // namespace mscharged::platform
