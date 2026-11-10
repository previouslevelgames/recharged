#include "platform/online_reports.h"

#include "platform/desync_dossier.h"
#include "platform/host_os.h"
#include "platform/http_client.h"
#include "platform/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

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
constexpr const char* kDossiersPath = "/api/v1/wfc/desyncs";
// A report not delivered is tried this many times.
constexpr int kAttempts = 4;
// The desync dossiers kept: the newest ones, for at most as many days as the
// server keeps them.
constexpr int kKeptDossiers = 20;
constexpr auto kDossierDays = std::chrono::hours(24 * 30);
constexpr const char* kDossierExtension = ".rcds";

// A dossier's header, when `bytes` holds one of this version that fits.
bool DossierHeader(const std::string& bytes, DesyncDossierHeader& header)
{
    if (bytes.size() < sizeof header || bytes.size() > kDesyncDossierMaxBytes) return false;
    std::memcpy(&header, bytes.data(), sizeof header);
    return std::memcmp(header.magic, kDesyncDossierMagic, sizeof header.magic) == 0
        && header.version == kDesyncDossierVersion;
}

// `text` in a fixed-size field, NUL-padded (cut when longer).
template <std::size_t N> void Fixed(char (&field)[N], const std::string& text)
{
    std::memset(field, 0, N);
    std::memcpy(field, text.data(), std::min(text.size(), N));
}

std::string Utf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string bytes;
    char chunk[4096];
    while (in && bytes.size() <= kDesyncDossierMaxBytes)
    {
        in.read(chunk, sizeof chunk);
        bytes.append(chunk, std::size_t(in.gcount()));
    }
    return bytes;
}

