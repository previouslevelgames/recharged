#include "platform/tcp_client.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#elif !defined(__SWITCH__)
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mscharged::platform
{
bool TcpClient::SplitServer(const std::string& server, std::string& host, std::uint16_t& port)
{
    host = server;
    port = 80;
    if (!server.empty() && server.front() == '[')
    {
        const auto close = server.find(']');
        if (close == std::string::npos) return false;
        host = server.substr(1, close - 1);
        if (close + 1 < server.size())
        {
            if (server[close + 1] != ':') return false;
            const std::string digits = server.substr(close + 2);
            if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos)
                return false;
            const unsigned long value = std::stoul(digits);
            if (!value || value > 65535) return false;
            port = std::uint16_t(value);
        }
        return !host.empty();
    }
    const auto colon = server.rfind(':');
    if (colon != std::string::npos && server.find(':') == colon)
    {
        host = server.substr(0, colon);
        const std::string digits = server.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos)
            return false;
        const unsigned long value = std::stoul(digits);
        if (!value || value > 65535) return false;
        port = std::uint16_t(value);
    }
    return !host.empty();
}

#if defined(__SWITCH__)
bool TcpClient::Connect(const std::string&, int, std::string& error)
{
    error = "not on this system";
    return false;
}
long TcpClient::SendSome(const void*, std::size_t) { return -1; }
long TcpClient::ReceiveSome(void*, std::size_t) { return -1; }
bool TcpClient::WaitReadable(int) { return false; }
void TcpClient::Close() {}
#else

namespace
{
// The send buffer: the most of this traffic the system queues in front of the
// match's packets.
constexpr int kSendBuffer = 4096;
// DSCP CS1 (background, "lower effort" for routers that honour it; Windows
// keeps its own unless a QoS policy says otherwise).
constexpr int kBackgroundTos = 0x20;

#if defined(_WIN32)
using Native = SOCKET;
bool Started()
{
    static std::once_flag once;
    static bool started = false;
    std::call_once(once, [] {
        WSADATA data;
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return started;
}
int LastError() { return WSAGetLastError(); }
bool WouldBlock(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
void CloseSocket(std::uintptr_t socket) { closesocket(Native(socket)); }
int Poll(pollfd* fds, int count, int timeout) { return WSAPoll(fds, ULONG(count), timeout); }
#else
using Native = int;
bool Started() { return true; }
int LastError() { return errno; }
bool WouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS; }
void CloseSocket(int socket) { ::close(socket); }
int Poll(pollfd* fds, int count, int timeout) { return ::poll(fds, nfds_t(count), timeout); }
#endif
} // namespace

bool TcpClient::Connect(const std::string& server, int timeout_ms, std::string& error)
{
    Close();
    std::string host;
    std::uint16_t port = 0;
    if (!SplitServer(server, host, port))
    {
        error = "the server's address is not one";
        return false;
    }
    if (!Started())
    {
        error = "the system's network is not available";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found)
    {
        error = "the server's name could not be resolved";
        return false;
    }
    error = "could not connect to the server";
    for (const addrinfo* address = found; address; address = address->ai_next)
    {
#if defined(_WIN32)
        const SOCKET raw = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (raw == INVALID_SOCKET) continue;
        const Socket candidate = Socket(raw);
        u_long nonblocking = 1;
        ioctlsocket(raw, FIONBIO, &nonblocking);
        setsockopt(raw, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&kSendBuffer), sizeof kSendBuffer);
        if (address->ai_family == AF_INET)
            setsockopt(raw, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&kBackgroundTos), sizeof kBackgroundTos);
#else
        const int candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) continue;
        fcntl(candidate, F_SETFL, fcntl(candidate, F_GETFL, 0) | O_NONBLOCK);
        fcntl(candidate, F_SETFD, FD_CLOEXEC);
        setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &kSendBuffer, sizeof kSendBuffer);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        // Background traffic, where the system lets a program mark it.
        if (address->ai_family == AF_INET)
            setsockopt(candidate, IPPROTO_IP, IP_TOS, &kBackgroundTos, sizeof kBackgroundTos);
#ifdef IPV6_TCLASS
        else if (address->ai_family == AF_INET6)
            setsockopt(candidate, IPPROTO_IPV6, IPV6_TCLASS, &kBackgroundTos, sizeof kBackgroundTos);
#endif
#endif
        int result = connect(Native(candidate), address->ai_addr, int(address->ai_addrlen));
        bool connected = result == 0;
        if (!connected && WouldBlock(LastError()))
        {
            pollfd wait{};
            wait.fd = decltype(wait.fd)(candidate);
            wait.events = POLLOUT;
            if (Poll(&wait, 1, timeout_ms) == 1 && (wait.revents & POLLOUT))
            {
                int failure = 0;
                socklen_t length = sizeof failure;
                connected = getsockopt(Native(candidate), SOL_SOCKET, SO_ERROR,
                                       reinterpret_cast<char*>(&failure), &length) == 0
                    && failure == 0;
            }
            else
                error = "the server did not answer in time";
        }
        if (!connected)
        {
            CloseSocket(candidate);
            continue;
        }
        // Each request's parts go out as they are written: the stream is a
        // little at a time, and should not wait for Nagle's algorithm.
        const int one = 1;
        setsockopt(Native(candidate), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&one), sizeof one);
        socket_ = candidate;
        break;
    }
    freeaddrinfo(found);
    if (socket_ == kNone) return false;
    error.clear();
    return true;
}

long TcpClient::SendSome(const void* data, std::size_t size)
{
    if (socket_ == kNone) return -1;
    if (!size) return 0;
#if defined(_WIN32)
    const int sent = send(SOCKET(socket_), static_cast<const char*>(data), int(size > 1u << 20 ? 1u << 20 : size), 0);
#elif defined(MSG_NOSIGNAL)
    const long sent = long(send(socket_, data, size, MSG_NOSIGNAL));
#else
    const long sent = long(send(socket_, data, size, 0));
#endif
    if (sent >= 0) return long(sent);
    return WouldBlock(LastError()) ? 0 : -1;
}

long TcpClient::ReceiveSome(void* data, std::size_t size)
{
    if (socket_ == kNone) return -1;
#if defined(_WIN32)
    const int got = recv(SOCKET(socket_), static_cast<char*>(data), int(size > 1u << 20 ? 1u << 20 : size), 0);
#else
    const long got = long(recv(socket_, data, size, 0));
#endif
    if (got > 0) return long(got);
    if (got == 0) return -1; // closed
    return WouldBlock(LastError()) ? 0 : -1;
}

bool TcpClient::WaitReadable(int timeout_ms)
{
    if (socket_ == kNone) return false;
    pollfd wait{};
    wait.fd = decltype(wait.fd)(socket_);
    wait.events = POLLIN;
    return Poll(&wait, 1, timeout_ms) == 1 && (wait.revents & (POLLIN | POLLHUP | POLLERR));
}

void TcpClient::Close()
{
    if (socket_ == kNone) return;
    CloseSocket(socket_);
    socket_ = kNone;
}
#endif
} // namespace mscharged::platform
