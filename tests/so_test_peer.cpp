#include "so_test_peer.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

// The peer may run before SOInit has started Winsock; WSAStartup counts its
// callers, so starting it here as well is harmless.
static bool so_test_peer_startup()
{
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
// Winsock handles are small integers in practice; the tests keep them in int.
#define close(fd) closesocket((SOCKET)(fd))
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static bool so_test_peer_startup()
{
    return true;
}
#endif

int so_test_peer_listen(uint16_t* port)
{
    if (!so_test_peer_startup())
        return -1;
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0
        || getsockname(fd, (sockaddr*)&addr, &len) != 0)
    {
        close(fd);
        return -1;
    }
    *port = addr.sin_port;
    return fd;
}

int so_test_peer_accept(int listener, uint16_t* client_port, uint32_t* client_addr)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int fd = (int)accept(listener, (sockaddr*)&addr, &len);
    if (fd >= 0 && client_port != nullptr)
        *client_port = addr.sin_port;
    if (fd >= 0 && client_addr != nullptr)
        *client_addr = addr.sin_addr.s_addr;
    return fd;
}

long so_test_peer_recv(int fd, void* buf, size_t len)
{
#ifdef _WIN32
    return recv(fd, (char*)buf, (int)len, MSG_WAITALL);
#else
    return recv(fd, buf, len, MSG_WAITALL);
#endif
}

long so_test_peer_send(int fd, const void* buf, size_t len)
{
#ifdef _WIN32
    return send(fd, (const char*)buf, (int)len, 0);
#else
    return send(fd, buf, len, 0);
#endif
}

void so_test_peer_close(int fd)
{
    close(fd);
}

int so_test_peer_open_fds(void)
{
#ifdef _WIN32
    // Sockets are kernel handles: a leaked one raises the process's count.
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return (int)count;
#else
    int open = 0;
    for (int fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) != -1)
            ++open;
    return open;
#endif
}
