#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// Recharged's own texts (the launcher's, the in-game menus' and messages'),
// by ID, in the player's language. The translations are files in
// assets/lang/<language>/<component>.json (i18next's JSON v4 layout, as
// translation platforms read it; assets/lang/README.md): an ID's first part
// is the component's file ("menus.pause.resume" is "pause" > "resume" in
// menus.json), English (en) is complete and stands in for whatever a
// translation lacks. The game's own texts come from its disc, in the game's
// language, and never pass through here; nor do log lines (testers share
// logs), INI values and key names.
namespace mscharged::texts {

// A {{name}} placeholder's value.
struct Arg {
    std::string_view name;
    std::string value;
};

// Loads the texts of `language`: "auto" (the first of `pc_languages`, the
// PC's preferred ones as platform::PcLanguages gives them, that `folder`
// has, else English) or a folder's name ("de", "pt-BR"). `folder` holds one
// folder per language (assets/lang). Returns the language loaded ("en" when
// nothing else could be). It may be called again (the setting changing):
// texts handed out before stay valid.
std::string Load(const std::filesystem::path& folder, std::string_view language,
                 const std::vector<std::string>& pc_languages = {});

// The text of `id` in the loaded language, else in English (built in: also
// before any Load), else `id` itself. Valid as long as the program runs.
const char* T(std::string_view id);
// The same with its {{name}} placeholders filled in.
std::string T(std::string_view id, std::initializer_list<Arg> args);
// A text that counts something: `id` with the count's form for the loaded
// language's plural rules (`id`_zero, _one, _two, _few, _many, else _other), its
// {{count}} and `args` filled in.
std::string Count(std::string_view id, long long count, std::initializer_list<Arg> args = {});

// The loaded language ("en" before any Load).
std::string Language();

// While one lives, this thread's texts are English whatever the language:
// for log lines that use a text function (testers share their logs).
class EnglishScope {
public:
    EnglishScope();
    ~EnglishScope();
    EnglishScope(const EnglishScope&) = delete;
    EnglishScope& operator=(const EnglishScope&) = delete;
};

// A language `folder` has: its folder's name and its own name for itself
// ("Deutsch"), English first.
struct LanguageChoice {
    std::string code;
    std::string name;
};
std::vector<LanguageChoice> Languages(const std::filesystem::path& folder);

// Every text of the loaded language one after another, for the fonts to
// note their characters (scripts the bundled font lacks come from the
// system's fonts).
std::string AllTexts();

// The plural form ("one", "two", "few", "many" or "other") of a whole
// `count` in `language` (CLDR's cardinal rules; languages without rules
// here: "one" for 1, else "other").
std::string_view PluralForm(std::string_view language, long long count);

// `utf8` in capitals: Latin (ß as SS), Greek and Cyrillic letters; anything
// else as it is.
std::string Upper(std::string_view utf8);

} // namespace mscharged::texts
