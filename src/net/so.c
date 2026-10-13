#include <revolution/so.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/socket.h> // needs to be imported after so.h to avoid
                        // redefinitions of symbols SO_BROADCAST
#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>

#include "so_host.h"

#define SO_BACKEND_MAX_SOCKET 32

static inline sa_family_t to_native_family(int family)
{
    switch (family)
    {
    case SO_PF_INET:
        return AF_INET;
    case SO_PF_INET6:
        return AF_INET6;
    default:
        return (sa_family_t)-1;
    }
}

static inline u8 from_native_family(sa_family_t family)
{
    switch (family)
    {
    case AF_INET:
        return SO_PF_INET;
    case AF_INET6:
        return SO_PF_INET6;
    default:
        return (u8)-1;
    }
}

static inline int to_native_type(int type)
{
    switch (type)
    {
    case SO_SOCK_STREAM:
        return SOCK_STREAM;
    case SO_SOCK_DGRAM:
        return SOCK_DGRAM;
    default:
        return -1;
    }
}

static inline struct sockaddr_in to_native_sockaddr(const SOSockAddrIn* nladdr)
{
    struct sockaddr_in addr = {
        .sin_family = to_native_family(nladdr->family),
        .sin_port = nladdr->port,
        .sin_addr = { .s_addr=nladdr->addr.addr }
    };
    return addr;
}

static inline void from_native_sockaddr(SOSockAddrIn* addr_dst, const struct sockaddr_in* addr_src)
{
    addr_dst->len = sizeof(SOSockAddrIn);
    addr_dst->addr.addr = addr_src->sin_addr.s_addr;
    addr_dst->family = from_native_family(addr_src->sin_family);
    addr_dst->port = addr_src->sin_port;
}

struct socketlist {
    int list[SO_BACKEND_MAX_SOCKET];
    bool occupied[SO_BACKEND_MAX_SOCKET];
};
static struct socketlist sockets = {0};

static int trsock_get_free_space()
{
    int i;
    for (i=0; i<SO_BACKEND_MAX_SOCKET; i++)
        if (!sockets.occupied[i])
            break;
    return i < SO_BACKEND_MAX_SOCKET ? i : -1;
}

static int trsock_new_socket(int fd)
{
    int idx = trsock_get_free_space();
    if (idx < 0 || idx >= SO_BACKEND_MAX_SOCKET)
        return SO_EMFILE;
    sockets.list[idx] = fd;
    sockets.occupied[idx] = true;
    return idx;
}

static int trsock_get_fd(int trsock)
{
    if (trsock < 0 || trsock >= SO_BACKEND_MAX_SOCKET)
        return -1;
    return sockets.occupied[trsock] ? sockets.list[trsock] : -1;
}

static int trsock_drop_fd(int trsock)
{
    int fd = trsock_get_fd(trsock);
    if (fd >= 0)
        sockets.occupied[trsock] = false;
    return fd;
}

static short so_pollevent_to_native(int event)
{
    short res = 0;
    if (event & SO_POLLRDNORM)
        res |= POLLIN;
    if (event & SO_POLLRDBAND)
        res |= POLLRDBAND;
    if (event & SO_POLLPRI)
        res |= POLLPRI;
    if (event & SO_POLLWRNORM)
        res |= POLLOUT;
    if (event & SO_POLLWRBAND)
        res |= POLLWRBAND;
    return res;
}

static int native_pollevent_to_so(short event)
{
    int res = 0;
    if (event & (POLLIN | POLLRDNORM))
        res |= SO_POLLRDNORM;
    if (event & POLLRDBAND)
        res |= SO_POLLRDBAND;
    if (event & POLLPRI)
        res |= SO_POLLPRI;
    if (event & (POLLOUT | POLLWRNORM))
        res |= SO_POLLWRNORM;
    if (event & POLLWRBAND)
        res |= SO_POLLWRBAND;
    if (event & POLLERR)
        res |= SO_POLLERR;
    if (event & POLLHUP)
        res |= SO_POLLHUP;
    if (event & POLLNVAL)
        res |= SO_POLLNVAL;
    return res;
}

