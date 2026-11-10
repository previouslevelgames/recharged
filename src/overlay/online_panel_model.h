#pragma once

#include "platform/profile_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The online panel the game opens over its blurred picture (F1, or the Wii
// Remote's HOME button): where the player plays online. The player and the
// switch for random games, their set when there is one, the friends online
// now (and how many are offline) and, while the player is available, the
// others available now; a player's screen to challenge them or keep them as
// a friend. This is its data, made from what the profile service answers and
// without any drawing types; online_panel.h draws it with ImGui.
namespace mscharged::overlay
{
// The colour a status line is drawn in.
enum class Tone
{
    Neutral, // muted text
    Good,    // green: free to play, a set won, ready
    Busy,    // amber: in a set or a match, waiting
    Away,    // dim: offline
    Bad,     // red: a set lost or failed
};

// The dot on a player's picture.
enum class Presence
{
    Free,    // online, free to play
    Busy,    // online, in a set or a match: no challenge now
    Offline, // closed (a set's opponent, the profile's friends)
};

// One player in a list.
struct OnlinePanelRow
{
    enum class Kind
    {
        Player, // a Recharged player (player_id)
        Mii,    // a player of the game's online mode, by their Mii (mii_profile)
    };
    Kind kind = Kind::Player;
    std::int64_t player_id = 0;
    std::int32_t mii_profile = 0;
    // Unique in the panel ("p<player id>", "m<Mii profile>"): the row's ImGui
    // id, and which player a player's screen shows.
    std::string key;
    std::string name;
    // The picture: the player's Discord picture (its URL), else their Mii
    // (PictureKey); empty for neither. Without a texture for it the menus
    // draw `initials` on a colour picked by `tint_seed`.
    std::string image;
    std::string initials; // the name's first character, in capitals
    std::int64_t tint_seed = 0;
    Presence presence = Presence::Free;
    std::string status; // "Domination · choosing teams", "Set vs Mario · game 2 of 3"
    Tone tone = Tone::Good;
    std::string detail;    // "@discord username", or a Mii's friend code
    bool discord = false;  // a Discord friend: Discord's logo after the name
    bool is_friend = false;
    // Add or Remove friend (Mii friend for a Mii) on their screen. Discord
    // friends have neither: they are friends already.
    bool can_befriend = false;
    bool can_challenge = false; // to a set, by the player's rules
    // No challenge now: why ("In a set", "Finish your set first"; a Mii's
    // is about the game's online mode).
    std::string blocked;
    // Another version of the game than the player's ("EU", "US", "JP",
    // "KR"): a set with them plays by the European version.
    std::string version;
    // The flag beside the name: a country's two letters (overlay/flags.h),
    // empty for none.
    std::string country;
};

struct OnlinePanelSection
{
    enum class Id
    {
        Online,    // friends, Discord friends and Mii friends online now
        Available, // others open to challenges, and the game's online players
        Friends,   // all of the friends, online first: the player's profile
    };
    Id id = Id::Online;
    std::string title; // "ONLINE NOW"
    std::vector<OnlinePanelRow> rows; // free first, then busy, by name
    std::string empty; // what shows without rows
    std::string hint;  // under it, smaller
    std::string aside; // at the right of its heading, quiet: "3 friends offline"
};

// The player's set (competitive series): the one still to be played, or the
// last one that ended.
struct OnlinePanelSet
{
    enum class Stage
    {
        None,       // no set (or one that ended without a result)
        Challenged, // the opponent challenges the player: accept or decline
        Waiting,    // the player's challenge, waiting for the opponent's answer
        Ready,      // accepted: both say Ready to play!
        Starting,   // both are ready: the games start
        Playing,    // the games play it
        Ended,      // finished, with a result
    };
    Stage stage = Stage::None;
    std::int64_t match = 0; // the set's id, for the actions
    OnlinePanelRow opponent;
    std::string title;  // "Mario challenges you", "Your set against Mario", "You won the set"
    std::string status; // what happens now
    Tone tone = Tone::Neutral;
    std::string rules;  // "Competitive: 3 minutes, best of 3, random stadium"
    // The games won and lost so far, of a series of `games` (best of; 0 shows no score).
    int won = 0, lost = 0, games = 0;
    // What the player can do now (OnlinePanelActions).
    bool can_accept = false, can_decline = false, can_ready = false, can_cancel = false, can_rematch = false;
    std::string cancel_label; // "Withdraw" or "Cancel set"
};

// The picture key of a Mii's record as the server sends it (base64), empty
// for none: the host draws the head ("miidata:<base64>").
inline std::string MiiImageKey(const std::string& mii_data)
{
    return mii_data.empty() ? std::string() : "miidata:" + mii_data;
}

// A player's picture: their Discord picture (its URL) when they have one,
// else their Mii.
inline std::string PictureKey(const std::string& avatar_url, const std::string& mii_data)
{
    return !avatar_url.empty() ? avatar_url : MiiImageKey(mii_data);
}

struct OnlinePanelPlayer
{
    std::string name;
    std::string detail; // "@discord username"
    std::string image;  // as a row's (the host's "mii:own" for the player's own Mii)
    std::string initials;
    std::int64_t tint_seed = 0;
    std::string country;            // the flag beside the name, as a row's
    bool available = false;         // open to challenges
    bool can_set_available = false; // the server knows random games (1.2.1)
};

// What a challenge can start from: one of the server's rule sets or one of
// the player's own presets (the launcher's Match rules page).
struct OnlinePanelPreset
{
    std::string name;
    platform::MatchRules rules;
    std::int64_t rule_set = 0; // the server's rule set's id; 0 for the player's own
};

// The player's search for a random opponent, as their card shows it.
struct OnlinePanelSearch
{
    bool active = false;
    std::string rules; // the rules' name
    int seconds = 0;   // how long it searches
    int asked = 0;     // players asked so far
};

struct OnlinePanelModel
{
    OnlinePanelPlayer me;
    OnlinePanelSet set;
    // A line over the lists while nobody can be challenged from this game.
    std::string note;
    std::vector<OnlinePanelSection> sections;
    // The friends and Discord friends with Recharged who are offline (the
    // online section's aside says so; none while the lists load).
    int friends_offline = 0;
    // What a challenge can start from (the server's rule sets, then the
    // player's presets) and the one its setup opens with: the last one used.
    std::vector<OnlinePanelPreset> presets;
    int chosen_preset = 0;
    // Searching for a random opponent, or whether the player can start.
    OnlinePanelSearch search;
    bool can_search = false;
    // For the footer: what the player should know (an action the server
    // refused, the server out of reach).
    std::string message;
    Tone message_tone = Tone::Neutral;
    // No player profile: the tab says what one brings and where to make it.
    bool signed_out = false;
    // The player's records for their profile (the host's, while it shows);
    // `stats_missing` for a server without them.
    std::optional<platform::PlayerStats> stats;
    bool stats_missing = false;
};

// What the game knows besides the server's lists and set.
struct OnlinePanelContext
{
    // The signed-in player (GET /api/v1/me; at least the name): name, Discord
    // username, avatar and whether they are open to challenges.
    platform::ProfilePlayer me;
    // The game this computer plays ("R4QE", ...): players of another version
    // carry its tag. Empty: not known.
    std::string game;
    // Recharged players who are the player's Discord friends, as POST
    // /api/v1/players/discord answers: those online join the friends, with
    // Discord's logo. The game has no Discord Social SDK: whatever list of
    // them the server gives it.
    std::vector<platform::ProfilePlayer> discord_friends;
    // The running set's games as this game counted them
    // (CompetitiveHost::CurrentSet); without it the server's report counts.
    struct Score
    {
        int won = 0, lost = 0;
    };
    std::optional<Score> score;
    // This game plays a match (or a set's game): nobody can be challenged from it now.
    bool in_match = false;
    // There are rules to challenge by (the server has a rule set).
    bool can_challenge = true;
    // The server's rule sets and the player's own presets (the launcher's
    // Match rules page), and the one a challenge's setup opens with
    // ([online] challenge_rules, its name; empty or unknown: the first).
    std::vector<platform::RuleSet> rule_sets;
    std::vector<OnlinePanelPreset> my_presets;
    // The built-in presets (Retail ranked, Competitive): always first, as
    // they are; a server's rule set of the same name gives its id when its
    // rules are the same, and doesn't show itself.
    std::vector<OnlinePanelPreset> built_in_presets;
    std::string chosen_rules;
    // The server knows searches for a random opponent (1.2.4), the player's
    // search and how long it runs.
    bool searches = false;
    std::optional<platform::ProfileSearch> search;
    int search_seconds = 0;
    // The server knows random games (1.2.1): the switch in the panel.
    bool random_games = true;
    // The lists haven't come yet: they say "Loading...".
    bool loading = false;
    // For the footer (the feed's notice or error); `error` draws it in red.
    std::string message;
    bool error = false;
    // No player profile on this computer (the launcher's Player profile
    // page makes one): nothing but what one brings.
    bool signed_out = false;
};

// The panel's data from the lobby's lists (ProfileService::Players()) and the
// player's set (ProfileService::CurrentMatch()).
OnlinePanelModel MakeOnlinePanelModel(const platform::PlayerLists& lists,
                                      const std::optional<platform::ProfileMatch>& match,
                                      const OnlinePanelContext& context);

// What waits for the player in the panel: a challenge to answer, their Ready
// to give for a set. The overlay's Online tab shows it as a badge.
int OnlinePanelWaiting(const OnlinePanelModel& model);

// The row of `key` in the lists (or the set's opponent), for a player's screen; null when gone.
const OnlinePanelRow* FindRow(const OnlinePanelModel& model, const std::string& key);

// A name's first character in capitals (a whole UTF-8 character), "?" for none.
std::string Initials(const std::string& name);
} // namespace mscharged::overlay
