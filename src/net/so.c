#include <stdio.h>
#include <revolution/so.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "so_convert.h"
#include "so_host.h"
#include "so_state.h"
#include "so_resolve.h"

// Test aids for network matches on one computer; unset, nothing changes.
// RECHARGED_NET_PAIR=0 or 1: one of two instances on this computer. The
// second binds the game's ports (6767 direct, 6768 broadcast) 100 higher, and
// each sends what goes to those ports to the other's, broadcasts through the
// loopback interface (a firewall may drop those the network interface loops back).
// RECHARGED_NET_DELAY_MS=<ms>: datagrams leave that much later, give or take
// RECHARGED_NET_JITTER_MS, to play as from far away. They leave when the game
// next sends, receives or polls, every frame in a match.
// RECHARGED_NET_TRACE=1: every datagram sent and received, on stderr.
enum { NET_DIRECT_PORT = 6767, NET_BROADCAST_PORT = 6768, NET_PAIR_SHIFT = 100 };
enum { NET_DELAYED_MAX = 512, NET_DELAYED_BYTES = 1536 };

struct delayed_datagram {
    so_sys_socket fd;
    struct sockaddr_in to;
    long long due_ns;
    int len;
    unsigned char data[NET_DELAYED_BYTES];
};
// The queue is under so_backend_lock.
static struct {
    bool read;
    bool trace;
    int pair; // -1: alone
    int delay_ms, jitter_ms;
    struct delayed_datagram queue[NET_DELAYED_MAX];
    int count;
} net_test = { .pair = -1 };

static int net_test_env(const char* name, int fallback)
{
    const char* text = getenv(name);
    return text && *text ? atoi(text) : fallback;
}

static void net_test_read(void)
{
    if (net_test.read)
        return;
    net_test.pair = net_test_env("RECHARGED_NET_PAIR", -1);
    if (net_test.pair != 0 && net_test.pair != 1)
        net_test.pair = -1;
    net_test.delay_ms = net_test_env("RECHARGED_NET_DELAY_MS", 0);
    net_test.jitter_ms = net_test_env("RECHARGED_NET_JITTER_MS", 0);
    net_test.trace = net_test_env("RECHARGED_NET_TRACE", 0) != 0;
    if (net_test.delay_ms < 0)
        net_test.delay_ms = 0;
    if (net_test.jitter_ms < 0 || net_test.jitter_ms > net_test.delay_ms)
        net_test.jitter_ms = net_test.delay_ms;
    net_test.read = true;
}

static bool net_test_game_port(u16 port)
{
    return port == NET_DIRECT_PORT || port == NET_BROADCAST_PORT;
}

// A port this instance binds (network byte order).
static u16 net_test_bound_port(u16 port)
{
    net_test_read();
    if (net_test.pair < 0 || !net_test_game_port(ntohs(port)))
        return port;
    return htons((u16)(ntohs(port) + NET_PAIR_SHIFT * net_test.pair));
}

// A port this instance sends to (network byte order): the other's.
static u16 net_test_destination_port(u16 port)
{
    net_test_read();
    if (net_test.pair < 0 || !net_test_game_port(ntohs(port)))
        return port;
    return htons((u16)(ntohs(port) + NET_PAIR_SHIFT * (1 - net_test.pair)));
}

static long long net_test_now_ns(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64() * 1000000LL;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
#endif
}

// Sends the delayed datagrams that are due, in the order they fall due.
static void net_test_flush(void)
{
    net_test_read();
    if (net_test.delay_ms == 0)
        return;
    so_backend_lock();
    const long long now = net_test_now_ns();
    for (;;)
    {
        int next = -1;
        for (int i = 0; i < net_test.count; i++)
            if (net_test.queue[i].due_ns <= now && (next < 0 || net_test.queue[i].due_ns < net_test.queue[next].due_ns))
                next = i;
        if (next < 0)
            break;
        struct delayed_datagram* datagram = &net_test.queue[next];
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        sendto(datagram->fd, (const char*)datagram->data, (so_sys_len)datagram->len, flags,
               (struct sockaddr*)&datagram->to, sizeof(datagram->to));
        net_test.queue[next] = net_test.queue[--net_test.count];
    }
    so_backend_unlock();
}

