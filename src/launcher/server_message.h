#pragma once

#include "platform/http_client.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace mscharged::launcher
{
// The WFC server's message for the Play page (a restart tonight, a tournament
// on Sunday), which its admins post on the dashboard's Maintenance page:
// GET <online.profile_server>/api/v1/message (Recharged WFC 1.2.25 and
// later), asked once when the launcher starts. The player closes it on the
// Play page ([launcher] dismissed_message) until the server posts another.
struct ServerMessage
{
    std::int64_t id = 0;  // new for every post
    std::string markdown; // headings, paragraphs, lists, **bold**, `code`, links
    bool warning = false; // its tone: a warning, else information
    std::optional<std::int64_t> ends_at; // unix seconds; none: until the server takes it away
};

// The server's answer, {"message": null} or {"message": {...}}: the message,
// or none. `readable` (when given) says whether the answer had that shape.
std::optional<ServerMessage> ParseServerMessage(std::string_view body, bool* readable = nullptr);
// An RFC 3339 time ("2026-11-21T23:00:00Z", with fractions of a second or an
// offset) in unix seconds; none for anything else.
std::optional<std::int64_t> ParseRfc3339(std::string_view text);
// Whether `message` still shows at `now` (unix seconds).
bool ServerMessageCurrent(const ServerMessage& message, std::int64_t now);

// The one ask, on a thread of its own: its answer once it came (or failed).
class ServerMessageFetch
{
public:
    using Log = std::function<void(const std::string& line)>;
    // One HTTP request (HttpGet; a test answers for the server).
    using Request = std::function<platform::HttpResponse(const platform::HttpRequest& request)>;

    // `server`: the profile server as ProfileServerAddress returns it.
    ServerMessageFetch(std::string server, Log log, Request request = {});
    // Stops the ask under way.
    ~ServerMessageFetch();
    ServerMessageFetch(const ServerMessageFetch&) = delete;
    ServerMessageFetch& operator=(const ServerMessageFetch&) = delete;

    // True once the ask is over; `message` is then the server's message, if
    // it has one.
    bool Done(std::optional<ServerMessage>* message = nullptr) const;

private:
    void Run();

    const std::string server_;
    const Log log_;
    const Request request_;
    const std::shared_ptr<platform::HttpCancel> cancel_ = std::make_shared<platform::HttpCancel>();
    mutable std::mutex mutex_;
    bool done_ = false;
    std::optional<ServerMessage> message_;
    std::thread thread_;
};
} // namespace mscharged::launcher
