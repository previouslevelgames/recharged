#pragma once

#include "platform/profile_service.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mscharged::launcher
{
// The launcher's side of competitive play, on a background thread: signing in
// to the WFC server's profile service (online.profile_server), the player's
// rule sets and current match, the answers to challenges, and cancellations.
// The launcher polls Snapshot() every frame; requests run one at a time.
class CompetitiveService
{
public:
    enum class State
    {
        NoServer,    // online.profile_server is empty
        Connecting,  // asking the server what it offers, or resuming the saved session
        Unavailable, // no answer, or no profile service there (error says why); tried again later
        SignedOut,
        SigningIn, // the browser page is open (code, url)
        SignedIn,
    };

    struct Status
    {
        State state = State::NoServer;
        std::string server;
        platform::ProfileServerInfo info;
        std::string error;           // the last failure, for a notice
        std::uint64_t error_serial = 0;
        std::string code, url;       // SigningIn: the code both screens show, the page
        std::uint64_t sign_in = 0;   // a new value: a new page to open in the browser
        // Signed in with a Mii: Discord being connected to the profile (code
        // and url as for SigningIn), and why the last try didn't (a Discord
        // account with a profile of its own).
        bool connecting_discord = false;
        std::string connect_error;
        // Why the last Mii sign-in failed (the server takes no more for now,
        // no answer...), for the page that asked; empty after a new one.
        std::string mii_error;
        platform::ProfilePlayer player;
        std::vector<platform::RuleSet> rule_sets;
        std::optional<platform::ProfileMatch> match;
        // The Discord friends (SetDiscordFriends) who have signed in to
        // Recharged, by name.
        std::vector<platform::ProfilePlayer> discord_players;
        // The game's RANKED leaderboards, while asked for (WantLeaderboard):
        // none before the first answer; `leaderboard_missing` for a server
        // without them (before 1.2.1).
        std::optional<platform::Leaderboard> leaderboard;
        bool leaderboard_missing = false;
        // Signed out (LookUpMiis): which of the game save's Miis (their WFC
        // profiles, `known_asked`: the list answered) play for a profile
        // already, and how it signs in again ("game", "discord", "test").
        // No answer (`known_asked` empty) from a server before 1.2.4.
        std::vector<std::pair<std::uint32_t, std::string>> known_miis;
        std::vector<std::uint32_t> known_asked;
        bool busy = false;           // an action is running
        std::uint64_t serial = 0;    // changes with every change
    };

    // `session_file`: where the session of this settings file is kept;
    // `discord_link_file`: where a Discord sign-in's tokens link the Discord
    // friends (platform::DiscordLinkFile), before it counts as signed in.
    CompetitiveService(std::filesystem::path session_file, std::filesystem::path discord_link_file);
    ~CompetitiveService();
    CompetitiveService(const CompetitiveService&) = delete;
    CompetitiveService& operator=(const CompetitiveService&) = delete;

    // The server to use, as online.profile_server has it; a different one
    // starts again (its own saved session, if any).
    void SetServer(const std::string& server);
    // Signed out: the Discord sign-in. Signed in with a Mii: connects
    // Discord to that profile (the sign-in with its session), which stays
    // signed in meanwhile.
    void SignIn();
    void TestSignIn(const std::string& name);
    // Without Discord: a new profile by `name` (1-32 characters), for the
    // Mii the player chose; a failure goes to Status::mii_error.
    void MiiSignIn(const std::string& name);
    void StopSignIn();
    void SignOut();
    // The player's Discord friends (DiscordFriends): who of them plays
    // Recharged is looked up now and then.
    void SetDiscordFriends(std::vector<std::string> discord_ids);
    // Signed out: asks which of these WFC profiles (the game save's Miis)
    // play for a profile already (Status::known_miis), once per list.
    void LookUpMiis(std::vector<std::uint32_t> profile_ids);
    // The Leaderboard page is shown (every frame it is): the leaderboards
    // are fetched now, and again once a minute while it stays.
    void WantLeaderboard();
    // Trades the Discord Social SDK's code (with its verifier and redirect)
    // or refresh token for its tokens; `done` gets them on the worker, or
    // the failure's words and whether Discord refused (the link has to be
    // made again). Dropped with a change of server or signing out.
    using DiscordTraded =
        std::function<void(const platform::DiscordTokens& tokens, const std::string& error, bool refused)>;
    void TradeDiscordToken(std::string code, std::string verifier, std::string redirect_uri,
                           std::string refresh_token, DiscordTraded done);
    // The set's Ready to play! (or taking it back): once more after its game
    // could not start.
    void ReadyToPlay(bool ready);
    void Accept();
    void Decline();
    void CancelMatch();
    Status Snapshot() const;

private:
    using Task = std::function<void(platform::ProfileService&)>;
    // Runs `task` on the worker with the API of the current server and
    // session, unless SetServer chose another server first; `action` shows
    // the page busy meanwhile.
    void Post(Task task, bool action);
    void Run();
    void Poll(platform::ProfileService& api);
    void Connect(platform::ProfileService& api);
    void Signed(platform::ProfileService& api, const std::string& token, const platform::ProfilePlayer& player);
    void Fail(const std::exception& error);
    void SignedOutHere(const char* reason);
    void Change(const std::function<void(Status&)>& change);

    const std::filesystem::path session_file_;
    const std::filesystem::path discord_link_file_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    struct Queued
    {
        std::uint64_t generation;
        Task task;
        bool action;
    };
    std::deque<Queued> tasks_;
    Status status_;
    std::string token_;
    std::vector<std::uint32_t> known_asking_; // LookUpMiis' list, asked or being asked
    std::uint64_t generation_ = 0; // SetServer starts a new one; older work is dropped
    std::uint64_t running_ = 0;    // the generation of the task the worker runs
    std::string sign_in_id_;       // the sign-in being waited for
    int poll_interval_ = 2;        // seconds between asks while signing in
    std::chrono::steady_clock::time_point next_poll_{};
    std::chrono::steady_clock::time_point next_me_{}, next_discord_{}, next_leaderboard_{};
    std::chrono::steady_clock::time_point leaderboard_wanted_{}; // the page's last frame
    std::vector<std::string> discord_ids_; // SetDiscordFriends
    std::shared_ptr<platform::HttpCancel> cancel_; // the running call's
    bool stop_ = false;
    std::thread worker_;
};
} // namespace mscharged::launcher