// Holds a datagram back when a delay is set: true when it did.
static bool net_test_hold(so_sys_socket fd, const void* buf, int len, const struct sockaddr_in* to)
{
    net_test_read();
    if (net_test.delay_ms == 0 || to == NULL || len > NET_DELAYED_BYTES)
        return false;
    so_backend_lock();
    const bool held = net_test.count < NET_DELAYED_MAX;
    if (held)
    {
        struct delayed_datagram* datagram = &net_test.queue[net_test.count++];
        int delay = net_test.delay_ms;
        if (net_test.jitter_ms > 0)
            delay += rand() % (2 * net_test.jitter_ms + 1) - net_test.jitter_ms;
        datagram->fd = fd;
        datagram->to = *to;
        datagram->due_ns = net_test_now_ns() + (long long)delay * 1000000LL;
        datagram->len = len;
        memcpy(datagram->data, buf, (size_t)len);
    }
    so_backend_unlock();
    return held;
}

int SOInit(SOLibraryConfig* config)
{
    so_backend_lock();
    int result = SO_SUCCESS;
    switch (so_backend_state_locked())
    {
    case SO_INTERNAL_STATE_READY:
    case SO_INTERNAL_STATE_ACTIVE:
        result = SO_EALREADY;
        break;
    case SO_INTERNAL_STATE_TERMINATED:
    default:
        if (config == NULL || config->alloc == NULL || config->free == NULL)
        {
            result = SO_EINVAL;
                break;
        }
        const int error = so_sys_startup();
        if (error != 0)
        {
            result = so_error_from_native(error);
            break;
        }
        so_backend_set_allocator_locked(config->alloc, config->free);
        so_backend_set_state_locked(SO_INTERNAL_STATE_READY);
        break;
    }
    so_backend_unlock();
    return result;
}

int SOStartup(void) { return SOStartupEx(0x927c0); }

int SOStartupEx(int timeout)
{
    so_backend_lock();
    int result = SO_SUCCESS;
    switch (so_backend_state_locked())
    {
    case SO_INTERNAL_STATE_TERMINATED:
    default:
        result  = SO_ENETRESET;
        break;
    case SO_INTERNAL_STATE_ACTIVE:
        result = SO_EALREADY;
        break;
    case SO_INTERNAL_STATE_READY:
        (void)timeout;
        so_backend_set_host_id_locked(so_host_find_ipv4());
        so_backend_set_state_locked(SO_INTERNAL_STATE_ACTIVE);
        break;
    }

    so_backend_unlock();
    return result;
}

int SOCleanup(void)
{
    so_backend_lock();
    int result = SO_SUCCESS;
    switch (so_backend_state_locked())
    {
    case SO_INTERNAL_STATE_TERMINATED:
    default:
        result = SO_ENETRESET;
        break;
    case SO_INTERNAL_STATE_READY:
        result = SO_EALREADY;
        break;
    case SO_INTERNAL_STATE_ACTIVE:
        // Closing the IOS device drops every socket; do the same on the host.
        so_backend_close_all_locked();
        so_backend_set_host_id_locked(0);
        so_backend_set_state_locked(SO_INTERNAL_STATE_READY);
        break;
    }
    so_backend_unlock();
    return result;
}

int SOFinish(void)
{
    so_backend_lock();
    int result = SO_SUCCESS;
    switch (so_backend_state_locked())
    {
    case SO_INTERNAL_STATE_TERMINATED:
        result = SO_EALREADY;
        break;
    case SO_INTERNAL_STATE_READY:
        so_sys_cleanup();
        so_backend_set_state_locked(SO_INTERNAL_STATE_TERMINATED);
        break;
    case SO_INTERNAL_STATE_ACTIVE:
        result = SO_EINPROGRESS;
        break;
    default:
        break;
    }
    so_backend_unlock();
    return result;
}

int SOSocket2(int pf, int type, int protocol)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (pf == SO_PF_INET6)
        return SO_EAFNOSUPPORT;
    return so_backend_open(pf, type, protocol);
}

