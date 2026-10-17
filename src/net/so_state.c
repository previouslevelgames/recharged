#include "so_state.h"

#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

#include "so_convert.h"

#define SO_BACKEND_MAX_SOCKET 32

enum slot_state { SLOT_FREE, SLOT_OPEN, SLOT_CLOSING };
struct slot { int fd, refs; enum slot_state state; };
static struct slot slots[SO_BACKEND_MAX_SOCKET];
static pthread_mutex_t so_lock = PTHREAD_MUTEX_INITIALIZER;

static u8 soState = SO_INTERNAL_STATE_TERMINATED;

static u32 soHostId = 0;

void so_backend_lock(void)
{
    pthread_mutex_lock(&so_lock);
}

void so_backend_unlock(void)
{
    pthread_mutex_unlock(&so_lock);
}

u8 so_backend_state_locked(void)
{
    return soState;
}

void so_backend_set_state_locked(u8 state)
{
    soState = state;
}

u32 so_backend_host_id_locked(void)
{
    return soHostId;
}

void so_backend_set_host_id_locked(u32 host_id)
{
    soHostId = host_id;
}

int so_backend_check_active_locked(void)
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

int so_backend_check_active(void)
{
    pthread_mutex_lock(&so_lock);
    int result = so_backend_check_active_locked();
    pthread_mutex_unlock(&so_lock);
    return result;
}

static bool slot_is_open_locked(int s)
{
    return s >= 0 && s < SO_BACKEND_MAX_SOCKET && slots[s].state == SLOT_OPEN;
}

static int slot_find_free_locked(void)
{
    for (int i = 0; i < SO_BACKEND_MAX_SOCKET; i++)
        if (slots[i].state == SLOT_FREE)
            return i;
    return SO_EMFILE;
}

static int so_open_native_socket(int pf, int type, int protocol)
{
    // protocol is safe since it's always called with protocol=0
    int sock = socket(to_native_family(pf), to_native_type(type), protocol);
    if (sock < 0)
        return so_error_from_errno(errno);
#ifndef MSG_NOSIGNAL
    int optval = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)) < 0)
    {
        int result = so_error_from_errno(errno);
        close(sock);
        return result;
    }
#endif
    if (type == SO_SOCK_DGRAM)
    {
        int enable = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0)
        {
            int result = so_error_from_errno(errno);
            close(sock);
            return result;
        }
    }
    return sock;
}

// socket() and setsockopt() never block, so creating the descriptor under the
// lock keeps SOSocket2 atomic with respect to SOCleanup.
int so_backend_open(int pf, int type, int protocol)
{
    pthread_mutex_lock(&so_lock);
    int result = so_backend_check_active_locked();
    if (result == SO_SUCCESS)
        result = slot_find_free_locked();
    if (result >= 0)
    {
        int sock = so_open_native_socket(pf, type, protocol);
        if (sock < 0)
            result = sock;
        else
            slots[result] = (struct slot){ .fd = sock, .refs = 0, .state = SLOT_OPEN };
    }
    pthread_mutex_unlock(&so_lock);
    return result;
}

// close() does not wake a thread blocked on the same descriptor, shutdown()
// does; the last so_backend_release then closes it.
int so_backend_close_locked(int s)
{
    if (!slot_is_open_locked(s))
        return SO_EBADF;
    if (slots[s].refs > 0)
    {
        slots[s].state = SLOT_CLOSING;
        shutdown(slots[s].fd, SHUT_RDWR);
        return SO_SUCCESS;
    }
    slots[s].state = SLOT_FREE;
    if (close(slots[s].fd) == -1)
        return so_error_from_errno(errno);
    return SO_SUCCESS;
}

void so_backend_close_all_locked(void)
{
    for (int i = 0; i < SO_BACKEND_MAX_SOCKET; i++)
        so_backend_close_locked(i);
}

int so_backend_acquire(int s, int* fd)
{
    pthread_mutex_lock(&so_lock);
    int result = so_backend_check_active_locked();
    if (result == SO_SUCCESS && !slot_is_open_locked(s))
        result = SO_EBADF;
    if (result == SO_SUCCESS)
    {
        slots[s].refs++;
        *fd = slots[s].fd;
    }
    pthread_mutex_unlock(&so_lock);
    return result;
}

static void slot_release_locked(int s)
{
    if (--slots[s].refs == 0 && slots[s].state == SLOT_CLOSING)
    {
        close(slots[s].fd);
        slots[s].state = SLOT_FREE;
    }
}

void so_backend_release(int s)
{
    pthread_mutex_lock(&so_lock);
    slot_release_locked(s);
    pthread_mutex_unlock(&so_lock);
}

// IOS fails the operations still pending on a socket it closes with
// SO_ENOTCONN; the host instead returns 0 from a recv woken by shutdown().
int so_backend_release_io(int s, int result)
{
    pthread_mutex_lock(&so_lock);
    bool interrupted = slots[s].state == SLOT_CLOSING;
    slot_release_locked(s);
    pthread_mutex_unlock(&so_lock);
    return interrupted && result <= 0 ? SO_ENOTCONN : result;
}

void so_backend_acquire_pollfds(struct pollfd* dst, const SOPollFD* src, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
    {
        if (so_backend_acquire(src[i].fd, &dst[i].fd) != SO_SUCCESS)
            dst[i].fd = -1;
        dst[i].events = so_pollevent_to_native(src[i].events);
        dst[i].revents = 0;
    }
}

void so_backend_release_pollfds(const struct pollfd* native, const SOPollFD* fds, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
        if (native[i].fd != -1)
            so_backend_release(fds[i].fd);
}
