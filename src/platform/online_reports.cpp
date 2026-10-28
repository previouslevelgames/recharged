#include "platform/online_reports.h"

#include "platform/http_client.h"
#include "platform/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>

namespace mscharged::platform {
namespace {
// The server's names (its src/profiles/rules.rs), in the game's enum order:
// eTeamID, eSidekickID, and eStadiumID up to the Sand Tomb (the tutorial field
// and the low-poly stadium are no online stadiums).
constexpr const char* kCaptains[] = {"mario",   "bowser", "daisy", "donkey_kong", "luigi",      "peach",
                                     "waluigi", "wario",  "yoshi", "bowser_jr",   "diddy_kong", "petey"};
constexpr const char* kSidekicks[] = {"toad",      "koopa",      "hammer_bro", "birdo",
                                      "boo",       "dry_bones",  "monty_mole", "shy_guy"};
constexpr const char* kStadiums[] = {"battle_dome",      "bowser_stadium", "crater_field",    "crystal_canyon",
                                     "the_dump",         "galactic_stadium", "konga_coliseum", "lava_pit",
                                     "pipeline_central", "storm_ship",     "the_palace",      "thunder_island",
                                     "the_underground",  "the_vice",       "the_wastelands",  "sand_tomb"};
// The game's sync checks (ChargedOnlineGame::sync_check 1 to 3) and its
// session states (eNetworkSessionState), as the server names them.
constexpr const char* kSyncChecks[] = {"frame", "checksum", "seed"};
constexpr const char* kPhases[] = {"none", "login", "matchmaking", "prestart", "loading", "in_game", "game_end"};
// The server's paths: the match reports and the error codes.
constexpr const char* kReportsPath = "/api/v1/wfc/reports";
constexpr const char* kErrorsPath = "/api/v1/wfc/errors";
// A report not delivered is tried this many times.
constexpr int kAttempts = 4;

template <std::size_t N> const char* Name(const char* const (&names)[N], int index)
{
    return index >= 0 && index < int(N) ? names[index] : "unknown";
}

// The build as the server keeps it: printable, at most 64 characters.
void AppendBuild(std::string& out, const std::string& build)
{
    std::string name;
    for (const char c : build)
        if (c >= 0x20 && c < 0x7F && name.size() < 64) name += c;
    out += ",\"build\":";
    AppendJsonString(out, name);
}

void Captains(std::string& out, const int (&captains)[2])
{
    out += "{\"home\":";
    AppendJsonString(out, Name(kCaptains, captains[0]));
    out += ",\"away\":";
    AppendJsonString(out, Name(kCaptains, captains[1]));
    out += '}';
}

const char* Ending(int ending)
{
    switch (ending)
    {
    case 2: return "desync";
    case 4: return "disconnect";
    default: return "normal";
    }
}

// Why a series ended: decided, or what stopped it; null when the game did not
// say.
const char* Cause(const ChargedOnlineMatchSnapshot& s)
{
    if (s.ended == 1) return "decided";
    switch (s.stopped_by)
    {
    case 0: return "offline";
    case 2: return "desync";
    case 4: return "disconnect";
    default: return nullptr;
    }
}

std::string Body(const ChargedOnlineMatchSnapshot& s, const std::string& release, const std::string& build,
                 std::uint64_t series, const char* event, const ChargedOnlineGame* game, int number)
{
    const int side = s.side == 1 ? 1 : 0;
    std::string out = "{\"profile_id\":" + std::to_string(s.profile_id) + ",\"release\":";
    AppendJsonString(out, release);
    AppendBuild(out, build);
    out += ",\"event\":";
    AppendJsonString(out, event);
    out += ",\"series\":" + std::to_string(series);
    out += std::string(",\"mode\":\"") + (s.ranked ? "ranked" : "friends") + '"';
    out += ",\"best_of\":" + std::to_string(s.best_of);
    out += std::string(",\"side\":\"") + (side ? "away" : "home") + '"';
    out += ",\"mii\":";
    AppendJsonString(out, std::string_view(s.mii[side], strnlen(s.mii[side], sizeof s.mii[side])));
    out += ",\"opponent_mii\":";
    AppendJsonString(out, std::string_view(s.mii[1 - side], strnlen(s.mii[1 - side], sizeof s.mii[1 - side])));
    out += ",\"captains\":";
    Captains(out, s.captains);
    out += ",\"sidekicks\":{";
    for (int team = 0; team < 2; ++team)
    {
        out += team ? ",\"away\":[" : "\"home\":[";
        bool first = true;
        for (const int sidekick : s.sidekicks[team])
        {
            if (sidekick < 0 || sidekick >= int(std::size(kSidekicks))) continue;
            if (!first) out += ',';
            AppendJsonString(out, kSidekicks[sidekick]);
            first = false;
        }
        out += ']';
    }
    out += "},\"game\":";
    if (game)
    {
        out += "{\"number\":" + std::to_string(number) + ",\"score\":{\"home\":" + std::to_string(game->scores[0])
            + ",\"away\":" + std::to_string(game->scores[1]) + "},\"captains\":";
        Captains(out, game->captains);
        out += ",\"stadium\":";
        AppendJsonString(out, Name(kStadiums, game->stadium));
        out += ",\"ending\":";
        AppendJsonString(out, Ending(game->ending));
        const float seconds = game->seconds > 0.0f && game->seconds < 86400.0f ? game->seconds : 0.0f;
        out += ",\"seconds\":" + std::to_string(int(seconds + 0.5f));
        // Out of sync: which of the game's checks failed, on which frame.
        if (game->ending == 2 && game->sync_check >= 1 && game->sync_check <= int(std::size(kSyncChecks))
            && game->sync_frame >= 0)
        {
            out += ",\"sync\":{\"check\":";
            AppendJsonString(out, kSyncChecks[game->sync_check - 1]);
            out += ",\"frame\":" + std::to_string(game->sync_frame) + '}';
        }
        out += '}';
    }
    else
        out += "null";
    out += ",\"reason\":";
    if (game || std::string_view(event) != "end")
        out += "null";
    else
    {
        AppendJsonString(out, s.ended == 1 ? "decided" : "abandoned");
        if (const char* cause = Cause(s))
        {
            out += ",\"cause\":";
            AppendJsonString(out, cause);
        }
    }
    out += '}';
    return out;
}
} // namespace

std::vector<std::string> OnlineMatchEvents(const ChargedOnlineMatchSnapshot* last,
                                           const ChargedOnlineMatchSnapshot& now, const std::string& release,
                                           const std::string& build, std::uint64_t series)
{
    std::vector<std::string> events;
    // No profile (the Mii never logged in): the server could not tell whose
    // report it was.
    if (!now.series || !now.profile_id) return events;
    const bool same = last && last->series == now.series;
    if (!same) events.push_back(Body(now, release, build, series, "start", nullptr, 0));
    const int games = std::clamp(now.games, 0, int(std::size(now.game)));
    for (int n = same ? std::clamp(last->games, 0, games) : 0; n < games; ++n)
        events.push_back(Body(now, release, build, series, "game", &now.game[n], n + 1));
    if (now.ended && !(same && last->ended)) events.push_back(Body(now, release, build, series, "end", nullptr, 0));
    return events;
}

std::string OnlineErrorReport(const ChargedOnlineError& error, const std::string& release, const std::string& build)
{
    if (error.code < 1 || error.code > 999999) return {};
    std::string out = "{\"release\":";
    AppendJsonString(out, release);
    AppendBuild(out, build);
    out += ",\"code\":" + std::to_string(error.code);
    if (error.phase >= 0 && error.phase < int(std::size(kPhases)))
    {
        out += ",\"phase\":";
        AppendJsonString(out, kPhases[error.phase]);
    }
    if (error.profile_id) out += ",\"profile_id\":" + std::to_string(error.profile_id);
    out += '}';
    return out;
}

struct OnlineReports::State {
    // A body and the path on the server it goes to.
    struct Post
    {
        const char* path;
        std::string body;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Post> queue;
    bool stop = false;
    std::shared_ptr<HttpCancel> cancel = std::make_shared<HttpCancel>();
};

std::unique_ptr<OnlineReports> OnlineReports::Start(const std::string& server, const std::string& release,
                                                    const std::string& build, Log log)
{
    if (server.empty()) return nullptr;
    Send send = HttpSend(server, log);
    return std::make_unique<OnlineReports>(release, build, std::move(send), std::move(log));
}

OnlineReports::Send OnlineReports::HttpSend(const std::string& server, Log log, Request request)
{
    if (!request) request = [](const HttpRequest& http) { return HttpGet(http); };
    // A server without match reports (another WFC server) answers 404, and
    // one from before the error codes does for those: no more of them this
    // run.
    struct Endpoints
    {
        std::atomic<bool> reports{false};
        std::atomic<bool> errors{false};
    };
    auto off = std::make_shared<Endpoints>();
    return [server, log = std::move(log), request = std::move(request), off](
               const std::string& path, const std::string& body, const std::shared_ptr<HttpCancel>& cancel) {
        const bool errors = path == kErrorsPath;
        std::atomic<bool>& unsupported = errors ? off->errors : off->reports;
        if (unsupported) return true;
        HttpRequest http;
        http.url = "http://" + server + path;
        http.method = "POST";
        http.body = body;
        // Plain http like the game's own WFC requests (its NAS login and
        // gamestats) to the same server.
        http.allow_http = true;
        http.headers = {"Content-Type: application/json"};
        http.connect_timeout = 10;
        http.stall_timeout = 15;
        http.max_size = 64u << 10;
        http.cancel = cancel;
        try
        {
            const HttpResponse response = request(http);
            // A report is worth another try once the server is back; an
            // error code is not (the server answered).
            if (response.status >= 500 && !errors) return false;
            if (response.status == 404 || response.status == 405)
            {
                unsupported = true;
                if (log)
                    log(std::string("Match reports: ") + (errors ? "error codes off" : "off")
                        + ", the server takes none (HTTP " + std::to_string(response.status) + ")");
            }
            else if (response.status != 200 && log)
                log(std::string("Match reports: the server refused ") + (errors ? "an error code" : "a report")
                    + " (HTTP " + std::to_string(response.status) + "): " + response.body.substr(0, 200));
            return true;
        }
        catch (const HttpError& error)
        {
            if (!error.Cancelled() && log) log(std::string("Match reports: ") + error.what());
            return false;
        }
    };
}

OnlineReports::OnlineReports(std::string release, std::string build, Send send, Log log,
                             std::chrono::milliseconds retry)
    : state_(std::make_shared<State>()), release_(std::move(release)), build_(std::move(build))
{
    thread_ = std::thread([state = state_, send = std::move(send), log = std::move(log), retry] {
        std::unique_lock lock(state->mutex);
        for (;;)
        {
            state->wake.wait(lock, [&] { return state->stop || !state->queue.empty(); });
            if (state->stop) return;
            const State::Post post = state->queue.front();
            bool delivered = false;
            for (int attempt = 0; attempt < kAttempts && !delivered; ++attempt)
            {
                if (attempt && state->wake.wait_for(lock, retry, [&] { return state->stop; })) return;
                lock.unlock();
                delivered = send(post.path, post.body, state->cancel);
                lock.lock();
                if (state->stop) return;
            }
            if (!delivered && log) log("Match reports: a report did not get through and was dropped");
            state->queue.pop_front();
        }
    });
}

OnlineReports::~OnlineReports()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_all();
    state_->cancel->Cancel();
    thread_.join();
}

void OnlineReports::Observe(const ChargedOnlineMatchSnapshot& snapshot)
{
    // Each series gets a number of its own from its start time, so a series
    // after the game restarts never takes an earlier one's.
    if (snapshot.series && (!seen_ || snapshot.series != last_.series))
    {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        series_ = std::max<std::uint64_t>(std::uint64_t(std::max<long long>(now, 0)), series_ + 1);
    }
    const auto events = OnlineMatchEvents(seen_ ? &last_ : nullptr, snapshot, release_, build_, series_);
    // An error code shown since the last look (none before the first).
    const std::string error = snapshot.error.shown != last_.error.shown
        ? OnlineErrorReport(snapshot.error, release_, build_)
        : std::string();
    last_ = snapshot;
    seen_ = true;
    if (events.empty() && error.empty()) return;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& event : events) state_->queue.push_back({kReportsPath, event});
        if (!error.empty()) state_->queue.push_back({kErrorsPath, error});
    }
    state_->wake.notify_all();
}
} // namespace mscharged::platform