int SOClose(int s)
{
    so_backend_lock();
    int result = so_backend_check_active_locked();
    if (result == SO_SUCCESS)
        result = so_backend_close_locked(s);
    so_backend_unlock();
    return result;
}

int SOBind(int s, const void *sockAddr)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;

    const SOSockAddr* nlAddr = sockAddr;
    if (sockAddr == NULL
        || sizeof(SOSockAddr) < nlAddr->len
        || nlAddr->len < sizeof(SOSockAddr))
        return SO_EINVAL;

    so_sys_socket real_fd;
    result = so_backend_acquire(s, &real_fd, NULL);
    if (result != SO_SUCCESS)
        return result;

    struct sockaddr_in unixsock = to_native_sockaddr((const SOSockAddrIn*)nlAddr);
    unixsock.sin_port = net_test_bound_port(unixsock.sin_port);
    if (bind(real_fd, (struct sockaddr *)&unixsock, sizeof(unixsock)) == -1)
        result = so_last_error();
    so_backend_release(s);
    return result;
}

static int so_fcntl_native(int s, int cmd, int arg)
{
    switch (cmd)
    {
        case SO_F_GETFL:
            return so_backend_nonblocking(s) ? SO_O_NONBLOCK : SO_SUCCESS;
        case SO_F_SETFL:
            return so_backend_set_nonblocking(s, (arg & SO_O_NONBLOCK) != 0);
        default:
            return SO_EINVAL;
    }
}

int SOFcntl(int s, int cmd, ...)
{
    va_list marker;
    int arg;
    va_start(marker, cmd);
    arg = va_arg(marker, int);
    va_end(marker);

    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;

    so_sys_socket real_fd;
    result = so_backend_acquire(s, &real_fd, NULL);
    if (result != SO_SUCCESS)
        return result;
    result = so_fcntl_native(s, cmd, arg);
    so_backend_release(s);
    return result;
}

u32 SOHtoNl(u32 hostlong)
{
    return htonl(hostlong);
}

u16 SOHtoNs(u16 hostshort)
{
    return htons(hostshort);
}

u32 SONtoHl(u32 netlong)
{
    return ntohl(netlong);
}

u16 SONtoHs(u16 netshort)
{
    return ntohs(netshort);
}

static int so_send_to(int s, const void* buf, int len, int flags, const void* sockTo)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    // no game path requires flags, fail loudly instead of accepting silently
    if (flags != 0)
        return SO_EINVAL;
#ifdef MSG_NOSIGNAL
    // don't let Linux's kernel kill the game with EPIPE, let it bubble up
    // to the caller
    flags |= MSG_NOSIGNAL;
#endif
    const SOSockAddr* to = sockTo;
    if (to != NULL && (sizeof(SOSockAddr) < to->len || to->len < sizeof(SOSockAddr)))
        return SO_EINVAL;
    if (len < 0 || (len > 0 && buf == NULL))
        return SO_EINVAL;
    so_sys_socket real_fd;
    struct so_backend_io io;
    result = so_backend_acquire_io(s, SO_BACKEND_WRITE, SO_ENOTCONN, &real_fd, &io);
    if (result != SO_SUCCESS)
        return result;
    net_test_flush();
    ssize_t sent;
    if (sockTo != NULL)
    {
        struct sockaddr_in addrnative = to_native_sockaddr((const SOSockAddrIn*)to);
        addrnative.sin_port = net_test_destination_port(addrnative.sin_port);
        if (net_test.pair >= 0 && addrnative.sin_addr.s_addr == htonl(INADDR_BROADCAST))
            addrnative.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (net_test.trace)
            fprintf(stderr, "net: send %d bytes to %08x:%d\n", len, ntohl(addrnative.sin_addr.s_addr), ntohs(addrnative.sin_port));
        if (net_test_hold(real_fd, buf, len, &addrnative))
            sent = len;
        else
            sent = sendto(real_fd, buf, (so_sys_len)len, flags, (struct sockaddr *)&addrnative, sizeof(struct sockaddr_in));
    }
    else
    {
        sent = send(real_fd, buf, (so_sys_len)len, flags);
    }
    return so_backend_release_io(&io, sent == -1 ? so_last_error() : (int)sent);
}