static inline void to_native_pollfds(struct pollfd* dst, const SOPollFD* src, unsigned int n)
{
    for (unsigned int i=0; i<n; i++)
    {
        dst[i].fd = trsock_get_fd(src[i].fd);
        dst[i].events = so_pollevent_to_native(src[i].events);
        dst[i].revents = 0;
    }

}

static u8 soState = SO_INTERNAL_STATE_TERMINATED;

static u32 soHostId = 0;

static int so_check_active(void)
{
    switch (soState)
    {
    case SO_INTERNAL_STATE_ACTIVE:
        return SO_SUCCESS;
    case SO_INTERNAL_STATE_READY:
        return SO_EINVAL;
    case SO_INTERNAL_STATE_TERMINATED:
    default:
        return SO_ENETRESET;
    }
}

static int so_error_from_errno(int error)
{
    switch (error)
    {
    case EAGAIN: return SO_EAGAIN;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return SO_EAGAIN;
#endif
    case EACCES: return SO_EACCES;
    case EADDRINUSE: return SO_EADDRINUSE;
    case EADDRNOTAVAIL: return SO_EADDRNOTAVAIL;
    case EAFNOSUPPORT: return SO_EAFNOSUPPORT;
    case EALREADY: return SO_EALREADY;
    case EBADF: return SO_EBADF;
    case ECONNABORTED: return SO_ECONNABORTED;
    case ECONNREFUSED: return SO_ECONNREFUSED;
    case ECONNRESET: return SO_ECONNRESET;
    case EFAULT: return SO_EFAULT;
    case EHOSTUNREACH: return SO_EHOSTUNREACH;
    case EINPROGRESS: return SO_EINPROGRESS;
    case EINTR: return SO_EINTR;
    case EINVAL: return SO_EINVAL;
    case EISCONN: return SO_EISCONN;
    case EMFILE: return SO_EMFILE;
    case EMSGSIZE: return SO_EMSGSIZE;
    case ENETDOWN: return SO_ENETDOWN;
    case ENETUNREACH: return SO_ENETUNREACH;
    case ENFILE: return SO_ENFILE;
    case ENOBUFS: return SO_ENOBUFS;
    case ENOMEM: return SO_ENOMEM;
    case ENOTCONN: return SO_ENOTCONN;
    case ENOTSOCK: return SO_ENOTSOCK;
    case EOPNOTSUPP: return SO_EOPNOTSUPP;
    case EPIPE: return SO_EPIPE;
    case EPROTONOSUPPORT: return SO_EPROTONOSUPPORT;
    case EPROTOTYPE: return SO_EPROTOTYPE;
    default: return SO_EFATAL;
    }
}

int SOInit(SOLibraryConfig* config) 
{
    int result = SO_SUCCESS;
    switch (soState)
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
        soState = SO_INTERNAL_STATE_READY;
        break;
    }
    return result;
}

int SOStartup(void) { return SOStartupEx(0x927c0); }

int SOStartupEx(int timeout)
{
    int result = SO_SUCCESS;
    switch (soState)
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
        soHostId = so_host_find_ipv4();
        soState = SO_INTERNAL_STATE_ACTIVE;
        break;
    }

    return result;
}

int SOCleanup(void)
{
    int result = SO_SUCCESS;
    switch (soState)
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
        for (int i = 0; i < SO_BACKEND_MAX_SOCKET; i++)
        {
            int fd = trsock_drop_fd(i);
            if (fd >= 0)
                close(fd);
        }
        soHostId = 0;
        soState = SO_INTERNAL_STATE_READY;
        break;
    }
    return result;
}

