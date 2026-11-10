#include "launcher/players_online.h"

#include "platform/json.h"

#include <exception>
#include <utility>

namespace mscharged::launcher
{
namespace
{
constexpr const char* kPath = "/api/v1/online";
} // namespace

std::string PlayersOnlineUrl(const std::string& server) { return "http://" + server + kPath; }

std::optional<PlayersOnline::Count> ParsePlayersOnline(std::string_view body)
{
    try
    {
        const auto json = platform::ParseJson(body);
        if (!json.IsObject()) return std::nullopt;
        const auto online = json["wfc"].Integer(), playing = json["playing"].Integer();
        if (!online || !playing || *online < 0 || *playing < 0 || *playing > *online) return std::nullopt;
        return PlayersOnline::Count{*online, *playing};
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

PlayersOnline::PlayersOnline(std::string server, Log log, std::chrono::milliseconds every, Request request)
    : server_(std::move(server)), log_(std::move(log)), every_(every),
      request_(request ? std::move(request) : Request([](const platform::HttpRequest& http) {
          return platform::HttpGet(http);
      }))
{
    thread_ = std::thread([this] { Run(); });
}

PlayersOnline::~PlayersOnline()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cancel_->Cancel();
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::optional<PlayersOnline::Count> PlayersOnline::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return count_;
}

void PlayersOnline::Run()
{
    std::unique_lock lock(mutex_);
    while (!stop_ && !unsupported_)
    {
        lock.unlock();
        Ask();
        lock.lock();
        wake_.wait_for(lock, every_, [this] { return stop_; });
    }
}

void PlayersOnline::Ask()
{
    platform::HttpRequest http;
    http.url = PlayersOnlineUrl(server_);
    // Plain http like the game's own WFC requests to the same server.
    http.allow_http = true;
    http.connect_timeout = 10;
    http.stall_timeout = 15;
    http.max_size = 4u << 10;
    http.cancel = cancel_;
    std::optional<Count> count;
    std::string problem;
    bool unsupported = false;
    try
    {
        const auto response = request_(http);
        if (response.status == 404 || response.status == 405)
        {
            unsupported = true;
            problem = "the server has no count (HTTP " + std::to_string(response.status) + ")";
        }
        else if (response.status != 200)
            problem = "HTTP " + std::to_string(response.status);
        else if (!(count = ParsePlayersOnline(response.body)))
            problem = "an answer it cannot read";
    }
    catch (const platform::HttpError& error)
    {
        if (error.Cancelled()) return;
        problem = error.what();
    }
    catch (const std::exception& error)
    {
        problem = error.what();
    }
    std::string line;
    {
        std::lock_guard lock(mutex_);
        if (stop_) return;
        count_ = count;
        unsupported_ = unsupported;
        if (count)
        {
            if (!answered_ || failing_) line = "Players online: from " + server_;
            answered_ = true;
            failing_ = false;
        }
        else if (unsupported)
            line = "Players online: off, " + problem;
        else if (!failing_)
        {
            failing_ = true;
            line = "Players online: not available (" + problem + ")";
        }
    }
    if (!line.empty() && log_) log_(line);
}
} // namespace mscharged::launcher
