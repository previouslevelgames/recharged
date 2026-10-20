#include "platform/game_presence.h"

#include <cstdlib>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace mscharged::platform
{
namespace
{
// The game's scene list (SceneList in Game/BaseGameSceneManager.h).
enum Scene : int
{
    kTitle = 0,
    kMainMenu = 1,
    kChooseCaptainsDomination = 2,
    kChooseStadium = 5,
    kChooseCaptainsCup = 6,
    kChooseSidesCup = 8,
    kCupCheater = 9,
    kOptions = 13,
    kVisualOptions = 15,
    kLegal = 16,
    kIntroMovie = 22,
    kCredits = 23,
    kNetworkStart = 24,
    kGameplayOptions = 27,
    kCheatsList = 28,
    kRoadToCup = 31,
    kCupSchedule = 32,
    kGameResults = 33,
    kCupNews = 39,
    kOnlineFirst = 40,
    kOnlineLast = 57,
    kHallOfFameFirst = 58,
    kHallOfFameLast = 74,
    kChallengeSelect = 75,
    kStrikers101Select = 76,
    kChallengeNews = 77,
    kHealthWarning = 79,
    kPause = 80,
    kStrikers101Pause = 84,
    kLesson = 85,
    kStrikers101Lessons = 86,
    kLessonMovie = 87,
};

// GameInfoManager::eGameMode and cGame's eGameState.
enum Mode : int
{
    kDomination = 0,
    kOnlineTournament = 1,
    kDemo = 2,
    kCup = 3,
    kChallenge = 4,
};
enum GameState : int
{
    kPreGame = 0,
    kKickoff = 1,
    kPostGoal = 2,
    kEndGame = 3,
    kUnloading = 4,
    kGameplay = 5,
    kOvertime = 6,
};

constexpr const char* kSeparator = " \xC2\xB7 "; // " · "
// How far the game's clock may drift from Discord's before the timer moves.
constexpr std::int64_t kClockSlack = 2;

struct MenuText
{
    const char* details;
    const char* state; // nullptr: the game line
};

// The status for a menu screen, or nothing for screens that only sit on top
// of another one (popups, the pointer, loading screens).
std::optional<MenuText> MenuFor(int scene, const ChargedPresenceSnapshot& s)
{
    if (scene == kTitle) return MenuText{"Title screen", nullptr};
    if ((scene >= kLegal && scene <= kIntroMovie) || scene == kHealthWarning) return MenuText{"Starting up", nullptr};
    if (scene == kMainMenu) return MenuText{"Main menu", nullptr};
    if (scene >= kChooseCaptainsDomination && scene < kChooseStadium) return MenuText{"Domination", "Choosing teams"};
    if (scene == kChooseStadium) return MenuText{"Domination", "Choosing a stadium"};
    if (scene >= kChooseCaptainsCup && scene <= kChooseSidesCup)
        return MenuText{"Road to the Striker Cup", "Choosing a team"};
    if (scene == kCupCheater || (scene >= kRoadToCup && scene <= kCupNews && scene != kGameResults))
        return MenuText{"Road to the Striker Cup", "Cup Central"};
    if (scene == kGameResults) return MenuText{"Match results", nullptr};
    if (scene >= kOptions && scene <= kVisualOptions) return MenuText{"Options", nullptr};
    // The rules before a match ("Domination options") and their cheats list.
    if (scene == kGameplayOptions || scene == kCheatsList)
        return MenuText{s.online ? "Online" : "Domination", "Match options"};
    if (scene == kCredits) return MenuText{"Credits", nullptr};
    if (scene == kNetworkStart || (scene >= kOnlineFirst && scene <= kOnlineLast))
        return MenuText{"Online", "In the menus"};
    if (scene >= kHallOfFameFirst && scene <= kHallOfFameLast) return MenuText{"Hall of Fame", nullptr};
    if (scene == kChallengeSelect || scene == kChallengeNews)
        return MenuText{"Striker Challenges", "Choosing a challenge"};
    if (scene == kStrikers101Select || scene == kStrikers101Lessons || scene == kLesson || scene == kLessonMovie)
        return MenuText{"Strikers 101", "Choosing a lesson"};
    return std::nullopt;
}

bool Paused(const ChargedPresenceSnapshot& s)
{
    for (int n = 0; n < s.scene_count && n < 8; ++n)
        if (s.scenes[n] >= kPause && s.scenes[n] <= kStrikers101Pause) return true;
    return false;
}

std::string MatchMode(const ChargedPresenceSnapshot& s)
{
    if (s.strikers101) return "Strikers 101";
    if (s.online)
        return s.ranked ? "Online ranked match"
            : s.mode == kOnlineTournament ? "Online tournament"
                                          : "Online friendly match";
    switch (s.mode)
    {
    case kCup: return "Road to the Striker Cup";
    case kChallenge: return "Striker Challenge";
    default: return "Domination";
    }
}

// "Luigi 2 - 1 Bowser", the team played on this machine first.
std::string Scoreline(const ChargedPresenceSnapshot& s, bool with_score)
{
    const int first = s.human_sides == 2 ? 1 : 0, second = 1 - first;
    const char* a = CaptainName(s.teams[first]);
    const char* b = CaptainName(s.teams[second]);
    if (!a || !b) return {};
    if (!with_score) return std::string(a) + " vs " + b;
    return std::string(a) + " " + std::to_string(s.scores[first]) + " - " + std::to_string(s.scores[second]) + " " + b;
}
}

const char* CaptainName(int team)
{
    static constexpr const char* kNames[] = {"Mario", "Bowser", "Daisy", "Donkey Kong", "Luigi", "Peach",
                                             "Waluigi", "Wario", "Yoshi", "Bowser Jr.", "Diddy Kong", "Petey"};
    return team >= 0 && team < int(std::size(kNames)) ? kNames[team] : nullptr;
}

const char* StadiumName(int stadium)
{
    static constexpr const char* kNames[] = {
        "Battle Dome", "Bowser Stadium", "Crater Field", "Crystal Canyon", "The Dump", "Galactic Stadium",
        "Konga Coliseum", "The Lava Pit", "Pipeline Central", "Stormship Stadium", "The Palace", "Thunder Island",
        "The Underground", "The Vice", "The Wastelands", "The Sand Tomb", "The Classroom"};
    return stadium >= 0 && stadium < int(std::size(kNames)) ? kNames[stadium] : nullptr;
}

GamePresence::GamePresence(std::string game_title, std::string region, std::string game_id, std::int64_t session_start)
    : game_line_(game_title + kSeparator + region), disc_line_(game_line_ + " (" + game_id + ")"),
      session_start_(session_start)
{
}

discord::Activity GamePresence::Describe(const ChargedPresenceSnapshot& s, std::int64_t now)
{
    discord::Activity activity;
    activity.large_image = "logo";
    activity.large_text = disc_line_;

    // The title screen's demo match belongs to the menus.
    if (s.in_match && s.mode != kDemo)
    {
        const bool paused = Paused(s);
        activity.details = MatchMode(s);
        if (const char* stadium = StadiumName(s.stadium); stadium && !s.strikers101)
            activity.details += kSeparator + std::string(stadium);
        const int state = s.game_state;
        // Loading (no state yet) and the captains' entrance come before kick-off.
        const bool before = state < kKickoff;
        if (s.strikers101)
            activity.state = "In a lesson";
        else if (state == kEndGame || state == kUnloading)
            activity.state = "Final score: " + Scoreline(s, true);
        else
            activity.state = Scoreline(s, !before);
        if (state == kOvertime && !activity.state.empty()) activity.state += std::string(kSeparator) + "Sudden death";
        if (paused && !activity.state.empty()) activity.state += " (paused)";

        // The game's clock counts up from 0; it stops for goals and kick-offs.
        const std::int64_t clock_start = now - std::int64_t(s.game_time);
        if (before || state == kEndGame || state == kUnloading)
            match_start_ = 0;
        else if (state == kGameplay && !paused
                 && (match_start_ == 0 || std::llabs(clock_start - match_start_) > kClockSlack))
            match_start_ = clock_start;
        else if (match_start_ == 0)
            match_start_ = clock_start;
        // Paused, the timer is hidden; in sudden death it runs on with real time.
        if (!paused) activity.start = match_start_;
        return activity;
    }

    match_start_ = 0;
    activity.start = session_start_;
    for (int n = 0; n < s.scene_count && n < 8; ++n)
        if (const auto menu = MenuFor(s.scenes[n], s))
        {
            activity.details = menu->details;
            activity.state = menu->state ? menu->state : game_line_;
            return activity;
        }
    activity.details = "In the menus";
    activity.state = game_line_;
    return activity;
}
}