int SOFinish(void)
{
int result = SO_SUCCESS;
    switch (soState)
    {
    case SO_INTERNAL_STATE_TERMINATED:
        result = SO_EALREADY;
        break;
    case SO_INTERNAL_STATE_READY:
        soState = SO_INTERNAL_STATE_TERMINATED;
        break;
    case SO_INTERNAL_STATE_ACTIVE:
        result = SO_EINPROGRESS;
        break;
    default:
        break;
    }
    return result;
}

int SOSocket2(int pf, int type, int protocol)
{
    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (pf == SO_PF_INET6)
        return SO_EAFNOSUPPORT;
    if (trsock_get_free_space() < 0)
        return SO_EMFILE;

    // protocol is safe since it's always called with protocol=0
    int sock = socket(to_native_family(pf), to_native_type(type), protocol);
    if (sock < 0)
        return so_error_from_errno(errno);
#ifndef MSG_NOSIGNAL
    int optval = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)) < 0)
    {
        close(sock);
        return so_error_from_errno(errno);
    }
#endif
    if (type == SO_SOCK_DGRAM)
    {
        int enable = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0)
        {
            close(sock);
            return so_error_from_errno(errno);
        }
    }
    return trsock_new_socket(sock);
}

int SOClose(int s)
{
    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;

    int real_fd = trsock_drop_fd(s);
    if (real_fd < 0)
        return SO_EBADF;
    if (close(real_fd) == -1)
        return so_error_from_errno(errno);
    return SO_SUCCESS;
}

int SOBind(int s, const void *sockAddr)
{
    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;

    const SOSockAddr* nlAddr = sockAddr;
    if (sockAddr == NULL
        || sizeof(SOSockAddr) < nlAddr->len
        || nlAddr->len < sizeof(SOSockAddr))
        return SO_EINVAL;

    int real_fd = trsock_get_fd(s);
    if (real_fd < 0)
        return SO_EBADF;

    struct sockaddr_in unixsock = to_native_sockaddr((const SOSockAddrIn*)nlAddr);
    if (bind(real_fd, (struct sockaddr *)&unixsock, sizeof(unixsock)) == -1)
        return so_error_from_errno(errno);
    return SO_SUCCESS;
}

int SOFcntl(int s, int cmd, ...)
{
    va_list marker;
    int arg;
    va_start(marker, cmd);
    arg = va_arg(marker, int);
    va_end(marker);

    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;

    int real_fd = trsock_get_fd(s);
    if (real_fd < 0)
        return SO_EBADF;

    int flags;
    switch (cmd)
    {
        case SO_F_GETFL:
            flags = fcntl(real_fd, F_GETFL, 0);
            if (flags == -1)
                return so_error_from_errno(errno);
            if (flags & O_NONBLOCK)
                return SO_O_NONBLOCK;
            return SO_SUCCESS;
        case SO_F_SETFL:
            flags = fcntl(real_fd, F_GETFL, 0);
            if (flags == -1)
                return so_error_from_errno(errno);
            if (arg & SO_O_NONBLOCK)
                flags |= O_NONBLOCK;
            else
                flags &= ~O_NONBLOCK;
            int ret = fcntl(real_fd, F_SETFL, flags);
            if (ret == -1)
                return so_error_from_errno(errno);
            return SO_SUCCESS;
        default:
            return SO_EINVAL;
    }
    (void)arg;
    (void)cmd;
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
    int result = so_check_active();
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
    int real_fd = trsock_get_fd(s);
    if (real_fd == -1)
        return SO_EBADF;
    ssize_t sent;
    if (sockTo != NULL)
    {
        struct sockaddr_in addrnative = to_native_sockaddr((const SOSockAddrIn*)to);
        sent = sendto(real_fd, buf, (size_t)len, flags, (struct sockaddr *)&addrnative, sizeof(struct sockaddr_in));
    }
    else
    {
        sent = send(real_fd, buf, (size_t)len, flags);
    }
    if (sent == -1)
        return so_error_from_errno(errno);
    return (int)sent;
}

