#pragma once

#include "platform/disc.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged
{
// The game code that runs a release. Both European revisions run the code of
// revision 2; a revision 1 disc also turns on the tweak series' adapters for
// its older data (audio bank and script numbering, one pool size).
enum class GameCode : std::uint8_t
{
    Usa,
    Europe,
};

// A game text language: the [game] language value, its launcher label, the
// ISO 639-1 code of the matching PC language and the Wii system language that
// makes original main pick it for this release.
struct GameLanguage
{
    const char* value;
    const char* label;
    const char* iso;
    std::uint8_t sc_language;
};

struct GameRelease
{
    const char* game_id;           // disc ID, e.g. "R4QE01"
    std::uint8_t revision;         // revision byte of the disc header
    const char* dol_sha1;          // SHA-1 of sys/main.dol (lower-case hex)
    const char* region;            // "USA", "Europe"
    const char* title;             // title as printed on the box
    std::uint64_t title_id;        // Wii title: saves and the NAND home
    std::uint16_t title_group;     // group ID of the disc's TMD
    GameCode code;                 // game module that runs it
    bool rev1_data;                // EU revision 1 data on the revision 2 code
    bool playable;                 // this build has the game module for it
    std::uint32_t disc_message_table; // sDiscMessageImages in main.dol
    const GameLanguage* languages; // game text languages, without Automatic;
    std::size_t language_count;    // the first one is the fallback (English)
};

// The decomp game version a game module is built from: "R4QE01" for the USA
// code, "R4QP01_REV2" for the European code.
const char* GameModuleVersion(GameCode code);

// Every release this port knows, playable or not.
std::size_t ReleaseCount();
const GameRelease& ReleaseAt(std::size_t index);

// The USA release: what the launcher shows before a disc is checked.
const GameRelease& DefaultRelease();

// The release with this disc ID and header revision, or nullptr.
const GameRelease* FindRelease(std::string_view game_id, std::uint8_t revision);
inline const GameRelease* FindRelease(const DiscInfo& info) { return FindRelease(info.game_id, info.revision); }

// "USA" or "Europe, revision 2" (the revision only where a region has several).
std::string ReleaseLabel(const GameRelease& release);

// What a player should know about how this build runs the release, or empty:
// a European revision 1 disc plays with the game code of revision 2.
std::string ReleaseNote(const GameRelease& release);

// The playable releases of this build, e.g. "the USA version (R4QE01)".
std::string PlayableReleasesText();

// Why this disc cannot be played in this build, or empty when it can.
// `release` is FindRelease(info) (a separate argument for tests).
std::string UnplayableReason(const DiscInfo& info, const GameRelease* release);
inline std::string UnplayableReason(const DiscInfo& info) { return UnplayableReason(info, FindRelease(info)); }

// The language entry for a [game] language value, or nullptr.
const GameLanguage* FindLanguage(const GameRelease& release, std::string_view value);

// The game text language a [game] language value selects. "auto" takes the
// first PC language (ISO 639-1 codes, preferred first) the release has, else
// its first language (English). A value the release does not have is treated
// as "auto" and marked unavailable.
struct LanguageChoice
{
    const GameLanguage* language;
    bool automatic;
    bool unavailable;
};
LanguageChoice ChooseLanguage(const GameRelease& release, std::string_view preference,
                              const std::vector<std::string>& pc_languages);

// The Wii storage folder of one title inside the save folder. The USA title
// keeps the save folder itself (its saves predate other releases); every
// other title gets a subfolder named after its game code, e.g. "R4QP".
std::filesystem::path TitleSaveFolder(const std::filesystem::path& save_folder, std::uint64_t title_id);
}
