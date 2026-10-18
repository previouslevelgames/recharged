#include "platform/discord_presence.h"

#include "platform/json.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace mscharged::platform
{
namespace
{
using Clock = std::chrono::steady_clock;

std::string MessageOf(const JsonValue& reply)
{
    const auto& message = reply["data"]["message"];
    return message.IsString() ? message.Text() : "no reason given";
}
}

DiscordPresence::DiscordPresence(Options options) : options_(std::move(options))
{
    thread_ = std::thread([this] { Run(); });
}

DiscordPresence::~DiscordPresence()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void DiscordPresence::Show(std::optional<discord::Activity> activity)
{
    {
        std::lock_guard lock(mutex_);
        if (wanted_ == activity) return;
        wanted_ = std::move(activity);
        changed_ = true;
    }
    wake_.notify_all();
}

void DiscordPresence::Note(const std::string& line)
{
    if (line == last_note_) return;
    last_note_ = line;
    if (options_.log) options_.log("Discord: " + line);
}

bool DiscordPresence::Service(discord::Connection& connection, const discord::Frame& frame)
{
    if (frame.opcode == discord::Opcode::Ping)
        return connection.Write(discord::EncodeFrame(discord::Opcode::Pong, frame.json));
    if (frame.opcode == discord::Opcode::Close)
    {
        std::string reason = "the Discord app closed the connection";
        try
        {
            const auto closing = ParseJson(frame.json);
            const auto& message = closing["message"];
            if (message.IsString() && !message.Text().empty()) reason += " (" + message.Text() + ")";
        }
        catch (const std::exception&)
        {
        }
        Note(reason);
        return false;
    }
    return true;
}

std::unique_ptr<discord::Connection> DiscordPresence::Handshake()
{
    auto connection = options_.connect();
    if (!connection)
    {
        Note("the Discord app is not running (looking for it every "
            + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(options_.retry).count()) + " s)");
        return nullptr;
    }
    try
    {
        if (!connection->Write(discord::EncodeFrame(discord::Opcode::Handshake,
                                                    discord::HandshakeJson(options_.application_id))))
            throw std::runtime_error("the Discord app closed the connection");
        const auto deadline = Clock::now() + options_.reply_timeout;
        while (Clock::now() < deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            const auto frame = connection->Read(left);
            if (!frame) break;
            if (!Service(*connection, *frame)) return nullptr;
            if (frame->opcode != discord::Opcode::Frame) continue;
            const auto reply = ParseJson(frame->json);
            if (reply["evt"].Text() == "READY")
            {
                Note("connected");
                return connection;
            }
            if (reply["evt"].Text() == "ERROR")
            {
                Note("the Discord app refused the connection (" + MessageOf(reply) + ")");
                return nullptr;
            }
        }
        Note("the Discord app did not answer");
    }
    catch (const std::exception& error)
    {
        Note(error.what());
    }
    return nullptr;
}

bool DiscordPresence::Send(discord::Connection& connection, const std::optional<discord::Activity>& activity)
{
    const auto nonce = std::to_string(++nonce_);
    try
    {
        if (!connection.Write(discord::EncodeFrame(
                discord::Opcode::Frame,
                discord::SetActivityJson(activity, discord::CurrentProcessId(), nonce))))
            throw std::runtime_error("the connection to the Discord app was lost");
        const auto deadline = Clock::now() + options_.reply_timeout;
        while (Clock::now() < deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            const auto frame = connection.Read(left);
            if (!frame) break;
            if (!Service(connection, *frame)) return false;
            if (frame->opcode != discord::Opcode::Frame) continue;
            const auto reply = ParseJson(frame->json);
            if (reply["nonce"].Text() != nonce) continue;
            // A refused status stays refused: sending it again would not help.
            if (reply["evt"].Text() == "ERROR") Note("the Discord app refused the status (" + MessageOf(reply) + ")");
            else if (activity) Note("showing the status");
            return true;
        }
        Note("the Discord app did not answer");
    }
    catch (const std::exception& error)
    {
        Note(error.what());
    }
    return false;
}

void DiscordPresence::Run()
{
    std::unique_ptr<discord::Connection> connection;
    // What Discord shows of ours; nothing on a new connection.
    std::optional<std::optional<discord::Activity>> shown;
    Clock::time_point next_attempt = Clock::now(), last_send{};
    bool sent_any = false;
    std::unique_lock lock(mutex_);
    while (!stop_)
    {
        const auto now = Clock::now();
        if (!connection && now >= next_attempt)
        {
            lock.unlock();
            connection = Handshake();
            lock.lock();
            next_attempt = Clock::now() + options_.retry;
            shown.reset();
            continue;
        }
        const bool pending = connection && (!shown || *shown != wanted_);
        const auto allowed = sent_any ? last_send + options_.min_interval : now;
        if (pending && now >= allowed)
        {
            const auto activity = wanted_;
            changed_ = false;
            lock.unlock();
            const bool sent = Send(*connection, activity);
            lock.lock();
            last_send = Clock::now();
            sent_any = true;
            if (sent)
            {
                shown = activity;
                if (options_.shown) options_.shown(activity);
            }
            else
            {
                connection.reset();
                next_attempt = Clock::now() + options_.retry;
            }
            continue;
        }
        // While connected and idle, answer pings and notice a closed app.
        if (connection && !pending)
        {
            lock.unlock();
            bool alive = true;
            try
            {
                while (const auto frame = connection->Read(std::chrono::milliseconds(0)))
                    if (!(alive = Service(*connection, *frame))) break;
            }
            catch (const std::exception& error)
            {
                Note(error.what());
                alive = false;
            }
            lock.lock();
            if (!alive)
            {
                connection.reset();
                next_attempt = Clock::now() + options_.retry;
                continue;
            }
        }
        const auto wake = !connection ? next_attempt
            : pending                 ? allowed
                                      : now + std::chrono::seconds(1);
        wake_.wait_until(lock, wake, [&] { return stop_ || changed_; });
        changed_ = false;
    }
    lock.unlock();
    // Clear the status before leaving, without waiting for an answer: Discord
    // also clears it when the connection closes.
    if (connection && shown && *shown)
        connection->Write(discord::EncodeFrame(
            discord::Opcode::Frame,
            discord::SetActivityJson(std::nullopt, discord::CurrentProcessId(), std::to_string(++nonce_))));
}
}