// Received bytes land in this host buffer and are then copied into the game's.
// A recvfrom straight into game memory is written by the kernel, which fails
// (EFAULT, WSAEFAULT) instead of faulting where a rollback write-protects that
// memory to find the pages that changed. 64 KiB holds any UDP datagram; a
// stream receive larger than that returns part of the data, as recv may.
static __thread unsigned char so_receive_bounce[65536];

static int so_recv_from(int s, void* buf, int len, int flags, void* sockFrom)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (flags != 0)
        return SO_EINVAL;
    const SOSockAddr* from = sockFrom;
    if (sockFrom != NULL && (sizeof(SOSockAddr) < from->len || from->len < sizeof(SOSockAddr)))
        return SO_EINVAL;
    if (len < 0 || (len > 0 && buf == NULL))
        return SO_EINVAL;
    so_sys_socket real_fd;
    struct so_backend_io io;
    result = so_backend_acquire_io(s, SO_BACKEND_READ, SO_ENOTCONN, &real_fd, &io);
    if (result != SO_SUCCESS)
        return result;
    net_test_flush();
    ssize_t received;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    size_t capacity = (size_t)len < sizeof(so_receive_bounce) ? (size_t)len : sizeof(so_receive_bounce);
    received = recvfrom(real_fd, (char*)so_receive_bounce, (so_sys_len)capacity, 0, (struct sockaddr *)&addr, &addrlen);
    if (received > 0)
        memcpy(buf, so_receive_bounce, (size_t)received);
    if (received > 0 && net_test.trace)
        fprintf(stderr, "net: received %d bytes from %08x:%d\n", (int)received, ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port));
    result = so_backend_release_io(&io, received == -1 ? so_last_recv_result() : (int)received);
    if (result >= 0 && sockFrom != NULL)
        from_native_sockaddr(sockFrom, &addr);
    
    return result;
}

int SOSendTo(int s, const void* buf, int len, int flags, const void* sockTo)
{
    return so_send_to(s, buf, len, flags, sockTo);
}

int SOSend(int s, const void *buf, int len, int flags)
{
    return so_send_to(s, buf, len, flags, NULL);
}

int SORecv(int s, void* buf, int len, int flags)
{
    return so_recv_from(s, buf, len, flags, NULL);
}

int SORecvFrom(int s, void* buf, int len, int flags, void* sockFrom)
{
    return so_recv_from(s, buf, len, flags, sockFrom);
}

int SOGetSockName(int s, void* sockAddr)
{

    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    SOSockAddr* addr = sockAddr;
    if (addr == NULL || sizeof(SOSockAddr) < addr->len || addr->len < sizeof(SOSockAddr))
        return SO_EINVAL;
    so_sys_socket real_fd;
    result = so_backend_acquire(s, &real_fd, NULL);
    if (result != SO_SUCCESS)
        return result;
    struct sockaddr_in addrnative;
    socklen_t addrlen = sizeof(addrnative);

    if (getsockname(real_fd, (struct sockaddr*)&addrnative, &addrlen) == -1)
        result = so_last_error();
    so_backend_release(s);
    if (result == SO_SUCCESS)
        from_native_sockaddr(sockAddr, &addrnative);
    return result;
}

