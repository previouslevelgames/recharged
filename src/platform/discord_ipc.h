#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The local connection to the Discord app for the Discord status (issue #71).
// The app listens on a Unix socket (Linux, macOS) or a named pipe (Windows)
// called discord-ipc-0 to -9. Each message is a frame: opcode and length as
// little-endian 32-bit numbers, then that many bytes of JSON. A handshake
// with the application ID (public, not a secret) is answered with READY;
// SET_ACTIVITY then sets or clears the status. No login or token is needed.
namespace mscharged::platform::discord
{
enum class Opcode : std::uint32_t
{
    Handshake = 0,
    Frame = 1,
    Close = 2,
    Ping = 3,
    Pong = 4,
};

struct Frame
{
    Opcode opcode = Opcode::Frame;
    std::string json;
};

// Discord's own limit on a frame's JSON is far below this; larger lengths
// mean the stream is not a Discord connection.
inline constexpr std::uint32_t kMaxFrameJson = 64 * 1024;

std::string EncodeFrame(Opcode opcode, std::string_view json);
// Takes one whole frame off the front of `buffer`, or nothing while it is
// incomplete. Throws std::runtime_error for a length above kMaxFrameJson.
std::optional<Frame> DecodeFrame(std::string& buffer);

// What the status shows: details is the second line, state the third (the
// application's name is the first). Empty texts are left out; start is a
// Unix time in seconds that Discord counts up from (0: no timer).
struct Activity
{
    std::string details;
    std::string state;
    std::string large_image;
    std::string large_text;
    std::int64_t start = 0;
    bool operator==(const Activity& other) const
    {
        return details == other.details && state == other.state && large_image == other.large_image
            && large_text == other.large_text && start == other.start;
    }
    bool operator!=(const Activity& other) const { return !(*this == other); }
};

// A JSON string literal: quotes, escapes and control characters.
std::string JsonString(std::string_view text);
std::string HandshakeJson(std::string_view application_id);
// SET_ACTIVITY for this process; no activity clears the status.
std::string SetActivityJson(const std::optional<Activity>& activity, long process_id, std::string_view nonce);

// A Discord application ID: a snowflake, 17 to 20 digits.
bool ValidApplicationId(std::string_view id);

// One open connection to the Discord app.
class Connection
{
public:
    virtual ~Connection() = default;
    // Sends all of `bytes`; false when the connection is gone.
    virtual bool Write(std::string_view bytes) = 0;
    // The next frame, waiting at most `timeout`; nothing on timeout. Throws
    // std::runtime_error when the connection is gone or sends garbage.
    virtual std::optional<Frame> Read(std::chrono::milliseconds timeout) = 0;
};

// The places the Discord app may listen, in the order to try: on Linux and
// macOS sockets in $XDG_RUNTIME_DIR, $TMPDIR, $TMP, $TEMP and /tmp (also
// inside Flatpak and Snap app folders), on Windows the pipes.
std::vector<std::string> EndpointCandidates();
// The first endpoint that accepts a connection, or nothing (Discord is not
// running).
std::unique_ptr<Connection> ConnectToDiscord();
// One endpoint (a socket path, or a pipe name on Windows), for tests.
std::unique_ptr<Connection> ConnectToEndpoint(const std::string& endpoint);

long CurrentProcessId();
}
