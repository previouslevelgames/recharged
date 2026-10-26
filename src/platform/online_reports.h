#pragma once

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
// trusts them (the address the profile's WFC login came from). A report that
// does not get through is tried again a few times, then dropped.
namespace mscharged::platform {
class HttpCancel;

// The report bodies (JSON) of what `now` adds to `last` (null: nothing seen
// yet) for the game `release` (R4QE, R4QP...). A new series number begins a
// series: its start, then any game and end it already has. `series` is the
// number the reports give it (unique among this player's series).
std::vector<std::string> OnlineMatchEvents(const ChargedOnlineMatchSnapshot* last,
                                           const ChargedOnlineMatchSnapshot& now, const std::string& release,
                                           std::uint64_t series);

class OnlineReports {
public:
    // Delivers one body: true when the server answered (accepted it or
    // refused it for good), false when it should be tried again. `cancel`
    // stops the request when the reports end.
    using Send = std::function<bool(const std::string& body, const std::shared_ptr<HttpCancel>& cancel)>;
    using Log = std::function<void(const std::string& line)>;

    // Reports to `server` (online.server: a host name or address).
    static std::unique_ptr<OnlineReports> Start(const std::string& server, const std::string& release, Log log);
    // Reports through `send`, a failed one tried again after `retry`.
    OnlineReports(std::string release, Send send, Log log,
                  std::chrono::milliseconds retry = std::chrono::seconds(5));
    // Stops the report under way; reports not sent yet are dropped.
    ~OnlineReports();
    OnlineReports(const OnlineReports&) = delete;
    OnlineReports& operator=(const OnlineReports&) = delete;

    // After a game frame, on the game's thread (once a second is enough).
    // Never waits for the network.
    void Observe(const ChargedOnlineMatchSnapshot& snapshot);

private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread thread_;
    std::string release_;
    bool seen_ = false;
    ChargedOnlineMatchSnapshot last_{};
    std::uint64_t series_ = 0;
};
} // namespace mscharged::platform
