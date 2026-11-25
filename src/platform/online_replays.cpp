#include "platform/online_replays.h"

#include "platform/json.h"
#include "platform/live_ring.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>

namespace mscharged::platform
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr const char* kStreamsPath = "/api/v1/wfc/streams";
// Where a part of a cutscene the player skipped goes (a server before 1.2.28
// has none: 404/405), and the tries a skip gets.
constexpr const char* kSkipsPath = "/api/v1/wfc/streams/skips";
constexpr int kSkipAttempts = 3;
constexpr const char* kReplaysPath = "/api/v1/wfc/replays";
// What one Take copies at most (room for any message, live_ring.h).
constexpr std::size_t kTakeBytes = 64u << 10;
// The server takes a game's stream up to this, and a replay file up to this.
constexpr std::size_t kMostStream = 8u << 20;
constexpr std::size_t kMostReplay = 1u << 20;
constexpr std::size_t kChunk = 4096;
// A game whose series the server streams nothing of is dropped this long
// after its end (the answer to the series' start can come after a short game).
constexpr auto kAnswerWait = std::chrono::seconds(10);
// A replay the server asked for whose file never came, or that could not go.
constexpr auto kReplayWait = std::chrono::minutes(10);
constexpr auto kReplayRetry = std::chrono::seconds(30);
constexpr int kReplayAttempts = 3;

std::string Hex(std::size_t value)
{
    char text[20];
    std::snprintf(text, sizeof text, "%zx", value);
    return text;
}

std::string Head(const std::string& server, const std::string& target, const std::string& length)
{
    return "POST " + target + " HTTP/1.1\r\nHost: " + server
        + "\r\nUser-Agent: Recharged\r\nContent-Type: application/octet-stream\r\n" + length
        + "Connection: close\r\n\r\n";
}
} // namespace

// One HTTP request on a connection of its own, written a little at a time:
// its head, then its body as it is queued; its answer read as it comes.
class OnlineReplaysRequest
{
public:
    bool Open(const std::string& server, const std::string& head, std::string& error)
    {
        Close();
        if (!connection_.Connect(server, 3000, error)) return false;
        out_ = head;
        return true;
    }
    bool IsOpen() const { return connection_.Connected(); }
    void Queue(const std::string& bytes) { out_ += bytes; }
    std::size_t Waiting() const { return out_.size(); }
    // At most `budget` bytes now: those written, -1 once the connection broke.
    long Write(std::size_t budget)
    {
        if (out_.empty()) return 0;
        const long sent = connection_.SendSome(out_.data(), std::min(budget, out_.size()));
        if (sent > 0) out_.erase(0, std::size_t(sent));
        return sent;
    }
    // The answer's status once it came whole (0 not yet; -1 the connection
    // closed without one), and its body.
    int Answer(std::string& body)
    {
        char buffer[4096];
        bool closed = false;
        for (;;)
        {
            const long got = connection_.ReceiveSome(buffer, sizeof buffer);
            if (got <= 0)
            {
                closed = got < 0;
                break;
            }
            answer_.append(buffer, std::size_t(got));
            if (answer_.size() > (64u << 10))
            {
                closed = true;
                break;
            }
        }
        const auto end = answer_.find("\r\n\r\n");
        if (end == std::string::npos) return closed ? -1 : 0;
        int status = -1;
        if (answer_.compare(0, 5, "HTTP/") == 0)
        {
            const auto space = answer_.find(' ');
            if (space != std::string::npos && space < end) status = std::atoi(answer_.c_str() + space + 1);
        }
        std::string lower = answer_.substr(0, end);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::size_t length = std::string::npos;
        if (const auto field = lower.find("\r\ncontent-length:"); field != std::string::npos)
            length = std::size_t(std::strtoull(lower.c_str() + field + 17, nullptr, 10));
        const std::size_t have = answer_.size() - end - 4;
        if (length == std::string::npos ? !closed : have < length && !closed) return 0;
        body = answer_.substr(end + 4, length == std::string::npos ? std::string::npos : length);
        return status > 0 ? status : -1;
    }
    void Close()
    {
        connection_.Close();
        out_.clear();
        answer_.clear();
    }

private:
    TcpClient connection_;
    std::string out_, answer_;
};

