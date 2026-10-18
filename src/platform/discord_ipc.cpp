#include "platform/discord_ipc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace mscharged::platform::discord
{
namespace
{
void PutLittle32(std::string& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) out.push_back(char((value >> shift) & 0xff));
}

std::uint32_t GetLittle32(const std::string& in, std::size_t at)
{
    std::uint32_t value = 0;
    for (int n = 3; n >= 0; --n) value = (value << 8) | std::uint8_t(in[at + std::size_t(n)]);
    return value;
}

// At most `limit` bytes, not splitting a UTF-8 character.
std::string_view Clip(std::string_view text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (std::uint8_t(text[end]) & 0xc0) == 0x80) --end;
    return text.substr(0, end);
}

// Discord rejects longer texts; it also wants at least two characters.
constexpr std::size_t kMaxText = 128;
bool Shown(std::string_view text) { return text.size() >= 2; }

using Clock = std::chrono::steady_clock;
}

std::string EncodeFrame(Opcode opcode, std::string_view json)
{
    std::string frame;
    frame.reserve(8 + json.size());
    PutLittle32(frame, std::uint32_t(opcode));
    PutLittle32(frame, std::uint32_t(json.size()));
    frame.append(json);
    return frame;
}

std::optional<Frame> DecodeFrame(std::string& buffer)
{
    if (buffer.size() < 8) return std::nullopt;
    const std::uint32_t length = GetLittle32(buffer, 4);
    if (length > kMaxFrameJson) throw std::runtime_error("Discord sent a frame of " + std::to_string(length) + " bytes");
    if (buffer.size() < 8 + std::size_t(length)) return std::nullopt;
    Frame frame{Opcode(GetLittle32(buffer, 0)), buffer.substr(8, length)};
    buffer.erase(0, 8 + std::size_t(length));
    return frame;
}

std::string JsonString(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (std::uint8_t(c) < 0x20)
            {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04x", unsigned(std::uint8_t(c)));
                out += escaped;
            }
            else
                out += c;
        }
    }
    return out + "\"";
}

std::string HandshakeJson(std::string_view application_id)
{
    return "{\"v\":1,\"client_id\":" + JsonString(application_id) + "}";
}

std::string SetActivityJson(const std::optional<Activity>& activity, long process_id, std::string_view nonce)
{
    std::string args = "\"pid\":" + std::to_string(process_id);
    if (activity)
    {
        std::string fields;
        const auto add = [&](std::string& to, const char* name, std::string_view text) {
            if (!Shown(text)) return;
            if (!to.empty()) to += ",";
            to += std::string("\"") + name + "\":" + JsonString(Clip(text, kMaxText));
        };
        add(fields, "details", activity->details);
        add(fields, "state", activity->state);
        if (activity->start > 0)
        {
            if (!fields.empty()) fields += ",";
            fields += "\"timestamps\":{\"start\":" + std::to_string(activity->start) + "}";
        }
        std::string assets;
        add(assets, "large_image", activity->large_image);
        add(assets, "large_text", activity->large_text);
        if (!assets.empty())
        {
            if (!fields.empty()) fields += ",";
            fields += "\"assets\":{" + assets + "}";
        }
        args += ",\"activity\":{" + fields + "}";
    }
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{" + args + "},\"nonce\":" + JsonString(nonce) + "}";
}

bool ValidApplicationId(std::string_view id)
{
    return id.size() >= 17 && id.size() <= 20
        && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

#ifdef _WIN32
namespace
{
class PipeConnection final : public Connection
{
public:
    explicit PipeConnection(HANDLE pipe) : pipe_(pipe) {}
    ~PipeConnection() override { CloseHandle(pipe_); }

    bool Write(std::string_view bytes) override
    {
        while (!bytes.empty())
        {
            DWORD written = 0;
            if (!WriteFile(pipe_, bytes.data(), DWORD(std::min<std::size_t>(bytes.size(), 1 << 16)), &written, nullptr)
                || written == 0)
                return false;
            bytes.remove_prefix(written);
        }
        return true;
    }

    std::optional<Frame> Read(std::chrono::milliseconds timeout) override
    {
        const auto deadline = Clock::now() + timeout;
        for (;;)
        {
            if (auto frame = DecodeFrame(buffer_)) return frame;
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr))
                throw std::runtime_error("the Discord app closed the connection");
            if (available > 0)
            {
                char chunk[4096];
                DWORD read = 0;
                if (!ReadFile(pipe_, chunk, std::min<DWORD>(available, sizeof chunk), &read, nullptr) || read == 0)
                    throw std::runtime_error("the Discord app closed the connection");
                buffer_.append(chunk, read);
                continue;
            }
            if (Clock::now() >= deadline) return std::nullopt;
            Sleep(5);
        }
    }

private:
    HANDLE pipe_;
    std::string buffer_;
};
}

