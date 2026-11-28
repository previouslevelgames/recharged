#pragma once

// A TCP connection for the replays' traffic to the WFC server (online_replays.h,
// the live replay's reader): plain HTTP over it, at the lowest priority this
// computer offers. Its send buffer is small, so the operating system never
// holds more than a few KB of it in front of the match's own packets; it is
// marked as background traffic (DSCP CS1) where the system lets a program say
// so. After Connect nothing waits: SendSome and ReceiveSome take what the
// connection has room for, or has, now.

#include <cstddef>
#include <cstdint>
#include <string>

namespace mscharged::platform
{
class TcpClient
{
public:
    TcpClient() = default;
    ~TcpClient() { Close(); }
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // `server` as online.server has it: a host name or address, with
    // :port or not (80). Waits at most `timeout_ms`; false with `error` set.
    bool Connect(const std::string& server, int timeout_ms, std::string& error);
    bool Connected() const { return socket_ != kNone; }
    // What the connection takes now, at most `size`: the bytes sent, 0 when
    // it is full, -1 when it broke (Close it).
    long SendSome(const void* data, std::size_t size);
    // What has arrived, at most `size`: the bytes, 0 none yet, -1 when the
    // server closed it or it broke.
    long ReceiveSome(void* data, std::size_t size);
    // Waits at most `timeout_ms` for something to read; true when there is.
    bool WaitReadable(int timeout_ms);
    void Close();

    // `server`'s host and port (80 without one); false when it is no name.
    static bool SplitServer(const std::string& server, std::string& host, std::uint16_t& port);

private:
#ifdef _WIN32
    using Socket = std::uintptr_t;
    static constexpr Socket kNone = ~Socket(0);
#else
    using Socket = int;
    static constexpr Socket kNone = -1;
#endif
    Socket socket_ = kNone;
};
} // namespace mscharged::platform
