#pragma once

#include "platform/presence_snapshot.h"

namespace mscharged::platform
{
// The screens the game shows, as the profile service's lists show them (the
// Recharged-Screen header, recharged-wfc-srv docs/profiles.md, "Activity"):
// the topmost screen on the scene stack that says where the game is (popups,
// the pointer and loading screens sit on top of another one); nullptr in a
// match, where its kind says enough (MatchKindOf).
inline const char* ScreenOf(const ChargedPresenceSnapshot& s)
{
    // GameInfoManager::eGameMode's title screen demo match, which belongs to
    // the title screen.
    constexpr int kDemo = 2;
    if (s.in_match && s.mode != kDemo) return nullptr;
    // SceneList (Game/BaseGameSceneManager.h).
    enum : int
    {
        kTitle = 0,
        kMainMenu = 1,
        kChooseCaptains = 2,
        kChooseStadium = 5,
        kCupCaptains = 6,
        kCupSides = 8,
        kCupCheater = 9,
        kSpoils = 11,
        kTournamentOptions = 12,
        kOptions = 13,
        kVisualOptions = 15,
        kLegal = 16,
        kIntroMovie = 22,
        kCredits = 23,
        kNetworkStart = 24,
        kGameplayOptions = 27,
        kCheatsList = 28,
        kRoadToCup = 31,
        kGameResults = 33,
        kCupNews = 39,
        kOnlineMenu = 40,
        kOnlineRanked = 41,
        kOnlineUnranked = 42,
        kInvitePlayers = 44,
        kInvitePreview = 45,
        kMatchmakingDraft = 49,
        kFriendsDraft = 50,
        kInviteResponse = 52,
        kInviteStatus = 53,
        kFriendsChooseSides = 56,
        kConnectionQuality = 57,
        kHallOfFameFirst = 58,
        kHallOfFameLast = 74,
        kChallengeSelect = 75,
        kStrikers101Select = 76,
        kChallengeNews = 77,
        kTournamentSides = 78,
        kHealthWarning = 79,
        kLesson = 85,
        kLessonMovie = 87,
    };
    for (int n = 0; n < s.scene_count && n < 8; ++n)
    {
        const int scene = s.scenes[n];
        if (scene == kTitle || (scene >= kLegal && scene <= kIntroMovie) || scene == kHealthWarning) return "starting";
        if (scene == kMainMenu || (scene >= kOptions && scene <= kVisualOptions) || scene == kCredits
            || scene == kSpoils || (scene >= kHallOfFameFirst && scene <= kHallOfFameLast))
            return "menus";
        if (scene >= kChooseCaptains && scene <= kChooseStadium) return "domination";
        // The rules before a match: Domination's, or a friend match's host's.
        if (scene == kGameplayOptions || scene == kCheatsList) return s.online ? "room" : "domination";
        if ((scene >= kCupCaptains && scene <= kCupSides) || scene == kTournamentSides) return "cup_team";
        if (scene == kGameResults) return "results";
        if (scene == kCupCheater || scene == kTournamentOptions || (scene >= kRoadToCup && scene <= kCupNews))
            return "cup";
        if (scene == kChallengeSelect || scene == kChallengeNews) return "challenges";
        if (scene == kStrikers101Select || (scene >= kLesson && scene <= kLessonMovie)) return "lessons";
        if (scene == kOnlineRanked) return "ranked";
        if (scene == kMatchmakingDraft || scene == kFriendsDraft) return "draft";
        if (scene == kOnlineUnranked || scene == kInvitePlayers || scene == kInvitePreview
            || scene == kInviteResponse || scene == kInviteStatus || scene == kFriendsChooseSides)
            return "room";
        if (scene == kNetworkStart || (scene >= kOnlineMenu && scene <= kConnectionQuality)) return "online";
    }
    return "menus";
}
} // namespace mscharged::platform