int SOPoll(SOPollFD fds[], unsigned nfds, OSTime timeout)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (fds == NULL)
        return SO_EINVAL;
    if (timeout > 0)
        return SO_EINVAL; // crash loudly because it should never happen
    if (nfds == 0) // prevent undefined behaviour
        return 0;
    net_test_flush();
    struct pollfd fdsnative[nfds];
    so_backend_acquire_pollfds(fdsnative, fds, nfds);
    int time = timeout < -1 ? -1 : (int)timeout;
    int cnt = so_sys_poll(fdsnative, nfds, time);
    if (cnt == -1)
        cnt = so_last_error();
    else
    {
        for (unsigned int i=0; i<nfds; i++)
        {
            fds[i].revents = fdsnative[i].fd != SO_SYS_INVALID_SOCKET ?
                 native_pollevent_to_so(fdsnative[i].revents)
                 : SO_POLLNVAL;
            if (fdsnative[i].fd == SO_SYS_INVALID_SOCKET)
                cnt++;
            // A socket SOClose closed during the wait is no longer valid.
            // Linux reports it itself; Windows reports nothing for it.
            else if (fdsnative[i].revents == 0 && so_backend_closing(fds[i].fd))
            {
                fds[i].revents = SO_POLLNVAL;
                cnt++;
            }
        }
    }
    so_backend_release_pollfds(fdsnative, fds, nfds);
    return cnt;
}

// Returns long like the SDK; on LP64 hosts that is 64 bits, so the address
// goes through u32 to avoid sign-extending addresses >= 128.0.0.0. Callers
// store it back into a u32 (NetworkSocket::GetLocalAddress, dwc_match.c).
long SOGetHostID(void)
{
    so_backend_lock();
    // The SDK returns 0 instead of an error code when the library is not up
    u32 hostId = so_backend_state_locked() == SO_INTERNAL_STATE_ACTIVE
        ? so_backend_host_id_locked()
        : 0;
    so_backend_unlock();
    return (long)hostId;
}

int SOConnect(int s, const void* sockAddr)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    const SOSockAddr* addr = sockAddr;
    if (addr == NULL || sizeof(SOSockAddr) < addr->len || addr->len < sizeof(SOSockAddr))
        return SO_EINVAL;
    so_sys_socket real_fd;
    struct so_backend_io io;
    result = so_backend_acquire_io(s, SO_BACKEND_WRITE, SO_ENETUNREACH, &real_fd, &io);
    if (result != SO_SUCCESS)
        return result;
    struct sockaddr_in addrnative = to_native_sockaddr((const SOSockAddrIn*)addr);
    addrnative.sin_port = net_test_destination_port(addrnative.sin_port);
    if (connect(real_fd, (struct sockaddr*)&addrnative, sizeof(addrnative)) == -1)
        result = so_last_connect_error();
    return so_backend_release_io(&io, result);
}


int SOListen(int s, int backlog)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    so_sys_socket real_fd;
    result = so_backend_acquire(s, &real_fd, NULL);
    if (result != SO_SUCCESS)
        return result;
    if (listen(real_fd, backlog) == -1)
        result = so_last_error();
    so_backend_release(s);
    return result;
}

int SOAccept(int s, void* sockAddr)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;

    const SOSockAddr* nlAddr = sockAddr;
    if (sockAddr != NULL
        && nlAddr->len != sizeof(SOSockAddr))
        return SO_EINVAL;

    so_sys_socket real_fd;
    struct so_backend_io io;
    result = so_backend_acquire_io(s, SO_BACKEND_WRITE, SO_EINVAL, &real_fd, &io);
    if (result != SO_SUCCESS)
        return result;
    struct sockaddr_in addr;
    socklen_t size = sizeof(addr);
    so_sys_socket new_conn = accept(real_fd, (struct sockaddr*)&addr, &size);
    result = so_backend_release_io(&io, new_conn == SO_SYS_INVALID_SOCKET ? so_last_error() : SO_SUCCESS);
    if (result >= 0)
    {
        result = so_backend_insert_accepted(new_conn);
        if (result >= 0 && sockAddr != NULL)
        {
            from_native_sockaddr(sockAddr, &addr);
        }
    }
    else if (new_conn != SO_SYS_INVALID_SOCKET)
    {
        so_sys_close(new_conn);
    }
    return result;
}