// The folder's dossiers, newest first; those past kDossierDays and past the
// kKeptDossiers newest are deleted.
std::vector<std::filesystem::path> KeptDossiers(const std::filesystem::path& folder)
{
    std::error_code error;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
    {
        std::error_code ignored;
        if (it->path().extension() == kDossierExtension && it->is_regular_file(ignored))
            found.emplace_back(it->last_write_time(ignored), it->path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const auto oldest = std::filesystem::file_time_type::clock::now() - kDossierDays;
    std::vector<std::filesystem::path> kept;
    for (const auto& [written, path] : found)
    {
        std::error_code ignored;
        if (written < oldest || int(kept.size()) == kKeptDossiers)
            std::filesystem::remove(path, ignored);
        else
            kept.push_back(path);
    }
    return kept;
}

template <std::size_t N> const char* Name(const char* const (&names)[N], int index)
{
    return index >= 0 && index < int(N) ? names[index] : "unknown";
}

// A diagnostic as the server keeps it: printable, at most 64 characters.
std::string Printable(const std::string& text)
{
    std::string name;
    for (const char c : text)
        if (c >= 0x20 && c < 0x7F && name.size() < 64) name += c;
    return name;
}

void AppendBuild(std::string& out, const std::string& build)
{
    out += ",\"build\":";
    AppendJsonString(out, Printable(build));
}

// The operating system (HostOsName), left out when unknown.
void AppendOs(std::string& out, const std::string& os)
{
    const std::string name = Printable(os);
    if (name.empty()) return;
    out += ",\"os\":";
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
                 const std::string& os, std::uint64_t series, const char* event, const ChargedOnlineGame* game,
                 int number)
{
    const int side = s.side == 1 ? 1 : 0;
    std::string out = "{\"profile_id\":" + std::to_string(s.profile_id) + ",\"release\":";
    AppendJsonString(out, release);
    AppendBuild(out, build);
    AppendOs(out, os);
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
                                           const std::string& build, const std::string& os,
                                           std::uint64_t series)
{
    std::vector<std::string> events;
    // No profile (the Mii never logged in): the server could not tell whose
    // report it was.
    if (!now.series || !now.profile_id) return events;
    const bool same = last && last->series == now.series;
    if (!same) events.push_back(Body(now, release, build, os, series, "start", nullptr, 0));
    const int games = std::clamp(now.games, 0, int(std::size(now.game)));
    for (int n = same ? std::clamp(last->games, 0, games) : 0; n < games; ++n)
        events.push_back(Body(now, release, build, os, series, "game", &now.game[n], n + 1));
    if (now.ended && !(same && last->ended)) events.push_back(Body(now, release, build, os, series, "end", nullptr, 0));
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
    // A body and the path on the server it goes to; or, `dossiers` set, the
    // dossiers of that profile in `folder`.
    struct Post
    {
        const char* path;
        std::string body;
        std::uint32_t dossiers = 0;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Post> queue;
    bool stop = false;
    std::shared_ptr<HttpCancel> cancel = std::make_shared<HttpCancel>();
    std::filesystem::path folder; // the dossiers', empty: none
};

std::unique_ptr<OnlineReports> OnlineReports::Start(const std::string& server, const std::string& release,
                                                    const std::string& build, Log log)
{
    if (server.empty()) return nullptr;
    Send send = HttpSend(server, log);
    return std::make_unique<OnlineReports>(release, build, HostOsName(), std::move(send), std::move(log));
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
        std::atomic<bool> dossiers{false};
    };
    auto off = std::make_shared<Endpoints>();
    return [server, log = std::move(log), request = std::move(request), off](
               const std::string& path, const std::string& body, const std::shared_ptr<HttpCancel>& cancel) {
        const bool errors = path == kErrorsPath;
        const bool dossier = path == kDossiersPath;
        std::atomic<bool>& unsupported = errors ? off->errors : dossier ? off->dossiers : off->reports;
        // A dossier stays for a server that takes it.
        if (unsupported) return !dossier;
        HttpRequest http;
        http.url = "http://" + server + path;
        http.method = "POST";
        http.body = body;
        // Plain http like the game's own WFC requests (its NAS login and
        // gamestats) to the same server.
        http.allow_http = true;
        http.headers = {dossier ? "Content-Type: application/octet-stream" : "Content-Type: application/json"};
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
                    log(std::string("Match reports: ")
                        + (errors ? "error codes off" : dossier ? "desync dossiers kept" : "off")
                        + ", the server takes none (HTTP " + std::to_string(response.status) + ")");
                if (dossier) return false;
            }
            // Not logged in from here (yet), or too many: a later series
            // sends the dossier again.
            else if (dossier && (response.status == 403 || response.status == 429))
                return false;
            else if (response.status != 200 && log)
                log(std::string("Match reports: the server refused ")
                    + (errors ? "an error code" : dossier ? "a desync dossier" : "a report")
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

OnlineReports::OnlineReports(std::string release, std::string build, std::string os, Send send, Log log,
                             std::chrono::milliseconds retry)
    : state_(std::make_shared<State>()), log_(log), release_(std::move(release)), build_(std::move(build)),
      os_(std::move(os))
{
    thread_ = std::thread([state = state_, send = std::move(send), log = std::move(log), retry] {
        std::unique_lock lock(state->mutex);
        for (;;)
        {
            state->wake.wait(lock, [&] { return state->stop || !state->queue.empty(); });
            if (state->stop) return;
            const State::Post post = state->queue.front();
            // A profile's dossiers: each sent once, deleted once delivered.
            if (post.dossiers)
            {
                const std::filesystem::path folder = state->folder;
                lock.unlock();
                for (const auto& path : KeptDossiers(folder))
                {
                    const std::string bytes = ReadFile(path);
                    DesyncDossierHeader header;
                    std::error_code ignored;
                    if (!DossierHeader(bytes, header))
                        std::filesystem::remove(path, ignored);
                    else if (header.profile_id == post.dossiers && send(kDossiersPath, bytes, state->cancel))
                        std::filesystem::remove(path, ignored);
                    std::lock_guard stopping(state->mutex);
                    if (state->stop) return;
                }
                lock.lock();
                state->queue.pop_front();
                continue;
            }
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
    const bool begun = snapshot.series && (!seen_ || snapshot.series != last_.series);
    if (begun)
    {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        series_ = std::max<std::uint64_t>(std::uint64_t(std::max<long long>(now, 0)), series_ + 1);
    }
    const auto events = OnlineMatchEvents(seen_ ? &last_ : nullptr, snapshot, release_, build_, os_, series_);
    // An error code shown since the last look (none before the first).
    const std::string error = snapshot.error.shown != last_.error.shown
        ? OnlineErrorReport(snapshot.error, release_, build_)
        : std::string();
    last_ = snapshot;
    seen_ = true;
    // A series begun: its profile is logged in, its dossiers left over can go.
    const bool dossiers = begun && snapshot.profile_id && !folder_.empty();
    if (events.empty() && error.empty() && !dossiers) return;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& event : events) state_->queue.push_back({kReportsPath, event});
        if (!error.empty()) state_->queue.push_back({kErrorsPath, error});
        if (dossiers) state_->queue.push_back({kDossiersPath, {}, snapshot.profile_id});
    }
    state_->wake.notify_all();
}

void OnlineReports::KeepDossiers(std::filesystem::path folder)
{
    std::error_code error;
    if (!folder.empty()) std::filesystem::create_directories(folder, error);
    // Files half written when an earlier run ended (a dossier is written on
    // the game's thread, so none is now).
    for (std::filesystem::directory_iterator it(folder, error), end; !folder.empty() && !error && it != end;
         it.increment(error))
    {
        std::error_code ignored;
        if (it->path().extension() == ".tmp") std::filesystem::remove(it->path(), ignored);
    }
    folder_ = folder;
    std::lock_guard lock(state_->mutex);
    state_->folder = std::move(folder);
}

void OnlineReports::Dossier(const void* data, std::size_t size)
{
    if (folder_.empty()) return;
    std::string bytes(static_cast<const char*>(data), size);
    DesyncDossierHeader header;
    // The module names its series by its own count: the one Observe saw last.
    if (!DossierHeader(bytes, header) || !seen_ || header.series != last_.series || !header.profile_id)
    {
        if (log_) log_("Desync dossiers: one dropped (no series seen for it)");
        return;
    }
    header.series = series_;
    header.written_at = std::uint64_t(std::max<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
        0));
    Fixed(header.release, release_);
    Fixed(header.build, Printable(build_));
    Fixed(header.platform, HostPlatform());
    std::memcpy(bytes.data(), &header, sizeof header);
    // Whole or not at all: a crash while writing leaves a .tmp the next
    // run deletes (KeepDossiers). By profile too: both sides' of a game
    // have the same series and game, and two games on one computer (a
    // test) share the folder.
    const std::filesystem::path path =
        folder_ / (std::to_string(header.series) + '-' + std::to_string(header.game) + '-'
                   + std::to_string(header.profile_id) + kDossierExtension);
    std::filesystem::path partial = path;
    partial += ".tmp";
    std::error_code error;
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.close();
        if (!out) error = std::make_error_code(std::errc::io_error);
    }
    if (!error) std::filesystem::rename(partial, path, error);
    if (error)
    {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        if (log_) log_("Desync dossiers: cannot write " + Utf8(path.filename()) + ": " + error.message());
        return;
    }
    if (log_) log_("Desync dossiers: kept " + Utf8(path.filename()));
    {
        std::lock_guard lock(state_->mutex);
        state_->queue.push_back({kDossiersPath, {}, header.profile_id});
    }
    state_->wake.notify_all();
}
} // namespace mscharged::platform
