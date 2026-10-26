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
// A report not delivered is tried this many times.
constexpr int kAttempts = 4;

template <std::size_t N> const char* Name(const char* const (&names)[N], int index)
{
    return index >= 0 && index < int(N) ? names[index] : "unknown";
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

std::string Body(const ChargedOnlineMatchSnapshot& s, const std::string& release, std::uint64_t series,
                 const char* event, const ChargedOnlineGame* game, int number)
{
    const int side = s.side == 1 ? 1 : 0;
    std::string out = "{\"profile_id\":" + std::to_string(s.profile_id) + ",\"release\":";
    AppendJsonString(out, release);
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
        out += ",\"seconds\":" + std::to_string(int(seconds + 0.5f)) + '}';
    }
    else
        out += "null";
    out += ",\"reason\":";
    if (game || std::string_view(event) != "end")
        out += "null";
    else
        AppendJsonString(out, s.ended == 1 ? "decided" : "abandoned");
    out += '}';
    return out;
}
} // namespace

std::vector<std::string> OnlineMatchEvents(const ChargedOnlineMatchSnapshot* last,
                                           const ChargedOnlineMatchSnapshot& now, const std::string& release,
                                           std::uint64_t series)
{
    std::vector<std::string> events;
    // No profile (the Mii never logged in): the server could not tell whose
    // report it was.
    if (!now.series || !now.profile_id) return events;
    const bool same = last && last->series == now.series;
    if (!same) events.push_back(Body(now, release, series, "start", nullptr, 0));
    const int games = std::clamp(now.games, 0, int(std::size(now.game)));
    for (int n = same ? std::clamp(last->games, 0, games) : 0; n < games; ++n)
        events.push_back(Body(now, release, series, "game", &now.game[n], n + 1));
    if (now.ended && !(same && last->ended)) events.push_back(Body(now, release, series, "end", nullptr, 0));
    return events;
}

struct OnlineReports::State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    bool stop = false;
    std::shared_ptr<HttpCancel> cancel = std::make_shared<HttpCancel>();
};

std::unique_ptr<OnlineReports> OnlineReports::Start(const std::string& server, const std::string& release, Log log)
{
    if (server.empty()) return nullptr;
    const std::string url = "http://" + server + "/api/v1/wfc/reports";
    // A server without match reports (another WFC server) answers 404: no
    // more reports this run.
    auto unsupported = std::make_shared<std::atomic<bool>>(false);
    Send send = [url, log, unsupported](const std::string& body, const std::shared_ptr<HttpCancel>& cancel) {
        if (*unsupported) return true;
        HttpRequest request;
        request.url = url;
        request.method = "POST";
        request.body = body;
        // Plain http like the game's own WFC requests (its NAS login and
        // gamestats) to the same server.
        request.allow_http = true;
        request.headers = {"Content-Type: application/json"};
        request.connect_timeout = 10;
        request.stall_timeout = 15;
        request.max_size = 64u << 10;
        request.cancel = cancel;
        try
        {
            const HttpResponse response = HttpGet(request);
            if (response.status >= 500) return false;
            if (response.status == 404 || response.status == 405)
            {
                *unsupported = true;
                if (log)
                    log("Match reports: off, the server takes none (HTTP " + std::to_string(response.status) + ")");
            }
            else if (response.status != 200 && log)
                log("Match reports: the server refused a report (HTTP " + std::to_string(response.status)
                    + "): " + response.body.substr(0, 200));
            return true;
        }
        catch (const HttpError& error)
        {
            if (!error.Cancelled() && log) log(std::string("Match reports: ") + error.what());
            return false;
        }
    };
    return std::make_unique<OnlineReports>(release, std::move(send), std::move(log));
}

OnlineReports::OnlineReports(std::string release, Send send, Log log, std::chrono::milliseconds retry)
    : state_(std::make_shared<State>()), release_(std::move(release))
{
    thread_ = std::thread([state = state_, send = std::move(send), log = std::move(log), retry] {
        std::unique_lock lock(state->mutex);
        for (;;)
        {
            state->wake.wait(lock, [&] { return state->stop || !state->queue.empty(); });
            if (state->stop) return;
            const std::string body = state->queue.front();
            bool delivered = false;
            for (int attempt = 0; attempt < kAttempts && !delivered; ++attempt)
            {
                if (attempt && state->wake.wait_for(lock, retry, [&] { return state->stop; })) return;
                lock.unlock();
                delivered = send(body, state->cancel);
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
    const auto events = OnlineMatchEvents(seen_ ? &last_ : nullptr, snapshot, release_, series_);
    last_ = snapshot;
    seen_ = true;
    if (events.empty()) return;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& event : events) state_->queue.push_back(event);
    }
    state_->wake.notify_all();
}
} // namespace mscharged::platform
