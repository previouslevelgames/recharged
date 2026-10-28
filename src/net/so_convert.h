#ifndef CHARGED_NET_SO_CONVERT_H
#define CHARGED_NET_SO_CONVERT_H

#include "so_sys.h"

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

static inline short so_pollevent_to_native(int event)
{
    short res = 0;
    if (event & SO_POLLRDNORM)
        res |= POLLIN;
    if (event & SO_POLLRDBAND)
        res |= POLLRDBAND;
#ifndef _WIN32
    // WSAPoll fails the whole call with WSAEINVAL when asked for POLLPRI.
    if (event & SO_POLLPRI)
        res |= POLLPRI;
#endif
    if (event & SO_POLLWRNORM)
        res |= POLLOUT;
    if (event & SO_POLLWRBAND)
        res |= POLLWRBAND;
    return res;
}

static inline int native_pollevent_to_so(short event)
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

// Only the options the game's libraries use. The SDK numbers name other
// options on Linux (SO_SO_RCVBUF is 0x1002, SO_RCVBUF is 8), so an unknown
// option must fail instead of reaching the host.
static inline bool to_native_sockopt(int level, int optname, int* native_level, int* native_optname)
{
    if (level != SO_SOL_SOCKET)
        return false;
    *native_level = SOL_SOCKET;
    switch (optname)
    {
    case SO_SO_SNDBUF:
        *native_optname = SO_SNDBUF;
        return true;
    case SO_SO_RCVBUF:
        *native_optname = SO_RCVBUF;
        return true;
    default:
        return false;
    }
}

#ifndef _WIN32
static inline int so_error_from_errno(int error)
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
    case ENOPROTOOPT: return SO_ENOPROTOOPT;
    case ENOTCONN: return SO_ENOTCONN;
    case ENOTSOCK: return SO_ENOTSOCK;
    case EOPNOTSUPP: return SO_EOPNOTSUPP;
    case EPIPE: return SO_EPIPE;
    case EPROTONOSUPPORT: return SO_EPROTONOSUPPORT;
    case EPROTOTYPE: return SO_EPROTOTYPE;
    default: return SO_EFATAL;
    }
}
#else
// The Winsock counterparts of the errno table above. A send after shutdown
// fails with WSAESHUTDOWN where POSIX reports EPIPE.
static inline int so_error_from_wsa(int error)
{
    switch (error)
    {
    case WSAEWOULDBLOCK: return SO_EAGAIN;
    case WSAEACCES: return SO_EACCES;
    case WSAEADDRINUSE: return SO_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return SO_EADDRNOTAVAIL;
    case WSAEAFNOSUPPORT: return SO_EAFNOSUPPORT;
    case WSAEALREADY: return SO_EALREADY;
    case WSAEBADF: return SO_EBADF;
    case WSAECONNABORTED: return SO_ECONNABORTED;
    case WSAECONNREFUSED: return SO_ECONNREFUSED;
    case WSAECONNRESET: return SO_ECONNRESET;
    case WSAEFAULT: return SO_EFAULT;
    case WSAEHOSTUNREACH: return SO_EHOSTUNREACH;
    case WSAEINPROGRESS: return SO_EINPROGRESS;
    case WSAEINTR: return SO_EINTR;
    case WSAEINVAL: return SO_EINVAL;
    case WSAEISCONN: return SO_EISCONN;
    case WSAEMFILE: return SO_EMFILE;
    case WSAEMSGSIZE: return SO_EMSGSIZE;
    case WSAENETDOWN: return SO_ENETDOWN;
    case WSAENETUNREACH: return SO_ENETUNREACH;
    case WSAENOBUFS: return SO_ENOBUFS;
    case WSA_NOT_ENOUGH_MEMORY: return SO_ENOMEM;
    case WSAENOPROTOOPT: return SO_ENOPROTOOPT;
    case WSAENOTCONN: return SO_ENOTCONN;
    case WSAENOTSOCK: return SO_ENOTSOCK;
    case WSAEOPNOTSUPP: return SO_EOPNOTSUPP;
    case WSAESHUTDOWN: return SO_EPIPE;
    case WSAEPROTONOSUPPORT: return SO_EPROTONOSUPPORT;
    case WSAEPROTOTYPE: return SO_EPROTOTYPE;
    default: return SO_EFATAL;
    }
}
#endif

