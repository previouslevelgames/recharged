#include <revolution/so.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/socket.h> // needs to be imported after so.h to avoid
                        // redefinitions of symbols SO_BROADCAST
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>

#include "so_convert.h"
#include "so_host.h"
#include "so_state.h"

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

    int real_fd;
    result = so_backend_acquire(s, &real_fd);
    if (result != SO_SUCCESS)
        return result;

    struct sockaddr_in unixsock = to_native_sockaddr((const SOSockAddrIn*)nlAddr);
    if (bind(real_fd, (struct sockaddr *)&unixsock, sizeof(unixsock)) == -1)
        result = so_error_from_errno(errno);
    so_backend_release(s);
    return result;
}

static int so_fcntl_native(int fd, int cmd, int arg)
{
    int flags;
    switch (cmd)
    {
        case SO_F_GETFL:
            flags = fcntl(fd, F_GETFL, 0);
            if (flags == -1)
                return so_error_from_errno(errno);
            if (flags & O_NONBLOCK)
                return SO_O_NONBLOCK;
            return SO_SUCCESS;
        case SO_F_SETFL:
            flags = fcntl(fd, F_GETFL, 0);
            if (flags == -1)
                return so_error_from_errno(errno);
            if (arg & SO_O_NONBLOCK)
                flags |= O_NONBLOCK;
            else
                flags &= ~O_NONBLOCK;
            if (fcntl(fd, F_SETFL, flags) == -1)
                return so_error_from_errno(errno);
            return SO_SUCCESS;
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

    int real_fd;
    result = so_backend_acquire(s, &real_fd);
    if (result != SO_SUCCESS)
        return result;
    result = so_fcntl_native(real_fd, cmd, arg);
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
    int real_fd;
    result = so_backend_acquire(s, &real_fd);
    if (result != SO_SUCCESS)
        return result;
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
    return so_backend_release_io(s, sent == -1 ? so_error_from_errno(errno) : (int)sent);
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
    int real_fd;
    result = so_backend_acquire(s, &real_fd);
    if (result != SO_SUCCESS)
        return result;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t received = recvfrom(real_fd, buf, (size_t)len, 0, (struct sockaddr *)&addr, &addrlen);
    result = so_backend_release_io(s, received == -1 ? so_error_from_errno(errno) : (int)received);
    if (result >= 0 && sockFrom != NULL)
        from_native_sockaddr(sockFrom, &addr);
    return result;
}

int SOGetSockName(int s, void* sockAddr)
{

    int result = so_backend_check_active();
    if (result != SO_SUCCESS)
        return result;
    SOSockAddr* addr = sockAddr;
    if (addr == NULL || sizeof(SOSockAddr) < addr->len || addr->len < sizeof(SOSockAddr))
        return SO_EINVAL;
    int real_fd;
    result = so_backend_acquire(s, &real_fd);
    if (result != SO_SUCCESS)
        return result;
    struct sockaddr_in addrnative;
    socklen_t addrlen = sizeof(addrnative);

    if (getsockname(real_fd, (struct sockaddr*)&addrnative, &addrlen) == -1)
        result = so_error_from_errno(errno);
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
    struct pollfd fdsnative[nfds];
    so_backend_acquire_pollfds(fdsnative, fds, nfds);
    int time = timeout < -1 ? -1 : (int)timeout;
    int cnt = poll(fdsnative, nfds, time);
    if (cnt == -1)
        cnt = so_error_from_errno(errno);
    else
    {
        for (unsigned int i=0; i<nfds; i++)
        {
            fds[i].revents = fdsnative[i].fd != -1 ?
                 native_pollevent_to_so(fdsnative[i].revents)
                 : SO_POLLNVAL;
            if (fdsnative[i].fd == -1)
                cnt++;
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
    int real_fd;
    result = so_backend_acquire(s, &real_fd);
    if (result != SO_SUCCESS)
        return result;
    struct sockaddr_in addrnative = to_native_sockaddr((const SOSockAddrIn*)addr);
    if (connect(real_fd, (struct sockaddr*)&addrnative, sizeof(addrnative)) == -1)
        result = so_error_from_errno(errno);
    return so_backend_release_io(s, result);
}
