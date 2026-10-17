#include "platform/game_release.h"

#include <cctype>
#include <cstdio>
#include <iterator>

namespace mscharged
{
namespace
{
// Wii system languages (SC_LANG_*) as original main maps them: the USA build
// shows English, NA French or NA Spanish; the European build UK English,
// German, French, Spanish or Italian.
constexpr GameLanguage kUsaLanguages[] = {
    {"english", "English", "en", 1},
    {"french", "French", "fr", 3},
    {"spanish", "Spanish", "es", 4},
};
constexpr GameLanguage kEuropeLanguages[] = {
    {"english", "English", "en", 1},
    {"german", "German", "de", 2},
    {"french", "French", "fr", 3},
    {"spanish", "Spanish", "es", 4},
    {"italian", "Italian", "it", 5},
};

// main.dol hashes and disc-message tables follow the decomp's
// config/<version>/build.sha1 and symbols.txt.
constexpr GameRelease kReleases[] = {
    {"R4QE01", 1, "e96d2298067b70652752145b4e63644e6e1b1560", "USA", "Mario Strikers Charged",
     0x0001000052345145ull, 0x3031, GameCode::Usa, false, true, 0x804E70E0u,
     kUsaLanguages, std::size(kUsaLanguages)},
    {"R4QP01", 2, "c39e1eda1942aa2fc96a6a71bfef051698e23516", "Europe", "Mario Strikers Charged Football",
     0x0001000052345150ull, 0x3031, GameCode::Europe, false, true, 0x804E6A08u,
     kEuropeLanguages, std::size(kEuropeLanguages)},
    {"R4QP01", 1, "0a066b755b469648a371d1cfc2d1bcad315b2b4a", "Europe", "Mario Strikers Charged Football",
     0x0001000052345150ull, 0x3031, GameCode::Europe, true, true, 0x804E6488u,
     kEuropeLanguages, std::size(kEuropeLanguages)},
};

// "European" for "Europe", as in "the European version".
std::string RegionAdjective(std::string_view region)
{
    if (region == "Europe") return "European";
    if (region == "Japan") return "Japanese";
    if (region == "Korea") return "Korean";
    return std::string(region);
}

bool SeveralRevisions(std::string_view game_id)
{
    int variants = 0;
    for (const auto& release : kReleases)
        if (game_id == release.game_id) ++variants;
    return variants > 1;
}
}

const char* GameModuleVersion(GameCode code)
{
    return code == GameCode::Europe ? "R4QP01_REV2" : "R4QE01";
}

std::size_t ReleaseCount() { return std::size(kReleases); }

const GameRelease& ReleaseAt(std::size_t index) { return kReleases[index]; }

const GameRelease& DefaultRelease() { return kReleases[0]; }

const GameRelease* FindRelease(std::string_view game_id, std::uint8_t revision)
{
    for (const auto& release : kReleases)
        if (game_id == release.game_id && revision == release.revision) return &release;
    return nullptr;
}

std::string ReleaseLabel(const GameRelease& release)
{
    std::string label = release.region;
    if (SeveralRevisions(release.game_id)) label += ", revision " + std::to_string(release.revision);
    return label;
}

std::string ReleaseNote(const GameRelease& release)
{
    return release.rev1_data ? "Plays with the game code of revision 2." : "";
}

std::string PlayableReleasesText()
{
    std::vector<std::string> versions;
    for (std::size_t i = 0; i < std::size(kReleases); ++i)
    {
        const auto& release = kReleases[i];
        bool listed = false;
        for (std::size_t j = 0; j < i; ++j)
            listed = listed || (kReleases[j].playable && std::string_view(kReleases[j].game_id) == release.game_id);
        if (release.playable && !listed)
            versions.push_back(RegionAdjective(release.region) + " (" + release.game_id + ")");
    }
    if (versions.empty()) return "no version of the game";
    if (versions.size() == 1)
    {
        const auto open = versions[0].find(" (");
        return "the " + versions[0].substr(0, open) + " version" + versions[0].substr(open);
    }
    std::string text = "the ";
    for (std::size_t i = 0; i < versions.size(); ++i)
        text += (i == 0 ? "" : i + 1 == versions.size() ? " and " : ", ") + versions[i];
    return text + " versions";
}

std::string UnplayableReason(const DiscInfo& info, const GameRelease* release)
{
    if (release && release->playable) return {};
    if (release)
    {
        std::string version = RegionAdjective(release->region) + " version (" + info.game_id;
        if (SeveralRevisions(release->game_id)) version += ", revision " + std::to_string(release->revision);
        return "This build cannot run the " + version + ") yet.";
    }
    std::string known;
    for (const auto& other : kReleases)
        if (info.game_id == other.game_id)
        {
            known += (known.empty() ? "revision " : " and revision ") + std::to_string(other.revision);
            if (!release) release = &other;
        }
    if (release)
        return "Recharged knows the " + RegionAdjective(release->region) + " version (" + info.game_id + ") in "
            + known + "; this disc is revision " + std::to_string(info.revision) + ".";
    return "Mario Strikers Charged for " + std::string(DiscRegionName(info.game_id)) + " (" + info.game_id
        + ") is not supported yet.";
}

const GameLanguage* FindLanguage(const GameRelease& release, std::string_view value)
{
    for (std::size_t i = 0; i < release.language_count; ++i)
        if (value == release.languages[i].value) return &release.languages[i];
    return nullptr;
}

LanguageChoice ChooseLanguage(const GameRelease& release, std::string_view preference,
                              const std::vector<std::string>& pc_languages)
{
    if (preference != "auto")
        if (const auto* language = FindLanguage(release, preference)) return {language, false, false};
    const bool unavailable = preference != "auto";
    for (const auto& code : pc_languages)
        for (std::size_t i = 0; i < release.language_count; ++i)
            if (code == release.languages[i].iso) return {&release.languages[i], true, unavailable};
    return {&release.languages[0], true, unavailable};
}

std::filesystem::path TitleSaveFolder(const std::filesystem::path& save_folder, std::uint64_t title_id)
{
    if (title_id == DefaultRelease().title_id) return save_folder;
    std::string code;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const char c = char((title_id >> shift) & 0xff);
        if (!std::isalnum(static_cast<unsigned char>(c))) { code.clear(); break; }
        code += c;
    }
    if (code.size() != 4)
    {
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(title_id));
        code = hex;
    }
    return save_folder / code;
}

bool SupportedRelease(const DiscInfo& info)
{
    const auto* release = FindRelease(info);
    return release && release->playable;
}
}