int SOShutdown(int s, int how)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    const int native_how = to_native_how(how);
    if (native_how == -1)
        return SO_EINVAL;

    so_sys_socket real_fd;
    int real_type;
    result = so_backend_acquire(s, &real_fd, &real_type);
    if (result != SO_SUCCESS)
        return result;
    // The Wii does nothing and returns 0 for UDP (Dolphin PR 8758's hwtest).
    if (real_type == SOCK_STREAM)
    {
        const bool shut_read = native_how == SHUT_RD || native_how == SHUT_RDWR;
        const bool shut_write = native_how == SHUT_WR || native_how == SHUT_RDWR;
        so_backend_record_shutdown(s, shut_read, shut_write);
        if (so_sys_shutdown(real_fd, native_how) == -1)
            result = so_last_error();
    }
    so_backend_release(s);
    return result;
}

// The SDK copies option values through a 20-byte IOS buffer.
#define SO_SOCKOPT_MAX_LEN 20

int SOSetSockOpt(int s, int level, int optname, const void* optval, int optlen)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (optlen < 0 || optlen > SO_SOCKOPT_MAX_LEN)
        return SO_EINVAL;
    int native_level, native_optname;
    if (!to_native_sockopt(level, optname, &native_level, &native_optname))
        return SO_ENOPROTOOPT;
    // A NULL value is zero-filled, as the SDK does before calling IOS.
    static const u8 zeros[SO_SOCKOPT_MAX_LEN];
    const void* native_value = optval != NULL ? optval : zeros;

    so_sys_socket real_fd;
    result = so_backend_acquire(s, &real_fd, NULL);
    if (result != SO_SUCCESS)
        return result;
    if (setsockopt(real_fd, native_level, native_optname, native_value, (socklen_t)optlen) == -1)
        result = so_last_error();
    so_backend_release(s);
    return result;
}

int SOGetSockOpt(int s, int level, int optname, void* optval, int* optlen)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    int native_level, native_optname;
    if (!to_native_sockopt(level, optname, &native_level, &native_optname))
        return SO_ENOPROTOOPT;

    so_sys_socket real_fd;
    result = so_backend_acquire(s, &real_fd, NULL);
    if (result != SO_SUCCESS)
        return result;
    u8 value[SO_SOCKOPT_MAX_LEN];
    socklen_t length = sizeof(value);
    // Winsock takes the option buffer as char*.
    if (getsockopt(real_fd, native_level, native_optname, (char*)value, &length) == -1)
        result = so_last_error();
    so_backend_release(s);
    if (result != SO_SUCCESS || optlen == NULL)
        return result;

    // As in the SDK, a short buffer gets the needed length and SO_EINVAL.
    if (*optlen < (int)length)
        result = SO_EINVAL;
    else if (optval != NULL)
        memcpy(optval, value, (size_t)length);
    *optlen = (int)length;
    return result;
}


int SOInetAtoN(const char* cp, SOInAddr* inp)
{
    so_backend_lock();
    int state = so_backend_state_locked();
    so_backend_unlock();
    if (state == SO_INTERNAL_STATE_TERMINATED)
        return SO_ENETRESET;
    if (cp == NULL)
        return SO_EINVAL;
    struct in_addr inaddr;
    int res = so_sys_inet_aton(cp, &inaddr);
    if (res > 0 && inp != NULL)
        inp->addr = inaddr.s_addr;
    return res;
}