int SOSendTo(int s, const void* buf, int len, int flags, const void* sockTo)
{
    return so_send_to(s, buf, len, flags, sockTo);
}

int SOSend(int s, const void *buf, int len, int flags)
{
    return so_send_to(s, buf, len, flags, NULL);
}

int SORecvFrom(int s, void* buf, int len, int flags, void* sockFrom)
{
    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (flags != 0)
        return SO_EINVAL;
    const SOSockAddr* from = sockFrom;
    if (sockFrom != NULL && (sizeof(SOSockAddr) < from->len || from->len < sizeof(SOSockAddr)))
        return SO_EINVAL;
    if (len < 0 || (len > 0 && buf == NULL))
        return SO_EINVAL;
    int real_fd = trsock_get_fd(s);
    if (real_fd == -1)
        return SO_EBADF;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t received = recvfrom(real_fd, buf, (size_t)len, 0, (struct sockaddr *)&addr, &addrlen);
    if (received == -1)
        return so_error_from_errno(errno);
    if (sockFrom != NULL)
        from_native_sockaddr(sockFrom, &addr);
    return (int)received;
}

int SOGetSockName(int s, void* sockAddr)
{

    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;
    SOSockAddr* addr = sockAddr;
    if (addr == NULL || sizeof(SOSockAddr) < addr->len || addr->len < sizeof(SOSockAddr))
        return SO_EINVAL;
    int real_fd = trsock_get_fd(s);
    if (real_fd == -1)
        return SO_EBADF;
    struct sockaddr_in addrnative;
    socklen_t addrlen = sizeof(addrnative);

    int res = getsockname(real_fd, (struct sockaddr*)&addrnative, &addrlen);
    if (res == -1)
        return so_error_from_errno(errno);
    from_native_sockaddr(sockAddr, &addrnative);
    return SO_SUCCESS;
}

int SOPoll(SOPollFD fds[], unsigned nfds, OSTime timeout)
{
    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;
    if (fds == NULL)
        return SO_EINVAL;
    if (timeout > 0)
        return SO_EINVAL; // crash loudly because it should never happen
    if (nfds == 0) // prevent undefined behaviour
        return 0;
    struct pollfd fdsnative[nfds];
    to_native_pollfds(fdsnative, fds, nfds);
    int time = timeout < -1 ? -1 : (int)timeout;
    int cnt = poll(fdsnative, nfds, time);
    if (cnt == -1)
        return so_error_from_errno(errno);
    for (unsigned int i=0; i<nfds; i++)
    {
        fds[i].revents = fdsnative[i].fd != -1 ?
             native_pollevent_to_so(fdsnative[i].revents)
             : SO_POLLNVAL;
        if (fdsnative[i].fd == -1)
            cnt++;
    }
    return cnt;
}

// Returns long like the SDK; on LP64 hosts that is 64 bits, so the address
// goes through u32 to avoid sign-extending addresses >= 128.0.0.0. Callers
// store it back into a u32 (NetworkSocket::GetLocalAddress, dwc_match.c).
long SOGetHostID(void)
{
    // The SDK returns 0 instead of an error code when the library is not up
    if (soState != SO_INTERNAL_STATE_ACTIVE)
        return 0;
    return (long)soHostId;
}

int SOConnect(int s, const void* sockAddr)
{
    int result = so_check_active();
    if (result != SO_SUCCESS)
        return result;
    const SOSockAddr* addr = sockAddr;
    if (addr == NULL || sizeof(SOSockAddr) < addr->len || addr->len < sizeof(SOSockAddr))
        return SO_EINVAL;
    int real_fd = trsock_get_fd(s);
    if (real_fd == -1)
        return SO_EBADF;
    struct sockaddr_in addrnative = to_native_sockaddr((const SOSockAddrIn*)addr);
    int ret = connect(real_fd, (struct sockaddr*)&addrnative, sizeof(addrnative));
    if (ret == -1)
        return so_error_from_errno(errno);
    return SO_SUCCESS;
}
