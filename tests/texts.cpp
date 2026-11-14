// Recharged's texts by ID (src/bootstrap/texts.cpp) from made-up language
// folders: English under every translation, a missing ID as itself, the
// PC's language for "auto", {{placeholders}}, plural forms by CLDR's rules,
// capitals beyond ASCII.

#include "bootstrap/gamepad_actions.h"
#include "bootstrap/table_texts.h"
#include "bootstrap/texts.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace texts = mscharged::texts;

namespace {
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (ok) return;
    std::fprintf(stderr, "texts.cpp:%d: %s\n", line, what);
    ++failures;
}
#define CHECK(x) Check((x), #x, __LINE__)

void Write(const std::filesystem::path& path, const char* text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
} // namespace

int main()
{
    const auto folder = std::filesystem::temp_directory_path() / "recharged-texts-test";
    std::filesystem::remove_all(folder);
    Write(folder / "en/common.json", R"({"language": {"name": "English"}})");
    Write(folder / "en/menus.json", R"({
        "pause": {"resume": "RESUME", "quit": "QUIT"},
        "set": {"against": "Set against {{name}}: game {{game}}"},
        "players": {"online_one": "{{count}} player online", "online_other": "{{count}} players online"},
        "minutes_one": "{{count}} minute", "minutes_other": "{{count}} minutes"
    })");
    Write(folder / "de/common.json", R"({"language": {"name": "Deutsch"}})");
    Write(folder / "de/menus.json", R"({
        "pause": {"resume": "WEITER", "quit": ""},
        "players": {"online_one": "{{count}} Spieler online", "online_other": "{{count}} Spieler online"}
    })");
    Write(folder / "ru/common.json", R"({"language": {"name": "Русский"}})");
    Write(folder / "ru/menus.json", R"({
        "minutes_one": "{{count}} минута", "minutes_few": "{{count}} минуты", "minutes_many": "{{count}} минут"
    })");
    Write(folder / "pt-BR/common.json", R"j({"language": {"name": "Português (Brasil)"}})j");
    Write(folder / "broken/menus.json", R"({"pause": )");

    // The shared tables' English is common.json's (built in).
    for (const auto& info : mscharged::kGameTweaks)
        CHECK(std::string(texts::TweakName(info)) == info.name && std::string(texts::TweakAbout(info)) == info.description);
    for (const auto& info : mscharged::kSoundGroups)
        CHECK(std::string(texts::SoundGroupName(info)) == info.name
              && std::string(texts::SoundGroupAbout(info)) == info.description);
    for (const auto& action : mscharged::kKeyActions)
        CHECK(std::string(texts::KeyActionName(action)) == action.label
              && (action.note ? std::string(texts::KeyActionAbout(action)) == action.note : !texts::KeyActionAbout(action)));
    // The gamepad's own names and notes (the launcher's profiles); its last
    // action, the pointer's recentring, is a settings text.
    for (const auto& action : mscharged::kGamepadActions)
        CHECK(std::string(action.key) == "recenter_pointer"
              || (std::string(texts::TableText("pad_actions", action.key, "name")) == action.label
                  && (!action.note || std::string(texts::TableText("pad_actions", action.key, "about")) == action.note)));
    for (const auto& hotkey : mscharged::kHotkeys)
        CHECK(std::string(texts::HotkeyActionName(hotkey)) == hotkey.label
              && (hotkey.note ? std::string(texts::HotkeyActionAbout(hotkey)) == hotkey.note
                              : !texts::HotkeyActionAbout(hotkey)));
    for (const auto& size : mscharged::kLauncherSizes)
        CHECK(std::string(texts::ChoiceLabel("launcher_sizes", size)) == size.label);
    for (const auto& level : mscharged::kLogLevels) CHECK(std::string(texts::ChoiceLabel("log_levels", level)) == level.label);

    // Before any Load: the built-in English (assets/lang/en), else the ID.
    CHECK(std::string(texts::T("common.language.name")) == "English");
    CHECK(std::string(texts::T("menus.test.none")) == "menus.test.none");
    CHECK(texts::Language() == "en");

    // English.
    CHECK(texts::Load(folder, "auto", {"en", "de"}) == "en"); // English preferred first
    CHECK(std::string(texts::T("menus.pause.resume")) == "RESUME");
    CHECK(std::string(texts::T("menus.nothing.here")) == "menus.nothing.here");
    CHECK(texts::T("menus.set.against", {{"name", "Mario"}, {"game", "2"}}) == "Set against Mario: game 2");
    CHECK(texts::Count("menus.players.online", 1) == "1 player online");
    CHECK(texts::Count("menus.players.online", 3) == "3 players online");
    CHECK(texts::Count("menus.players.online", 0) == "0 players online");

    // German over English: an empty text is English's; what German lacks too.
    const char* before = texts::T("menus.pause.resume");
    CHECK(texts::Load(folder, "auto", {"fr", "de"}) == "de");
    CHECK(std::string(texts::T("menus.pause.resume")) == "WEITER");
    CHECK(std::string(texts::T("menus.pause.quit")) == "QUIT");
    CHECK(texts::Count("menus.minutes", 2) == "2 minutes");
    CHECK(texts::Count("menus.players.online", 1) == "1 Spieler online");
    CHECK(std::string(before) == "RESUME"); // a text handed out before stays
    CHECK(texts::Language() == "de");
    {
        // A log line's texts: English.
        const texts::EnglishScope english;
        CHECK(std::string(texts::T("common.language.name")) == "English");
    }
    CHECK(std::string(texts::T("common.language.name")) == "Deutsch");

    // Russian's three forms.
    CHECK(texts::Load(folder, "ru") == "ru");
    CHECK(texts::Count("menus.minutes", 1) == "1 минута");
    CHECK(texts::Count("menus.minutes", 3) == "3 минуты");
    CHECK(texts::Count("menus.minutes", 5) == "5 минут");
    CHECK(texts::Count("menus.minutes", 11) == "11 минут");
    CHECK(texts::Count("menus.minutes", 21) == "21 минута");
    CHECK(texts::Count("menus.minutes", 22) == "22 минуты");

    // A regional folder for the PC's language; unknown and unsafe names fall
    // back to English.
    CHECK(texts::Load(folder, "auto", {"pt"}) == "pt-BR");
    CHECK(texts::Load(folder, "xx") == "en");
    CHECK(texts::Load(folder, "../en") == "en");
    CHECK(texts::Load(folder, "broken") == "broken");
    CHECK(std::string(texts::T("menus.pause.resume")) == "RESUME");

    // The choices: English first, folders without a name left out.
    const auto choices = texts::Languages(folder);
    CHECK(choices.size() == 4);
    CHECK(!choices.empty() && choices[0].code == "en" && choices[0].name == "English");
    CHECK(choices.size() > 1 && choices[1].code == "de" && choices[1].name == "Deutsch");

    // Plural rules.
    CHECK(texts::PluralForm("en", 1) == "one" && texts::PluralForm("en", 0) == "other");
    CHECK(texts::PluralForm("fr", 0) == "one" && texts::PluralForm("fr", 2) == "other");
    CHECK(texts::PluralForm("pt-BR", 1) == "one" && texts::PluralForm("pt-PT", 0) == "other");
    CHECK(texts::PluralForm("pl", 1) == "one" && texts::PluralForm("pl", 4) == "few");
    CHECK(texts::PluralForm("pl", 12) == "many" && texts::PluralForm("pl", 22) == "few");
    CHECK(texts::PluralForm("cs", 3) == "few" && texts::PluralForm("cs", 5) == "other");
    CHECK(texts::PluralForm("ja", 1) == "other");
    CHECK(texts::PluralForm("ar", 2) == "two" && texts::PluralForm("ar", 100) == "other");

    // Capitals.
    CHECK(texts::Upper("resume") == "RESUME");
    CHECK(texts::Upper("Straße") == "STRASSE");
    CHECK(texts::Upper("ça déjà") == "ÇA DÉJÀ");
    CHECK(texts::Upper("łódź ĳ") == "ŁÓDŹ Ĳ");
    CHECK(texts::Upper("привет ёж") == "ПРИВЕТ ЁЖ");
    CHECK(texts::Upper("αβγ ά ς") == "ΑΒΓ Ά Σ");
    CHECK(texts::Upper("日本 1-2") == "日本 1-2");
    CHECK(texts::Upper(std::string("a\xff" "b")) == std::string("A\xff" "B"));

    std::filesystem::remove_all(folder);
    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
