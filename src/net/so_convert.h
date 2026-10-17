#ifndef CHARGED_NET_SO_CONVERT_H
#define CHARGED_NET_SO_CONVERT_H

#include <revolution/so.h>
#include <errno.h>
#include <sys/socket.h> // needs to be imported after so.h to avoid
                        // redefinitions of symbols SO_BROADCAST
#include <netinet/in.h>
#include <poll.h>

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
    if (event & SO_POLLPRI)
        res |= POLLPRI;
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
    case ENOTCONN: return SO_ENOTCONN;
    case ENOTSOCK: return SO_ENOTSOCK;
    case EOPNOTSUPP: return SO_EOPNOTSUPP;
    case EPIPE: return SO_EPIPE;
    case EPROTONOSUPPORT: return SO_EPROTONOSUPPORT;
    case EPROTOTYPE: return SO_EPROTOTYPE;
    default: return SO_EFATAL;
    }
}

#endif // CHARGED_NET_SO_CONVERT_H