static inline int so_error_from_native(int error)
{
#ifdef _WIN32
    return so_error_from_wsa(error);
#else
    return so_error_from_errno(error);
#endif
}

// The SO error for the last failed host socket call.
static inline int so_last_error(void)
{
    return so_error_from_native(so_sys_last_error());
}

// What a failed receive returns. After a read shutdown POSIX's recv reports
// the end of the stream (0) where Winsock's fails with WSAESHUTDOWN.
static inline int so_last_recv_result(void)
{
    const int error = so_sys_last_error();
#ifdef _WIN32
    if (error == WSAESHUTDOWN)
        return 0;
#endif
    return so_error_from_native(error);
}

// A non-blocking connect reports EINPROGRESS on POSIX but WSAEWOULDBLOCK on
// Windows; the SDK's callers expect SO_EINPROGRESS.
static inline int so_last_connect_error(void)
{
    const int error = so_sys_last_error();
#ifdef _WIN32
    if (error == WSAEWOULDBLOCK)
        return SO_EINPROGRESS;
#endif
    return so_error_from_native(error);
}

static inline int from_native_type(int type)
{
    switch (type)
    {
    case SOCK_STREAM:
        return SO_SOCK_STREAM;
    case SOCK_DGRAM:
        return SO_SOCK_DGRAM;
    default:
        return -1;
    }
}

// SOGetAddrInfo's hints. The SDK sends a zero family as SO_PF_INET and
// refuses SO_PF_INET6; the backend is IPv4 only. SO_IPPROTO_TCP and
// SO_IPPROTO_UDP are the IANA numbers the host uses. so.h names no AI_* flags
// and no caller sets any, so none are passed on.
static inline int to_native_addrinfo_hints(const SOAddrInfo* hints, struct addrinfo* native)
{
    *native = (struct addrinfo){ .ai_family = AF_INET };
    if (hints == NULL)
        return SO_SUCCESS;
    if (hints->family == SO_PF_INET6)
        return SO_EPROTONOSUPPORT;
    if (hints->family != 0 && hints->family != SO_PF_INET)
        return SO_EAFNOSUPPORT;
    if (hints->sockType != 0)
    {
        native->ai_socktype = to_native_type(hints->sockType);
        if (native->ai_socktype == -1)
            return SO_EPROTOTYPE;
    }
    native->ai_protocol = hints->protocol;
    return SO_SUCCESS;
}

// getaddrinfo reports its own EAI_* codes. The game's callers only test for
// 0, so these keep the SO meaning closest to each failure.
static inline int so_error_from_gai(int error)
{
    switch (error)
    {
    case EAI_AGAIN: return SO_EAGAIN;
    case EAI_FAMILY: return SO_EAFNOSUPPORT;
    case EAI_MEMORY: return SO_ENOMEM;
    case EAI_SOCKTYPE: return SO_EPROTOTYPE;
    case EAI_SERVICE: return SO_EINVAL;
    case EAI_BADFLAGS: return SO_EINVAL;
#ifdef EAI_SYSTEM
    case EAI_SYSTEM: return so_error_from_errno(errno);
#endif
    default: return SO_EHOSTUNREACH;
    }
}

// so.h names no shutdown modes: callers pass the BSD numbers 0, 1 and 2.
static inline int to_native_how(int how)
{
    switch (how)
    {
    case 0:
        return SHUT_RD;
    case 1:
        return SHUT_WR;
    case 2:
        return SHUT_RDWR;
    default:
        return -1;
    }
}

#endif // CHARGED_NET_SO_CONVERT_H