std::vector<std::string> EndpointCandidates()
{
    std::vector<std::string> endpoints;
    for (int n = 0; n < 10; ++n) endpoints.push_back("\\\\?\\pipe\\discord-ipc-" + std::to_string(n));
    return endpoints;
}

std::unique_ptr<Connection> ConnectToEndpoint(const std::string& endpoint)
{
    const std::wstring name(endpoint.begin(), endpoint.end());
    const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return nullptr;
    return std::make_unique<PipeConnection>(pipe);
}

long CurrentProcessId() { return long(GetCurrentProcessId()); }
#else
namespace
{
class SocketConnection final : public Connection
{
public:
    explicit SocketConnection(int socket) : socket_(socket) {}
    ~SocketConnection() override { close(socket_); }

    bool Write(std::string_view bytes) override
    {
#ifdef MSG_NOSIGNAL
        constexpr int kFlags = MSG_NOSIGNAL; // a closed connection must not raise SIGPIPE
#else
        constexpr int kFlags = 0; // macOS: SO_NOSIGPIPE on the socket instead
#endif
        while (!bytes.empty())
        {
            const auto sent = send(socket_, bytes.data(), bytes.size(), kFlags);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) return false;
            bytes.remove_prefix(std::size_t(sent));
        }
        return true;
    }

    std::optional<Frame> Read(std::chrono::milliseconds timeout) override
    {
        const auto deadline = Clock::now() + timeout;
        for (;;)
        {
            if (auto frame = DecodeFrame(buffer_)) return frame;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return std::nullopt;
            pollfd poll_fd{socket_, POLLIN, 0};
            const int ready = poll(&poll_fd, 1, int(std::min<long long>(left, 1000)));
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) throw std::runtime_error(std::string("cannot wait for the Discord app: ") + std::strerror(errno));
            if (ready == 0) continue;
            char chunk[4096];
            const auto received = recv(socket_, chunk, sizeof chunk, 0);
            if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (received <= 0) throw std::runtime_error("the Discord app closed the connection");
            buffer_.append(chunk, std::size_t(received));
        }
    }

private:
    int socket_;
    std::string buffer_;
};
}

std::vector<std::string> EndpointCandidates()
{
    std::vector<std::string> folders;
    for (const char* variable : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
        if (const char* value = std::getenv(variable); value && *value) folders.emplace_back(value);
    folders.emplace_back("/tmp");
    std::vector<std::string> endpoints;
    for (std::size_t n = 0; n < folders.size(); ++n)
    {
        auto folder = folders[n];
        while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
        if (std::find(folders.begin(), folders.begin() + std::ptrdiff_t(n), folder) != folders.begin() + std::ptrdiff_t(n))
            continue;
        // Discord itself, then its Flatpak and Snap packages and Vesktop.
        for (const char* app : {"", "/app/com.discordapp.Discord", "/app/com.discordapp.DiscordCanary",
                                "/app/dev.vencord.Vesktop", "/snap.discord", "/snap.discord-canary"})
            for (int index = 0; index < 10; ++index)
                endpoints.push_back(folder + app + "/discord-ipc-" + std::to_string(index));
    }
    return endpoints;
}

std::unique_ptr<Connection> ConnectToEndpoint(const std::string& endpoint)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (endpoint.size() >= sizeof address.sun_path) return nullptr;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    const int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) return nullptr;
    fcntl(socket_fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    if (connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
    {
        close(socket_fd);
        return nullptr;
    }
    return std::make_unique<SocketConnection>(socket_fd);
}

long CurrentProcessId() { return long(getpid()); }
#endif

std::unique_ptr<Connection> ConnectToDiscord()
{
    for (const auto& endpoint : EndpointCandidates())
        if (auto connection = ConnectToEndpoint(endpoint)) return connection;
    return nullptr;
}
}