struct OnlineReplays::Live
{
    std::uint32_t stream = 0;
    replay::LiveBegin begin;
    std::string bytes;
    bool complete = false; // its end came
    bool stopped = false;  // the module stopped putting it in the ring: it stays incomplete
    Clock::time_point over_at{};
    std::uint64_t series = 0;
    enum class State { Waiting, Sending, Done } state = State::Waiting;
    OnlineReplaysRequest request;
    std::size_t offset = 0; // where the request starts in the stream
    std::size_t queued = 0; // the stream's bytes in the request so far
    bool ended = false;     // its last chunk queued
    int attempts = 0;
    Clock::time_point retry_at{}, progress_at{};
    bool announced = false;
    // The parts of cutscenes skipped (number, hundredths of a second into it)
    // and those said so, one request at a time.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> skips;
    std::size_t skips_said = 0;
    OnlineReplaysRequest skip_request;
    int skip_attempts = 0;
    Clock::time_point skip_retry_at{};
};

struct OnlineReplays::Upload
{
    OnlineReplayAnswers::Wanted wanted;
    std::filesystem::path file;
    std::vector<std::uint8_t> bytes; // the file in memory, for a PC that keeps no replays
    Clock::time_point asked_at{}, retry_at{};
    int attempts = 0;
    bool sending = false;
    OnlineReplaysRequest request;
};

OnlineReplays::OnlineReplays(std::string server, std::shared_ptr<OnlineReplayAnswers> answers,
                             ChargedTakeLiveFunction take, Log log)
    : OnlineReplays(std::move(server), std::move(answers), take, std::move(log), Limits{})
{
}

OnlineReplays::OnlineReplays(std::string server, std::shared_ptr<OnlineReplayAnswers> answers,
                             ChargedTakeLiveFunction take, Log log, Limits limits)
    : server_(std::move(server)), answers_(std::move(answers)), take_(take), log_(std::move(log)), limits_(limits),
      taken_(kTakeBytes)
{
    thread_ = std::thread([this] { Run(); });
}

OnlineReplays::~OnlineReplays()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void OnlineReplays::Kept(std::uint64_t series, int game, std::filesystem::path file)
{
    if (!series || game < 1) return;
    std::lock_guard lock(mutex_);
    if (kept_.size() >= 16) kept_.erase(kept_.begin());
    kept_.push_back({series, game, std::move(file), {}});
}

void OnlineReplays::Kept(std::uint64_t series, int game, std::vector<std::uint8_t> bytes)
{
    if (!series || game < 1 || bytes.empty()) return;
    std::lock_guard lock(mutex_);
    if (kept_.size() >= 16) kept_.erase(kept_.begin());
    kept_.push_back({series, game, {}, std::move(bytes)});
}

void OnlineReplays::Run()
{
    std::unique_lock lock(mutex_);
    while (!stop_)
    {
        lock.unlock();
        const bool busy = Tick();
        lock.lock();
        wake_.wait_for(lock, busy ? limits_.tick : limits_.idle_tick, [&] { return stop_; });
    }
}

bool OnlineReplays::Tick()
{
    Drain();
    unsigned stopped = 0;
    int unconfirmed = 0;
    if (take_) take_(nullptr, 0, &stopped, &unconfirmed);
    SendLive(unconfirmed);
    SendSkips();
    SendReplays();
    return (live_ && live_->state != Live::State::Done) || !uploads_.empty();
}

