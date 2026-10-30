#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged::platform
{
class HttpCancel;

// The profile service of a Recharged WFC server (recharged-wfc-srv,
// docs/profiles.md): players who signed in with Discord, the friend codes
// their games proved theirs, the rule sets the server's admins keep, and test
// matches between two players. The launcher signs in and arranges matches;
// the game reports its login, readiness and results. Every call blocks.

// A server address as typed ("https://wfc.example.com", a trailing slash
// allowed) in its canonical form, or nullopt with `error` saying what is
// wrong. http:// is accepted only for this computer or a private network
// (a test server), as the token would otherwise cross the internet readable.
std::optional<std::string> ProfileServerAddress(std::string_view text, std::string* error = nullptr);

struct WfcProfile
{
    std::int64_t profile_id = 0;
    std::string game; // R4QE or R4QP
    std::string friend_code;
    std::string mii;
};

struct ProfilePlayer
{
    std::int64_t id = 0;
    std::string name;    // as shown: the Discord display name at the last sign-in
    std::string discord; // the Discord username others challenge
    std::string avatar_url;
    bool test = false; // a test server's player, signed in by name
    std::vector<WfcProfile> wfc; // the latest first
    // GET /api/v1/me only: the Discord account id (digits; empty for test
    // players), which never changes: the player's Mii is made from it.
    std::string discord_id;
    // In the lobby's lists: Recharged is open, and a set to play; when they
    // last had it open (Unix seconds); the player keeps them as a friend.
    bool online = false, busy = false;
    // While online, what their Recharged is doing: "launcher", "game" (the
    // game, outside a match) or "match" (a match in the game: nobody can
    // challenge them); empty from servers before 1.2.0.
    std::string activity;
    std::optional<std::int64_t> last_online;
    bool is_friend = false;
    // Both keep each other as friends: they can write each other; their
    // messages not read yet. With chat, discord_id is theirs (empty for test
    // players).
    bool chat = false;
    int unread = 0;
};

// A message between two friends (GET /api/v1/chat/<id>).
struct ChatMessage
{
    std::int64_t id = 0;
    bool mine = false; // sent by this player
    std::string body;
    std::int64_t sent_at = 0; // Unix seconds
};

struct ChatPage
{
    std::vector<ChatMessage> messages; // oldest first
    bool can_send = false;
};

// The lobby's lists (GET /api/v1/players).
struct PlayerLists
{
    std::vector<ProfilePlayer> online; // who has Recharged open, by name
    std::vector<ProfilePlayer> recent; // the last players a set was played with
    // The player's friends: online first, by name, then the last online first.
    std::vector<ProfilePlayer> friends;
};

// When a player last had Recharged open, for the friends list: "Last online 5
// minutes ago", "... yesterday", "... on 3 October"; `now` and `when` in Unix
// seconds.
std::string DescribeLastOnline(std::int64_t now, std::optional<std::int64_t> when);

// When a chat message was sent, in local time: "14:05" today, "Yesterday
// 14:05", "3 October, 14:05", or with the year when it was another.
std::string DescribeChatTime(std::int64_t now, std::int64_t when);

// The rules a match is played by (docs/profiles.md, "Rules").
struct MatchRules
{
    std::vector<std::string> tweaks; // fast_terrain, no_mega_strikes, thunder_island_walls
    std::string skill;               // training, rookie, professional, superstar, legend
    std::string limit;               // time or goals
    int time = 0;                    // seconds
    int goals = 0;
    int games = 0;       // in the series: 1, 3, 5 or 7
    std::string stadium; // random, or a stadium's name
};

struct RuleSet
{
    std::int64_t id = 0;
    std::string name;
    MatchRules rules;
    std::string hash;
};

// One game of a series, as one side saw it.
struct MatchGame
{
    int my_score = 0;
    int opponent_score = 0;
    std::string my_captain;
    std::string opponent_captain;
    std::string stadium;
};

// A match as one of its players sees it.
struct ProfileMatch
{
    std::int64_t id = 0;
    // challenged, accepted, declined, cancelled, expired, finished or disputed.
    std::string state;
    std::string side; // challenger or challenged
    ProfilePlayer opponent;
    std::string rule_set;
    MatchRules rules;
    std::string rules_hash;
    bool paired = false;   // both games are ready for it
    bool reported = false; // this player's game reported the result
    std::string winner;    // you, opponent or draw once finished
    std::vector<MatchGame> games; // this player's report
    std::int64_t created_at = 0;
    // Where the set is (docs/profiles.md of recharged-wfc-srv, "Sets"):
    // invited, open, go, playing, finished, disputed or dissolved (then
    // `reason` is the state). Empty from servers before sets.
    std::string stage, reason;
    bool you_ready = false, opponent_ready = false; // Ready to play!
    bool opponent_online = false; // the opponent has Recharged open
    std::int64_t go_at = 0, playing_at = 0; // unix seconds, 0: not yet
    std::int64_t rematch_of = 0; // the set this one plays again

    // Still to be played: challenged or accepted.
    bool Open() const { return state == "challenged" || state == "accepted"; }
};

struct ProfileServerInfo
{
    std::string version;
    bool discord = false; // Discord sign-in
    bool test = false;    // sign-in by name (test servers)
};

struct SignInStart
{
    std::string id;
    std::string code; // shown in the launcher and on the browser page
    std::string url;  // the page the browser opens
    int expires_in = 600;
    int poll_interval = 2;
};

struct SignInResult
{
    enum class Status { Waiting, SignedIn, Refused, Expired };
    Status status = Status::Waiting;
    std::string token;
    ProfilePlayer player;
    std::string message; // why it was refused
};

struct ReadyAnswer
{
    bool paired = false;
    bool other_release = false; // both are ready, but on releases that can't play each other
};

// A call the server refused (Status() its HTTP status, Message() its words)
// or one that got no answer (Status() 0).
class ProfileError : public std::runtime_error
{
public:
    ProfileError(const std::string& message, long status, bool cancelled = false)
        : std::runtime_error(message), status_(status), cancelled_(cancelled) {}
    long Status() const { return status_; }
    bool SignedOut() const { return status_ == 401; }
    bool Cancelled() const { return cancelled_; }

private:
    long status_;
    bool cancelled_;
};

class ProfileService
{
public:
    // `server` as ProfileServerAddress returns it.
    explicit ProfileService(std::string server, std::string token = {});

    const std::string& Server() const { return server_; }
    void SetToken(std::string token) { token_ = std::move(token); }
    // Lets another thread stop the running call.
    void SetCancel(std::shared_ptr<HttpCancel> cancel) { cancel_ = std::move(cancel); }
    // The game's requests say what it is doing ("game" or "match", the
    // Recharged-Activity header); the launcher's say nothing.
    void SetActivity(std::string activity) { activity_ = std::move(activity); }

    // Without a session.
    ProfileServerInfo Info();
    SignInStart StartSignIn();
    SignInResult PollSignIn(const std::string& id);
    SignInResult TestSignIn(const std::string& name);
    std::vector<RuleSet> RuleSets();

    // With the session's token.
    ProfilePlayer Me();
    void SignOut();
    ProfileMatch Challenge(const std::string& opponent, std::int64_t rule_set);
    // The same by the player's id, from the lobby's lists.
    ProfileMatch ChallengePlayer(std::int64_t opponent_id, std::int64_t rule_set);
    // The friends after adding one (by player id or Discord username) or
    // taking one off.
    std::vector<ProfilePlayer> AddFriend(std::int64_t player_id);
    std::vector<ProfilePlayer> AddFriend(const std::string& name);
    std::vector<ProfilePlayer> RemoveFriend(std::int64_t player_id);
    // The messages with `player_id`, those after message `after` (0: the
    // latest 50); a message to them.
    ChatPage Chat(std::int64_t player_id, std::int64_t after);
    ChatMessage SendChat(std::int64_t player_id, const std::string& body);
    // The set still to be played, or the last one that ended; `poll_interval`
    // gets how soon (seconds) the server wants to be asked again.
    std::optional<ProfileMatch> CurrentMatch(int* poll_interval = nullptr);
    PlayerLists Players();
    // The launcher's Ready to play! (or taking it back).
    ProfileMatch ReadyToPlay(std::int64_t match, bool ready);
    // The ended set again: the same opponent and rules.
    ProfileMatch Rematch(std::int64_t match);
    ProfileMatch Accept(std::int64_t match);
    ProfileMatch Decline(std::int64_t match);
    ProfileMatch Cancel(std::int64_t match);
    // The game's proof that it is logged in to WFC as `profile_id`.
    WfcProfile ProveWfcLogin(std::int64_t profile_id, const std::string& login_ticket);
    ReadyAnswer Ready(std::int64_t match, std::int64_t profile_id, const std::string& rules_hash);
    ProfileMatch Report(std::int64_t match, const std::vector<MatchGame>& games);

private:
    struct Answer;
    Answer Call(const char* method, const std::string& path, const std::string* body);

    std::string server_;
    std::string token_;
    std::string activity_;
    std::shared_ptr<HttpCancel> cancel_;
};

// The session a settings file signed in to, kept between runs: the server it
// belongs to (a token is never sent to another), the token and the player.
struct SavedProfileSession
{
    std::string server;
    std::string token;
    std::string player_name;
};

// <data folder>/online/<settings file name>.session: two settings files (a
// second game on one computer) sign in separately.
std::filesystem::path ProfileSessionFile(const std::filesystem::path& data_folder,
                                         const std::filesystem::path& config);
std::optional<SavedProfileSession> LoadProfileSession(const std::filesystem::path& file);
// Readable by this user only, where the system has such permissions.
void SaveProfileSession(const std::filesystem::path& file, const SavedProfileSession& session);
void ForgetProfileSession(const std::filesystem::path& file);
} // namespace mscharged::platform
