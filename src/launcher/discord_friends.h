#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mscharged::launcher
{
// The player's Discord friends in the launcher (docs/COMPETITIVE.md, "Discord
// friends"), through the Discord Social SDK. The SDK is Discord's closed
// library, which only official builds ship beside the program. It is loaded
// when the player connects Discord, so a build or a computer without it simply
// has no Discord friends. Its own thread runs the SDK; the launcher reads
// Snapshot() every frame. The profile server trades the SDK's codes for
// tokens (Trade): the SDK can't hold the client secret.
class DiscordFriends
{
public:
    enum class State
    {
        Missing,    // no SDK beside the program, or it does not load (error says why)
        Off,        // nothing to link to: signed out, or a server without Discord sign-in
        NotLinked,  // Connect() links Discord
        Linking,    // Discord's authorization is open, in the Discord app or the browser
        Connecting, // trading tokens, or the SDK connecting to Discord
        Ready,
        Failed,     // error says why; Connect() tries again
    };

    enum class Presence
    {
        Online,
        Idle,
        DoNotDisturb,
        Offline,
    };

    struct Friend
    {
        std::string id;   // the Discord account id (digits)
        std::string name; // as Discord shows them
        Presence presence = Presence::Offline;
        bool in_recharged = false; // Discord shows them playing Recharged
    };

    struct Status
    {
        State state = State::Missing;
        std::string error;
        std::uint64_t error_serial = 0;
        std::vector<Friend> friends; // Ready: in Recharged first, then online, then by name
        std::uint64_t serial = 0;    // changes with every change
    };

    // What to link to: the profile server and its Discord application
    // (ProfileServerInfo::discord_application); an empty server when there
    // is nothing (signed out, a server without Discord sign-in).
    struct Account
    {
        std::string server;
        std::uint64_t application = 0;
        bool operator==(const Account& other) const
        {
            return server == other.server && application == other.application;
        }
        bool operator!=(const Account& other) const { return !(*this == other); }
    };

    // A trade with the profile server: the code of the SDK's authorization
    // with its verifier and redirect, or (only) a refresh token.
    struct TradeRequest
    {
        std::string code, verifier, redirect_uri, refresh_token;
    };
    struct TradeResult
    {
        std::string access_token, refresh_token; // both, when it worked
        std::string error;
        bool refused = false; // Discord refused: the link has to be made again
    };
    // Runs a trade and calls back with its result, on any thread, or never
    // when the account changes meanwhile.
    using Trade = std::function<void(TradeRequest request, std::function<void(TradeResult)> done)>;

    // `library_folder`: where the SDK is (beside the program); `link_file`:
    // where the link is kept between runs (platform::DiscordLinkFile).
    DiscordFriends(std::filesystem::path library_folder, std::filesystem::path link_file, Trade trade);
    ~DiscordFriends();
    DiscordFriends(const DiscordFriends&) = delete;
    DiscordFriends& operator=(const DiscordFriends&) = delete;

    void SetAccount(const Account& account);
    void Connect();
    void Cancel();     // stops a link waiting for Discord's authorization
    void Disconnect(); // forgets the link on this computer
    Status Snapshot() const;

    // The SDK's file name on this system.
    static const char* LibraryName();

private:
    struct Inbox;
    struct Sdk;
    struct Worker;
    void Run();

    const std::filesystem::path library_;
    const std::filesystem::path link_file_;
    const Trade trade_;
    std::shared_ptr<Inbox> inbox_; // commands and trade results, for the thread
    std::thread thread_;
};
} // namespace mscharged::launcher