void OnlineReplays::Drain()
{
    if (!take_) return;
    // A few rounds at most: the ring keeps the rest for the next tick.
    for (int round = 0; round < 8; ++round)
    {
        unsigned stopped = 0;
        int unconfirmed = 0;
        const unsigned size = take_(taken_.data(), unsigned(taken_.size()), &stopped, &unconfirmed);
        std::size_t at = 0;
        replay::LiveMessage message;
        while (replay::NextLiveMessage(taken_.data(), size, at, message))
        {
            if (message.kind == replay::kLiveBegin)
            {
                if (live_ && live_->state == Live::State::Sending && log_)
                    log_("Live stream: game " + std::to_string(live_->begin.game)
                         + " stopped (the next game began before it was sent)");
                live_ = std::make_unique<Live>();
                live_->stream = message.stream;
                if (message.size >= sizeof live_->begin) std::memcpy(&live_->begin, message.data, sizeof live_->begin);
                continue;
            }
            if (!live_ || live_->stream != message.stream || live_->complete || live_->stopped) continue;
            if (message.kind == replay::kLiveData)
            {
                if (live_->bytes.size() + message.size > kMostStream)
                {
                    live_->stopped = true;
                    live_->over_at = Clock::now();
                }
                else
                    live_->bytes.append(reinterpret_cast<const char*>(message.data), message.size);
            }
            else if (message.kind == replay::kLiveSkip && message.size >= 4 && live_->skips.size() < 512)
                live_->skips.emplace_back(std::uint16_t(message.data[0] | message.data[1] << 8),
                                          std::uint16_t(message.data[2] | message.data[3] << 8));
            else if (message.kind == replay::kLiveEnd)
            {
                live_->complete = true;
                live_->over_at = Clock::now();
            }
        }
        if (live_ && stopped == live_->stream && !live_->complete && !live_->stopped)
        {
            live_->stopped = true;
            live_->over_at = Clock::now();
        }
        if (!size) break;
    }
}

