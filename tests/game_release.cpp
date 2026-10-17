// The known releases: lookups, texts, game text languages and save folders.
#include "platform/game_release.h"
#if MSCHARGED_TEST_PC_LANGUAGES
#include "platform/pc_languages.h"
#include <SDL3/SDL_hints.h>
#endif

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace mscharged;
namespace fs = std::filesystem;
unsigned checks{};

void Check(bool value, const std::string& message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

DiscInfo Disc(const char* id, std::uint8_t revision) { return {"ISO", id, "Fixture", revision, 1}; }

void Releases()
{
    const auto* usa = FindRelease("R4QE01", 1);
    const auto* europe2 = FindRelease("R4QP01", 2);
    const auto* europe1 = FindRelease("R4QP01", 1);
    Check(usa && europe2 && europe1 && ReleaseCount() == 3, "USA and both European revisions are known");
    Check(&DefaultRelease() == usa, "The USA release is the default");
    Check(!FindRelease("R4QE01", 0) && !FindRelease("R4QP01", 0) && !FindRelease("R4QJ01", 0),
          "Unknown revisions and regions are not releases");
    Check(usa->title_id == 0x0001000052345145ull && europe2->title_id == 0x0001000052345150ull
              && europe1->title_id == europe2->title_id,
          "Both European revisions share their Wii title");
    Check(usa->title_group == 0x3031 && europe1->title_group == 0x3031 && europe2->title_group == 0x3031,
          "Every release has the TMD group 0x3031");
    Check(usa->code == GameCode::Usa && europe2->code == GameCode::Europe && europe1->code == GameCode::Europe
              && europe1->rev1_data && !europe2->rev1_data && !usa->rev1_data,
          "European revision 1 runs on the revision 2 code with its data adapters");
    Check(usa->playable && SupportedRelease(Disc("R4QE01", 1)), "The USA release is playable");

    Check(ReleaseLabel(*usa) == "USA" && ReleaseLabel(*europe2) == "Europe, revision 2"
              && ReleaseLabel(*europe1) == "Europe, revision 1",
          "Labels name the revision only where a region has several");
    Check(ReleaseNote(*europe1) == "Plays with the game code of revision 2." && ReleaseNote(*europe2).empty()
              && ReleaseNote(*usa).empty(),
          "Only European revision 1 carries a note");

    // Reasons follow the table; a release copy marked playable has none.
    Check(UnplayableReason(Disc("R4QE01", 1)).empty(), "A playable disc has no reason");
    auto playable = *europe2;
    playable.playable = true;
    Check(UnplayableReason(Disc("R4QP01", 2), &playable).empty(), "A playable European release has no reason");
    auto blocked = *europe1;
    blocked.playable = false;
    Check(UnplayableReason(Disc("R4QP01", 1), &blocked)
              == "This build cannot run the European version (R4QP01, revision 1) yet.",
          "An unplayable release names its version: " + UnplayableReason(Disc("R4QP01", 1), &blocked));
    Check(UnplayableReason(Disc("R4QP01", 3), nullptr)
              == "Recharged knows the European version (R4QP01) in revision 2 and revision 1; this disc is revision 3.",
          "An unknown revision lists the known ones: " + UnplayableReason(Disc("R4QP01", 3), nullptr));
    Check(UnplayableReason(Disc("R4QJ01", 0)) == "Mario Strikers Charged for Japan (R4QJ01) is not supported yet."
              && UnplayableReason(Disc("R4QK01", 0)) == "Mario Strikers Charged for Korea (R4QK01) is not supported yet.",
          "Japan and Korea are not supported yet");
    const auto runs = PlayableReleasesText();
    Check(runs == "the USA version (R4QE01)" || runs == "the USA (R4QE01) and European (R4QP01) versions",
          "Playable versions: " + runs);
}

void Languages()
{
    const auto& usa = *FindRelease("R4QE01", 1);
    const auto& europe = *FindRelease("R4QP01", 2);
    Check(usa.language_count == 3 && std::string(usa.languages[0].value) == "english", "USA: English first");
    Check(europe.language_count == 5 && std::string(europe.languages[0].value) == "english",
          "Europe: English and four more");
    struct Expect { const GameRelease* release; const char* value; std::uint8_t sc; };
    for (const auto& e : {Expect{&usa, "english", 1}, Expect{&usa, "french", 3}, Expect{&usa, "spanish", 4},
                          Expect{&europe, "english", 1}, Expect{&europe, "german", 2}, Expect{&europe, "french", 3},
                          Expect{&europe, "spanish", 4}, Expect{&europe, "italian", 5}})
    {
        const auto* language = FindLanguage(*e.release, e.value);
        Check(language && language->sc_language == e.sc, std::string("Wii language of ") + e.value);
        const auto choice = ChooseLanguage(*e.release, e.value, {"ja", "de"});
        Check(choice.language == language && !choice.automatic && !choice.unavailable,
              std::string("An offered language is kept: ") + e.value);
    }
    Check(!FindLanguage(usa, "german") && !FindLanguage(usa, "italian") && !FindLanguage(europe, "dutch")
              && !FindLanguage(europe, "japanese"),
          "Languages a release does not have are absent");

    // Automatic: the first PC language the release has, else English.
    auto choice = ChooseLanguage(europe, "auto", {"nl", "it", "de"});
    Check(choice.automatic && !choice.unavailable && std::string(choice.language->value) == "italian",
          "Automatic picks the first PC language the release has");
    choice = ChooseLanguage(usa, "auto", {"nl", "it", "de"});
    Check(choice.automatic && std::string(choice.language->value) == "english", "No match falls back to English");
    choice = ChooseLanguage(usa, "auto", {"fr"});
    Check(std::string(choice.language->value) == "french", "A French PC gets NA French on the USA release");
    choice = ChooseLanguage(europe, "auto", {});
    Check(std::string(choice.language->value) == "english", "Without PC languages Automatic is English");

    // A saved language the release does not have starts as Automatic.
    choice = ChooseLanguage(usa, "german", {"es"});
    Check(choice.automatic && choice.unavailable && std::string(choice.language->value) == "spanish",
          "An unavailable language falls back to Automatic");
    choice = ChooseLanguage(europe, "japanese", {});
    Check(choice.unavailable && std::string(choice.language->value) == "english",
          "An unavailable language without PC match falls back to English");
}

void SaveFolders()
{
    const fs::path folder = fs::path("saves") / "nand";
    Check(TitleSaveFolder(folder, 0x0001000052345145ull) == folder, "The USA title keeps the save folder");
    Check(TitleSaveFolder(folder, 0x0001000052345150ull) == folder / "R4QP", "Europe: the R4QP subfolder");
    Check(TitleSaveFolder(folder, 0x000100005234514Aull) == folder / "R4QJ", "Other titles: their game code");
    Check(TitleSaveFolder(folder, 0x0123456789ABCDEFull) == folder / "0123456789abcdef",
          "A title without a printable code: its hex ID");
}

#if MSCHARGED_TEST_PC_LANGUAGES
void PcLanguageCodes()
{
    Check(SDL_SetHintWithPriority(SDL_HINT_PREFERRED_LOCALES, "de_CH,EN_us,it", SDL_HINT_OVERRIDE),
          "Preferred locales hint");
    const auto codes = platform::PcLanguages();
    Check(codes == std::vector<std::string>{"de", "en", "it"}, "PC languages are lower-case codes, preferred first");
    const auto choice = ChooseLanguage(*FindRelease("R4QP01", 1), "auto", codes);
    Check(std::string(choice.language->value) == "german", "A Swiss German PC gets German on a European disc");
}
#endif
}

int main()
{
    try
    {
        Releases();
        Languages();
        SaveFolders();
#if MSCHARGED_TEST_PC_LANGUAGES
        PcLanguageCodes();
#endif
        std::printf("game_release: %u checks passed\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "game_release: %s\n", error.what());
        return 1;
    }
}
