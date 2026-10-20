#pragma once

#include "platform/discord_ipc.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace mscharged::platform
{
// The Discord status (issue #71) on a thread of its own, so the game never
// waits for Discord: it looks for the Discord app every `retry` while the
// app is not running and sends the newest status at most every
// `min_interval` (Discord drops updates that come faster). Destroying it
// clears the status.
class DiscordPresence
{
public:
    using Connect = std::function<std::unique_ptr<discord::Connection>()>;
    using Log = std::function<void(const std::string&)>;
    struct Options
    {
        std::string application_id;
        Connect connect = discord::ConnectToDiscord;
        Log log; // connection changes and refusals, once each
        // Each status Discord accepted (nothing: cleared), for verbose logs.
        std::function<void(const std::optional<discord::Activity>&)> shown;
        std::chrono::milliseconds min_interval{15000};
        std::chrono::milliseconds retry{15000};
        std::chrono::milliseconds reply_timeout{5000};
    };

    explicit DiscordPresence(Options options);
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    // The status to show from now on (nothing: none). Only the newest counts.
    void Show(std::optional<discord::Activity> activity);

private:
    void Run();
    std::unique_ptr<discord::Connection> Handshake();
    bool Send(discord::Connection& connection, const std::optional<discord::Activity>& activity);
    // Answers a ping; false for a frame that closes the connection.
    bool Service(discord::Connection& connection, const discord::Frame& frame);
    void Note(const std::string& line);

    Options options_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<discord::Activity> wanted_;
    bool changed_ = false;
    bool stop_ = false;
    unsigned nonce_ = 0;
    std::string last_note_;
    std::thread thread_;
};
}
