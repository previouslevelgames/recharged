#ifndef CHARGED_NET_SO_SYS_H
#define CHARGED_NET_SO_SYS_H

// The host socket API the SO backend calls: POSIX sockets, or Winsock on
// Windows. Only what differs between the two goes through here; the rest
// (bind, sendto, getaddrinfo, ...) has the same name and meaning on both.
// Failures report the host's own error codes; so_convert.h maps them to SO.

#include <revolution/so.h>
#include <stdbool.h>

// The host headers come after so.h, whose SO_BROADCAST they redefine.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
typedef SOCKET so_sys_socket;
#define SO_SYS_INVALID_SOCKET INVALID_SOCKET
// Winsock takes buffer lengths as int, names no sa_family_t and keeps the
// family of a sockaddr_in in a short.
typedef int so_sys_len;
typedef short sa_family_t;
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#else
typedef int so_sys_socket;
#define SO_SYS_INVALID_SOCKET (-1)
typedef size_t so_sys_len;
#endif

// The error of the last failed host socket call: errno, or WSAGetLastError.
static inline int so_sys_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// SOInit and SOFinish bracket the host library: Winsock needs WSAStartup
// before any call, including name lookups. Returns 0 or a host error.
static inline int so_sys_startup(void)
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
#else
    return 0;
#endif
}

static inline void so_sys_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static inline int so_sys_close(so_sys_socket sock)
{
#ifdef _WIN32
    return closesocket(sock);
#else
    return close(sock);
#endif
}

// Returns 0, or -1 with so_sys_last_error set. Windows cannot read the mode
// back, so the backend keeps it in its socket table.
static inline int so_sys_set_nonblocking(so_sys_socket sock, bool nonblocking)
{
#ifdef _WIN32
    u_long mode = nonblocking ? 1 : 0;
    return ioctlsocket(sock, (long)FIONBIO, &mode) == SOCKET_ERROR ? -1 : 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1)
        return -1;
    flags = nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return fcntl(sock, F_SETFL, flags);
#endif
}

// shutdown(), which also ends the calls other threads are blocked in on the
// socket. POSIX's shutdown() wakes them itself (Linux also on datagram
// sockets, macOS only on streams). Winsock's does not: its blocking calls are
// overlapped I/O, which CancelIoEx ends with WSA_OPERATION_ABORTED. Winsock
// also refuses to shut a listening socket down (WSAENOTCONN), which POSIX
// accepts. Returns 0, or -1 with so_sys_last_error set.
static inline int so_sys_shutdown(so_sys_socket sock, int how)
{
#ifdef _WIN32
    int result = shutdown(sock, how);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (error == WSAENOTCONN)
    {
        BOOL listening = FALSE;
        int length = sizeof(listening);
        if (getsockopt(sock, SOL_SOCKET, SO_ACCEPTCONN, (char*)&listening, &length) == 0 && listening)
            result = 0;
    }
    CancelIoEx((HANDLE)sock, NULL);
    WSASetLastError(error);
    return result == SOCKET_ERROR ? -1 : 0;
#else
    return shutdown(sock, how);
#endif
}

// Wakes the threads blocked on the socket without closing it, so its handle
// cannot be reused while they still hold it.
static inline void so_sys_wake(so_sys_socket sock)
{
    so_sys_shutdown(sock, SHUT_RDWR);
}

// A WSAPoll that so_sys_wake cancelled fails with WSA_OPERATION_ABORTED;
// polling again without waiting reports what is ready, as poll() does when
// shutdown() wakes it.
static inline int so_sys_poll(struct pollfd* fds, unsigned int nfds, int timeout)
{
#ifdef _WIN32
    const int count = WSAPoll(fds, nfds, timeout);
    if (count == SOCKET_ERROR && WSAGetLastError() == WSA_OPERATION_ABORTED)
        return WSAPoll(fds, nfds, 0);
    return count;
#else
    return poll(fds, nfds, timeout);
#endif
}

// inet_aton, which Windows lacks. Its inet_addr reads the same forms but
// returns INADDR_NONE both for errors and for the broadcast address.
static inline int so_sys_inet_aton(const char* cp, struct in_addr* inp)
{
#ifdef _WIN32
    const unsigned long addr = inet_addr(cp);
    if (addr == INADDR_NONE && strcmp(cp, "255.255.255.255") != 0)
        return 0;
    inp->s_addr = addr;
    return 1;
#else
    return inet_aton(cp, inp);
#endif
}

#endif // CHARGED_NET_SO_SYS_H