void OnlineReplays::SendLive(int unconfirmed)
{
    if (!live_ || live_->state == Live::State::Done) return;
    Live& live = *live_;
    const auto now = Clock::now();
    const std::string game = "game " + std::to_string(live.begin.game);
    auto give_up = [&](const std::string& why) {
        live.request.Close();
        live.state = Live::State::Done;
        if (log_) log_("Live stream: " + game + " stopped (" + why + "); its replay still goes after the game");
    };
    if (live.state == Live::State::Waiting)
    {
        if (!live.series && answers_) live.series = answers_->SeriesNumber(live.begin.module_series);
        const bool over = live.complete || live.stopped;
        if (!live.series || !answers_ || !answers_->Streamed(live.series))
        {
            if (over && now - live.over_at > kAnswerWait) live.state = Live::State::Done;
            return;
        }
        if (live.stopped)
        {
            live.state = Live::State::Done;
            return;
        }
        live.state = Live::State::Sending;
    }
    if (live.stopped && !live.complete) return give_up("the game module could not keep up");
    // The match's own connection comes first: nothing while it is in trouble.
    if (unconfirmed > limits_.trouble_steps && !live.complete) return;
    if (!live.request.IsOpen())
    {
        if (now < live.retry_at) return;
        if (live.attempts >= limits_.attempts) return give_up("the server could not be reached");
        ++live.attempts;
        const std::string target = std::string(kStreamsPath) + "?profile_id=" + std::to_string(live.begin.profile_id)
            + "&series=" + std::to_string(live.series) + "&game=" + std::to_string(live.begin.game)
            + "&offset=" + std::to_string(live.offset);
        std::string error;
        if (!live.request.Open(server_, Head(server_, target, "Transfer-Encoding: chunked\r\n"), error))
        {
            live.retry_at = now + limits_.retry;
            return;
        }
        live.queued = live.offset;
        live.ended = false;
        live.progress_at = now;
        if (!live.announced && log_)
            log_("Live stream: " + game + " of series " + std::to_string(live.series) + " to " + server_);
        live.announced = true;
    }
    // The stream's new bytes as chunks, as much as a tick sends.
    while (live.request.Waiting() < limits_.live_per_tick && live.queued < live.bytes.size())
    {
        const std::size_t size = std::min(kChunk, live.bytes.size() - live.queued);
        live.request.Queue(Hex(size) + "\r\n" + live.bytes.substr(live.queued, size) + "\r\n");
        live.queued += size;
    }
    if (live.complete && live.queued == live.bytes.size() && !live.ended)
    {
        live.request.Queue("0\r\n\r\n");
        live.ended = true;
    }
    const long sent = live.request.Write(limits_.live_per_tick);
    auto reconnect = [&] {
        // From a little before what was written: the server skips what it
        // has, and answers 409 with its size for a gap.
        const std::size_t unsure = live.request.Waiting() + 8192;
        live.offset = live.queued > unsure ? live.queued - unsure : 0;
        live.request.Close();
        live.retry_at = now + limits_.retry;
    };
    if (sent < 0) return reconnect();
    // A stall is data the connection does not take, or no answer after the
    // end; a game that has nothing new for a while (a pause, a cutscene) is
    // none.
    if (sent > 0 || (!live.request.Waiting() && !live.ended)) live.progress_at = now;
    else if (now - live.progress_at > limits_.stall)
        return give_up(live.request.Waiting() ? "the connection is too slow" : "the server did not answer");
    std::string body;
    const int status = live.request.Answer(body);
    if (!status) return;
    live.request.Close();
    if (status == 200 && live.ended)
    {
        live.state = Live::State::Done;
        if (log_) log_("Live stream: " + game + " complete (" + std::to_string(live.bytes.size()) + " bytes)");
        return;
    }
    if (status == 409)
    {
        try
        {
            const auto have = ParseJson(body)["have"].Integer();
            if (have && *have >= 0 && std::size_t(*have) <= live.bytes.size())
            {
                live.offset = std::size_t(*have);
                live.retry_at = now;
                return;
            }
        }
        catch (const std::exception&)
        {
        }
    }
    if (status < 0 && !live.ended) return reconnect();
    give_up("the server refused it, HTTP " + std::to_string(status) + (body.empty() ? "" : ": " + body.substr(0, 120)));
}

// A part of a cutscene the player skipped goes to the server at once, beside
// the stream: the match stands still while a cutscene plays, and each machine
// plays it as long as it lasts there, so the server's spectators skip it too.
void OnlineReplays::SendSkips()
{
    if (!live_ || skips_off_) return;
    Live& live = *live_;
    if (live.skips_said >= live.skips.size() || !live.announced) return;
    const auto now = Clock::now();
    auto next = [&] {
        ++live.skips_said;
        live.skip_attempts = 0;
    };
    auto again = [&] {
        live.skip_request.Close();
        live.skip_retry_at = now + limits_.retry;
        if (++live.skip_attempts >= kSkipAttempts) next();
    };
    if (!live.skip_request.IsOpen())
    {
        if (now < live.skip_retry_at) return;
        const auto [part, after] = live.skips[live.skips_said];
        const std::string target = std::string(kSkipsPath) + "?profile_id=" + std::to_string(live.begin.profile_id)
            + "&series=" + std::to_string(live.series) + "&game=" + std::to_string(live.begin.game)
            + "&section=" + std::to_string(part) + "&after=" + std::to_string(after);
        std::string error;
        if (!live.skip_request.Open(server_, Head(server_, target, "Content-Length: 0\r\n"), error)) return again();
    }
    if (live.skip_request.Write(limits_.live_per_tick) < 0) return again();
    std::string body;
    const int status = live.skip_request.Answer(body);
    if (!status) return;
    live.skip_request.Close();
    if (status == 404 || status == 405)
    {
        skips_off_ = true;
        if (log_) log_("Live stream: the server passes no cutscene skips on (it predates them)");
        return;
    }
    if (status < 0) return again();
    next();
}