static _Thread_local char addressBuffer[40];
char* SOInetNtoA(SOInAddr in)
{
    u8* addr = (u8*)&in;
    sprintf(addressBuffer, "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
    return addressBuffer;
}

static char online_server_override[256] = {0};

static int endswith(const char *str, const char *suffix)
{
    if (!str || !suffix)
        return 0;
    size_t lenstr = strlen(str);
    size_t lensuffix = strlen(suffix);
    if (lensuffix >  lenstr)
        return 0;
    return strncasecmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}


// The one place the SO name lookups reach the host resolver.
static int so_resolve(const char* node, const char* service, const struct addrinfo* hints,
    struct addrinfo** list)
{
    if (endswith(node, ".nintendowifi.net") && online_server_override[0] != '\0')
        node = online_server_override;
    const int error = getaddrinfo(node, service, hints, list);
    return error == 0 ? SO_SUCCESS : so_error_from_gai(error);
}

// NULL or "" clears the override. A server that does not fit the buffer is
// refused with a warning, and the previous override stays.
void so_set_server_override(const char *hijacked)
{
    if (hijacked == NULL || hijacked[0] == 0)
    {
        online_server_override[0] = 0;
        return;
    }
    const size_t length = strlen(hijacked);
    if (length >= sizeof(online_server_override))
    {
        fprintf(stderr, "Warning: online server ignored: %zu characters, at most %zu fit; keeping %s\n",
            length, sizeof(online_server_override) - 1,
            online_server_override[0] != 0 ? online_server_override : "the default servers");
        return;
    }
    memcpy(online_server_override, hijacked, length + 1);
}


// The SDK fills one 0x460-byte buffer allocated by SOInit (soWork._unk10) and
// returns it, so a result is valid until the next call. Callers only read the
// SOHostEnt fields, so this keeps a native layout instead of the IOS one. It
// is per thread: GameSpy resolves both on its resolver thread and on the main
// thread, and reads the result after the call returns.
#define SO_HOSTENT_MAX_ADDRS 16

struct so_hostent_storage
{
    SOHostEnt host;
    char name[256];
    char* aliases[1];
    u8* addr_list[SO_HOSTENT_MAX_ADDRS + 1];
    SOInAddr addrs[SO_HOSTENT_MAX_ADDRS];
};
static _Thread_local struct so_hostent_storage so_hostent;

SOHostEnt* SOGetHostByName(const char* name)
{
    if (so_backend_check_active() != SO_SUCCESS || name == NULL)
        return NULL;

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_CANONNAME,
    };
    struct addrinfo* res;
    if (so_resolve(name, NULL, &hints, &res) != SO_SUCCESS)
        return NULL;

    struct so_hostent_storage* storage = &so_hostent;
    size_t count = 0;
    for (const struct addrinfo* ai = res; ai != NULL && count < SO_HOSTENT_MAX_ADDRS; ai = ai->ai_next)
    {
        if (ai->ai_family != AF_INET)
            continue;
        // Both keep the address in network byte order.
        const struct sockaddr_in* sin = (const struct sockaddr_in*)ai->ai_addr;
        storage->addrs[count].addr = sin->sin_addr.s_addr;
        storage->addr_list[count] = (u8*)&storage->addrs[count];
        count++;
    }
    storage->addr_list[count] = NULL;

    const char* canonical = res->ai_canonname != NULL ? res->ai_canonname : name;
    snprintf(storage->name, sizeof(storage->name), "%s", canonical);
    freeaddrinfo(res);
    if (count == 0)
        return NULL;

    storage->aliases[0] = NULL;
    storage->host = (SOHostEnt){
        .name = storage->name,
        .aliases = storage->aliases,
        .addrType = SO_PF_INET,
        .length = (s16)sizeof(SOInAddr),
        .addrList = storage->addr_list,
    };
    return &storage->host;
}

// The SDK returns one 0x840-byte block from SOInit's allocator (name 10): the
// SOAddrInfo entries are consecutive and each addr points to a SOSockAddrIn
// further in the same block, so SOFreeAddrInfo frees the whole list at once.
// The IOS layout holds 35 entries.
#define SO_ADDRINFO_ALLOC_NAME 10
#define SO_ADDRINFO_MAX 35

struct so_addrinfo_block
{
    SOAddrInfo entries[SO_ADDRINFO_MAX];
    SOSockAddrIn addrs[SO_ADDRINFO_MAX];
};

