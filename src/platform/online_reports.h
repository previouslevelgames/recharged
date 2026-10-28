#pragma once

#include "platform/http_client.h"
#include "platform/online_match_snapshot.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The WFC server's match reports: the host turns the game module's online
// series (online_match_snapshot.h) into the events the server pairs between
// both players' games: a series began, a game ended, the series ended. They
// go to the server the game logs in to, http://<online.server>/api/v1/wfc/reports,
// from a thread of their own and from this computer, which is how the server
// trusts them (the address the profile's WFC login came from). The error
// codes the game shows its player go to /api/v1/wfc/errors the same way. A
// report that does not get through is tried again a few times, then dropped.
namespace mscharged::platform {
// The report bodies (JSON) of what `now` adds to `last` (null: nothing seen
// yet) for the game `release` (R4QE, R4QP...) and the program `build` (its
// version). A new series number begins a series: its start, then any game
// and end it already has. `series` is the number the reports give it (unique
// among this player's series).
std::vector<std::string> OnlineMatchEvents(const ChargedOnlineMatchSnapshot* last,
                                           const ChargedOnlineMatchSnapshot& now, const std::string& release,
                                           const std::string& build, std::uint64_t series);

// The body (JSON) of an error code the game showed; empty for a code the
// server takes none of (only popups' codes, 1 to 999999).
std::string OnlineErrorReport(const ChargedOnlineError& error, const std::string& release, const std::string& build);

class OnlineReports {
public:
    // Delivers one body to `path` on the server (/api/v1/wfc/reports or
    // /api/v1/wfc/errors): true when the server answered (accepted it or
    // refused it for good), false when it should be tried again. `cancel`
    // stops the request when the reports end.
    using Send = std::function<bool(const std::string& path, const std::string& body,
                                    const std::shared_ptr<HttpCancel>& cancel)>;
    using Log = std::function<void(const std::string& line)>;
    // One HTTP request (HttpGet; a test answers for the server).
    using Request = std::function<HttpResponse(const HttpRequest& request)>;

    // Reports to `server` (online.server: a host name or address).
    static std::unique_ptr<OnlineReports> Start(const std::string& server, const std::string& release,
                                                const std::string& build, Log log);
    // How Start delivers: a POST to http://<server><path> through `request`
    // (HttpGet when empty). Any answer delivers an error code; a report is
    // tried again after a server error (5xx). A path the server has no
    // endpoint for (404, 405: another or an older server) sends no more this run.
    static Send HttpSend(const std::string& server, Log log, Request request = {});
    // Reports through `send`, a failed one tried again after `retry`.
    OnlineReports(std::string release, std::string build, Send send, Log log,
                  std::chrono::milliseconds retry = std::chrono::seconds(5));
    // Stops the report under way; reports not sent yet are dropped.
    ~OnlineReports();
    OnlineReports(const OnlineReports&) = delete;
    OnlineReports& operator=(const OnlineReports&) = delete;

    // After a game frame, on the game's thread (once a second is enough).
    // Never waits for the network. Of several error codes shown between two
    // calls, only the last one is reported.
    void Observe(const ChargedOnlineMatchSnapshot& snapshot);

private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread thread_;
    std::string release_;
    std::string build_;
    bool seen_ = false;
    ChargedOnlineMatchSnapshot last_{};
    std::uint64_t series_ = 0;
};
} // namespace mscharged::platform