void OnlineReplays::SendReplays()
{
    const auto now = Clock::now();
    if (answers_)
        for (const auto& wanted : answers_->TakeWanted())
        {
            if (replays_off_) break;
            const bool known = std::any_of(uploads_.begin(), uploads_.end(), [&](const auto& upload) {
                return upload->wanted.series == wanted.series && upload->wanted.game == wanted.game;
            });
            if (known || uploads_.size() >= 16) continue;
            auto upload = std::make_unique<Upload>();
            upload->wanted = wanted;
            upload->asked_at = now;
            uploads_.push_back(std::move(upload));
        }
    {
        std::lock_guard lock(mutex_);
        for (const auto& upload : uploads_)
            if (upload->file.empty() && upload->bytes.empty())
                for (const auto& kept : kept_)
                    if (kept.series == upload->wanted.series && kept.game == upload->wanted.game)
                    {
                        upload->file = kept.file;
                        upload->bytes = kept.bytes;
                    }
    }
    // Only between games: never while one is played.
    const bool playing = live_ && !live_->complete && !live_->stopped;
    for (auto it = uploads_.begin(); it != uploads_.end();)
    {
        Upload& upload = **it;
        const std::string game = "game " + std::to_string(upload.wanted.game) + " of series "
            + std::to_string(upload.wanted.series);
        bool drop = replays_off_ || (!upload.sending && now - upload.asked_at > kReplayWait);
        if (!drop && !upload.sending && (!upload.file.empty() || !upload.bytes.empty()) && !playing
            && now >= upload.retry_at && it == uploads_.begin())
        {
            std::ifstream in;
            std::string bytes(upload.bytes.begin(), upload.bytes.end());
            if (bytes.empty())
            {
                in.open(upload.file, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            const std::string target = std::string(kReplaysPath) + "?profile_id="
                + std::to_string(upload.wanted.profile_id) + "&series=" + std::to_string(upload.wanted.series)
                + "&game=" + std::to_string(upload.wanted.game);
            std::string error;
            if (upload.bytes.empty() && !in.good() && !in.eof())
                drop = true;
            else if (bytes.empty() || bytes.size() > kMostReplay)
                drop = true;
            else if (upload.request.Open(server_, Head(server_, target, "Content-Length: " + std::to_string(bytes.size()) + "\r\n"), error))
            {
                upload.request.Queue(bytes);
                upload.sending = true;
                upload.retry_at = now;
            }
            else
                upload.retry_at = now + kReplayRetry;
        }
        if (!drop && upload.sending)
        {
            const long sent = upload.request.Write(limits_.replay_per_tick);
            if (sent > 0) upload.retry_at = now;
            std::string body;
            const int status = sent < 0 ? -1 : upload.request.Answer(body);
            const bool stalled = sent == 0 && now - upload.retry_at > limits_.stall;
            if (status || stalled)
            {
                upload.request.Close();
                upload.sending = false;
                if (status == 200)
                {
                    if (log_) log_("Replays: sent " + game);
                    drop = true;
                }
                else if (status == 404 || status == 405)
                {
                    if (log_) log_("Replays: the server takes none (HTTP " + std::to_string(status) + ")");
                    replays_off_ = true;
                    drop = true;
                }
                else if (status == 400 || status == 413 || status == 422)
                {
                    if (log_) log_("Replays: the server refused " + game + " (HTTP " + std::to_string(status) + ")");
                    drop = true;
                }
                else if (++upload.attempts >= kReplayAttempts)
                {
                    if (log_) log_("Replays: " + game + " did not get through");
                    drop = true;
                }
                else
                    upload.retry_at = now + kReplayRetry;
            }
        }
        it = drop ? uploads_.erase(it) : std::next(it);
    }
}
} // namespace mscharged::platform
