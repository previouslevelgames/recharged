#pragma once

#include "platform/profile_service.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mscharged::platform
{
// The game's online panel (F1 or HOME, src/online_panel): the player, their
// lists, their Discord friends who play and their set from the profile
// service, with the session the launcher signed in for this settings file.
// Its calls run on a thread of their own, so the game never waits for the
// server: while the panel is open it asks every few seconds (at once when it
// opens), and the panel's actions go out there; the next answer shows their
// result.
class OnlinePanelFeed
{
public:
    using Log = std::function<void(const std::string&)>;

    // Nothing without online.profile_server or a session for it (the reason
    // goes to `off`). `challenge_rules`: [online] challenge_rules, the rule
    // set a challenge plays by (its name; empty or unknown: the first).
    // `discord_friends`: the Discord account ids of the player's Discord
    // friends, as the launcher knows them (the game has no Discord SDK);
    // which of them play Recharged is asked with the lists. `log` gets a
    // line when that fails (once until it works again), on the feed's thread.
    static std::unique_ptr<OnlinePanelFeed> Start(const std::string& server_setting,
                                                  const std::filesystem::path& session_file,
                                                  std::string challenge_rules,
                                                  std::vector<std::string> discord_friends, Log log,
                                                  std::string* off);
    ~OnlinePanelFeed();
    OnlinePanelFeed(const OnlinePanelFeed&) = delete;
    OnlinePanelFeed& operator=(const OnlinePanelFeed&) = delete;

    // The panel opened or closed: it asks the server only while open.
    void SetOpen(bool open);
    // The Ranked tab is shown: the leaderboards come with the lists (at once
    // when it opens).
    void WantLeaderboard(bool want);
    // The player's profile is shown: their records come with the lists (at
    // once when it opens).
    void WantStats(bool want);
    // The Miis of the player's WFC profiles (profile id, the Mii's 74-byte
    // record), sent to the server once with the next answers, so others'
    // menus show them; a server without Miis (before 1.2.22) is not asked again.
    void SetOwnMiis(std::vector<std::pair<std::int32_t, std::string>> miis);
    // What the game does, for the server's lists ("game" or "match"), and
    // in a match its kind (MatchKindOf).
    void SetActivity(std::string activity, std::string match_kind = {});

    // The last answers. `loaded` once the first arrived; `version` grows with
    // each, so the caller can tell a new one.
    struct Snapshot
    {
        bool loaded = false;
        std::uint64_t version = 0;
        ProfilePlayer me;
        PlayerLists lists;
        std::optional<ProfileMatch> match;
        // The player's search for a random opponent, and the server's clock
        // when the feed last asked (Unix seconds) and the feed's then.
        std::optional<ProfileSearch> search;
        std::int64_t server_time = 0;
        std::chrono::steady_clock::time_point fetched{};
        // The Discord friends who play Recharged (FindDiscordPlayers); the
        // last list while the server can't say.
        std::vector<ProfilePlayer> discord_friends;
        std::vector<RuleSet> rule_sets;
        std::string server_version;
        std::string error; // the last call that failed, for the panel's footer
        // The game's RANKED leaderboards, while the Ranked tab wants them
        // (asked at most once a minute); `leaderboard_missing` for a server
        // without them (before 1.2.1).
        std::optional<Leaderboard> leaderboard;
        bool leaderboard_missing = false;
        std::chrono::steady_clock::time_point leaderboard_fetched{};
        // The player's records, while the profile wants them (asked at most
        // once a minute); `stats_missing` for a server without them (before 1.2.22).
        std::optional<PlayerStats> stats;
        bool stats_missing = false;
        std::chrono::steady_clock::time_point stats_fetched{};
    };
    Snapshot Latest();

    // The rule set the next challenges play by (its name; empty or unknown:
    // the server's first), as the player chooses it in the panel. From any thread.
    void SetChallengeRules(std::string challenge_rules);
    std::string ChallengeRules();

    // The panel's actions, sent in turn on the feed's thread. A challenge
    // plays by one of the server's rule sets as it is (`rule_set`), else by
    // `rules`, under `name` (the preset they come from).
    void Challenge(std::int64_t player, std::int64_t rule_set, MatchRules rules, std::string name);
    void AddFriend(std::int64_t player);
    void RemoveFriend(std::int64_t player);
    void AddMiiFriend(std::int32_t profile);
    void RemoveMiiFriend(std::int32_t profile);
    void SetAvailable(bool available);
    void ReadyToPlay(std::int64_t match, bool ready);
    void Cancel(std::int64_t match);
    // Search for a random opponent (as Challenge, without a player; `game`
    // the first four characters of the game's id), and stop searching.
    void StartSearch(std::int64_t rule_set, MatchRules rules, std::string name, std::string game);
    void StopSearch();
    // A short message for the player about an action the server did not
    // take, once.
    std::optional<std::string> TakeNotice();

private:
    OnlinePanelFeed(std::string server, std::string token, std::string challenge_rules,
                    std::vector<std::string> discord_friends, Log log);
    void Run();
    void Fetch(ProfileService& api);
    void FetchDiscordFriends(ProfileService& api, std::vector<ProfilePlayer>& players);
    void Send(std::function<void(ProfileService&)> action, std::string what);

    const std::string server_, token_;
    const std::vector<std::string> discord_ids_;
    const Log log_;
    bool discord_failed_ = false; // the feed's thread only
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false, open_ = false, due_ = false, want_leaderboard_ = false, want_stats_ = false;
    // The player's Miis to send, until sent (or the server has no Miis).
    std::vector<std::pair<std::int32_t, std::string>> own_miis_;
    std::string activity_ = "game", match_kind_;
    std::string challenge_rules_;
    std::shared_ptr<class HttpCancel> cancel_;
    struct Pending
    {
        std::function<void(ProfileService&)> action;
        std::string what;
    };
    std::vector<Pending> actions_;
    Snapshot snapshot_;
    std::optional<std::string> notice_;
    std::thread worker_;
};
} // namespace mscharged::platform