int SOGetAddrInfo(const char* nodeName, const char* servName,
    const SOAddrInfo* hints, SOAddrInfo** res)
{
    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (res == NULL)
        return SO_EINVAL;
    *res = NULL;

    struct addrinfo native_hints;
    result = to_native_addrinfo_hints(hints, &native_hints);
    if (result != SO_SUCCESS)
        return result;

    struct addrinfo* native;
    result = so_resolve(nodeName, servName, &native_hints, &native);
    if (result != SO_SUCCESS)
        return result;

    struct so_addrinfo_block* block = so_backend_alloc(SO_ADDRINFO_ALLOC_NAME, (s32)sizeof(*block));
    if (block == NULL)
    {
        freeaddrinfo(native);
        return SO_ENOMEM;
    }

    size_t count = 0;
    for (const struct addrinfo* ai = native; ai != NULL && count < SO_ADDRINFO_MAX; ai = ai->ai_next)
    {
        if (ai->ai_family != AF_INET)
            continue;
        // With no socket type in the hints, POSIX's getaddrinfo returns every
        // address three times: for stream, datagram and raw sockets. SO has
        // no raw sockets, so those entries are skipped. Winsock's returns the
        // address once, with no type; it stands for both SO entries.
        int types[2] = { from_native_type(ai->ai_socktype), -1 };
        int protocols[2] = { ai->ai_protocol, 0 };
        if (ai->ai_socktype == 0)
        {
            types[0] = SO_SOCK_STREAM;
            protocols[0] = SO_IPPROTO_TCP;
            types[1] = SO_SOCK_DGRAM;
            protocols[1] = SO_IPPROTO_UDP;
        }
        for (int i = 0; i < 2 && types[i] != -1 && count < SO_ADDRINFO_MAX; i++)
        {
            SOSockAddrIn* addr = &block->addrs[count];
            from_native_sockaddr(addr, (const struct sockaddr_in*)ai->ai_addr);
            // The SDK never relocates canonName into the block; no caller reads it.
            block->entries[count] = (SOAddrInfo){
                .family = SO_PF_INET,
                .sockType = types[i],
                .protocol = protocols[i],
                .addrLen = sizeof(SOSockAddrIn),
                .canonName = NULL,
                .addr = addr,
                .next = NULL,
            };
            if (count > 0)
                block->entries[count - 1].next = &block->entries[count];
            count++;
        }
    }
    freeaddrinfo(native);

    if (count == 0)
    {
        so_backend_free(SO_ADDRINFO_ALLOC_NAME, block, (s32)sizeof(*block));
        return SO_EHOSTUNREACH;
    }
    *res = block->entries;
    return SO_SUCCESS;
}

void SOFreeAddrInfo(SOAddrInfo* head)
{
    so_backend_free(SO_ADDRINFO_ALLOC_NAME, head, (s32)sizeof(struct so_addrinfo_block));
}

int SOGetInterfaceOpt(IPInterface* interface, int level, int optname,
                        void* optval, int* optlen)
{
    (void)interface;
    so_backend_lock();
    int state = so_backend_state_locked();
    so_backend_unlock();
    if (state == SO_INTERNAL_STATE_TERMINATED)
        return SO_ENETRESET;
    if (optname == SO_CONFIG_FILTER_INPUT || optname == SO_CONFIG_FILTER_OUTPUT)
        return SO_EINVAL;
    if (optlen == NULL)
        return SO_EINVAL;
    if (level != SO_SOL_CONFIG)
        return SO_ENOPROTOOPT;
    switch (optname)
    {
        case SO_CONFIG_IP_ADDR_NUMBER:
            if (*optlen >= (long)sizeof(int))
            {
                if (optval != NULL)
                    *(s32*)optval = 1;
                *optlen = sizeof(int);
            }
            else
            {
                *optlen = sizeof(int);
                return SO_EINVAL;
            }
            break;
        case SO_CONFIG_IP_ADDR_TABLE:
            if (*optlen >= (long)sizeof(IPAddrEntry))
            {
                if (optval != NULL)
                {
                    IPAddrEntry* ipaddr = optval;
                    struct so_host_ipv4 ipaddr_native;
                    so_host_find_ipv4_info(&ipaddr_native);
                    memcpy(ipaddr->addr, &ipaddr_native.addr, sizeof(u32));
                    memcpy(ipaddr->bcastAddr, &ipaddr_native.broadcast, sizeof(u32));
                    memcpy(ipaddr->netMask, &ipaddr_native.netmask, sizeof(u32));
                }
                *optlen = sizeof(IPAddrEntry);
            }
            else
            {
                *optlen = sizeof(IPAddrEntry);
                return SO_EINVAL;
            }
            break;
        default: return SO_ENOPROTOOPT;
    }
    return SO_SUCCESS;
}
