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
// rule sets and current match, and challenges, answers and cancellations.
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
        platform::ProfilePlayer player;
        std::vector<platform::RuleSet> rule_sets;
        std::optional<platform::ProfileMatch> match;
        platform::PlayerLists lists; // the lobby's: online now, recent opponents, friends
        // The conversation open in the chat panel (OpenChat): with whom (0:
        // none), the messages so far (oldest first), whether the first fetch
        // came, whether the two can write, and a message on its way.
        struct Chat
        {
            std::int64_t with = 0;
            std::vector<platform::ChatMessage> messages;
            bool loaded = false, can_send = false, sending = false;
        } chat;
        bool busy = false;           // an action is running
        std::uint64_t serial = 0;    // changes with every change
    };

    // `session_file`: where the session of this settings file is kept.
    explicit CompetitiveService(std::filesystem::path session_file);
    ~CompetitiveService();
    CompetitiveService(const CompetitiveService&) = delete;
    CompetitiveService& operator=(const CompetitiveService&) = delete;

    // The server to use, as online.profile_server has it; a different one
    // starts again (its own saved session, if any).
    void SetServer(const std::string& server);
    void SignIn();
    void TestSignIn(const std::string& name);
    void StopSignIn();
    void SignOut();
    void Challenge(const std::string& opponent, std::int64_t rule_set);
    void ChallengePlayer(std::int64_t opponent_id, std::int64_t rule_set);
    // The friends list: add a player (from the lists, or by Discord
    // username) or take one off.
    void AddFriend(std::int64_t player_id);
    void AddFriend(const std::string& name);
    void RemoveFriend(std::int64_t player_id);
    // The chat panel: the conversation with a friend, followed every two
    // seconds while open, and a message to them.
    void OpenChat(std::int64_t player_id);
    void CloseChat();
    void SendChat(const std::string& body);
    // The set's Ready to play! (or taking it back), and playing it again.
    void ReadyToPlay(bool ready);
    void Rematch();
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
    std::uint64_t generation_ = 0; // SetServer starts a new one; older work is dropped
    std::uint64_t running_ = 0;    // the generation of the task the worker runs
    std::string sign_in_id_;       // the sign-in being waited for
    int poll_interval_ = 2;        // seconds between asks while signing in
    std::chrono::steady_clock::time_point next_poll_{};
    std::chrono::steady_clock::time_point next_me_{}, next_lists_{}, next_chat_{};
    std::int64_t chat_seen_ = 0; // the last message a fetch of the open conversation had
    std::shared_ptr<platform::HttpCancel> cancel_; // the running call's
    bool stop_ = false;
    std::thread worker_;
};
} // namespace mscharged::launcher
