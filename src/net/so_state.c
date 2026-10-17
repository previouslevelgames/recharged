#include "so_state.h"

#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <unistd.h>

#include "so_convert.h"

#define SO_BACKEND_MAX_SOCKET 32

enum slot_state { SLOT_FREE, SLOT_OPEN, SLOT_CLOSING };
struct slot
{
    int fd, refs, type;
    // SOShutdown calls per direction, so an operation can tell whether one
    // arrived while it was waiting.
    unsigned int shutdowns[2];
    enum slot_state state;
};
static struct slot slots[SO_BACKEND_MAX_SOCKET];
static pthread_mutex_t so_lock = PTHREAD_MUTEX_INITIALIZER;

static u8 soState = SO_INTERNAL_STATE_TERMINATED;

static u32 soHostId = 0;

static SOAlloc soAlloc = NULL;
static SOFree soFree = NULL;

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

void so_backend_set_allocator_locked(SOAlloc alloc_fn, SOFree free_fn)
{
    soAlloc = alloc_fn;
    soFree = free_fn;
}

void* so_backend_alloc(u32 name, s32 size)
{
    pthread_mutex_lock(&so_lock);
    SOAlloc alloc_fn = soAlloc;
    pthread_mutex_unlock(&so_lock);
    return alloc_fn != NULL && size > 0 ? alloc_fn(name, size) : NULL;
}

void so_backend_free(u32 name, void* ptr, s32 size)
{
    pthread_mutex_lock(&so_lock);
    SOFree free_fn = soState != SO_INTERNAL_STATE_TERMINATED ? soFree : NULL;
    pthread_mutex_unlock(&so_lock);
    if (free_fn != NULL && ptr != NULL)
        free_fn(name, ptr, size);
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

static int so_configure_native_socket(int sock, int type)
{
    int result = sock;
    int enable = 1;
#ifndef MSG_NOSIGNAL
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) < 0)
    {
        result = so_error_from_errno(errno);
        close(sock);
        return result;
    }
#endif
    if (type == SO_SOCK_DGRAM)
    {
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0)
        {
            result = so_error_from_errno(errno);
            close(sock);
            return result;
        }
    }
    return result;
}

static int so_open_native_socket(int pf, int type, int protocol)
{
    // protocol is safe since it's always called with protocol=0
    int sock = socket(to_native_family(pf), to_native_type(type), protocol);
    if (sock < 0)
        return so_error_from_errno(errno);
    return so_configure_native_socket(sock, type);
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
            slots[result] = (struct slot){ 
                .fd = sock,
                .refs = 0,
                .type = to_native_type(type),
                .state = SLOT_OPEN
            };
    }
    pthread_mutex_unlock(&so_lock);
    return result;
}

int so_backend_insert_accepted(int fd)
{
    fd = so_configure_native_socket(fd, SO_SOCK_STREAM);
    if (fd < 0)
        return fd;
    pthread_mutex_lock(&so_lock);
    int result = so_backend_check_active_locked();
    if (result == SO_SUCCESS)
        result = slot_find_free_locked();
    if (result >= 0)
    {
        slots[result] = (struct slot){
            .fd = fd,
            .refs = 0,
            .type = SOCK_STREAM,
            .state = SLOT_OPEN
        };
    }
    else
    {
        close(fd);
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

static int slot_acquire_locked(int s)
{
    int result = so_backend_check_active_locked();
    if (result == SO_SUCCESS && !slot_is_open_locked(s))
        result = SO_EBADF;
    if (result == SO_SUCCESS)
        slots[s].refs++;
    return result;
}

int so_backend_acquire(int s, int* fd, int* type)
{
    pthread_mutex_lock(&so_lock);
    int result = slot_acquire_locked(s);
    if (result == SO_SUCCESS)
    {
        *fd = slots[s].fd;
        if (type != NULL)
            *type = slots[s].type;
    }
    pthread_mutex_unlock(&so_lock);
    return result;
}

int so_backend_acquire_io(int s, enum so_backend_direction direction, int shutdown_error,
    int* fd, struct so_backend_io* io)
{
    pthread_mutex_lock(&so_lock);
    int result = slot_acquire_locked(s);
    if (result == SO_SUCCESS)
    {
        *fd = slots[s].fd;
        *io = (struct so_backend_io){
            .s = s,
            .direction = direction,
            .shutdown_error = shutdown_error,
            .shutdowns = slots[s].shutdowns[direction],
        };
    }
    pthread_mutex_unlock(&so_lock);
    return result;
}

void so_backend_record_shutdown(int s, bool shut_read, bool shut_write)
{
    pthread_mutex_lock(&so_lock);
    if (shut_read)
        slots[s].shutdowns[SO_BACKEND_READ]++;
    if (shut_write)
        slots[s].shutdowns[SO_BACKEND_WRITE]++;
    pthread_mutex_unlock(&so_lock);
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
// SO_ENOTCONN, and those a shutdown ends with the operation's own error
// (Dolphin's CloseFd and Shutdown; hwtest in dolphin-emu PR 8758). The host
// instead returns 0 from a recv woken by shutdown(). Data that arrived before
// the interruption is still reported.
int so_backend_release_io(const struct so_backend_io* io, int result)
{
    pthread_mutex_lock(&so_lock);
    const bool closed = slots[io->s].state == SLOT_CLOSING;
    const bool shut_down = slots[io->s].shutdowns[io->direction] != io->shutdowns;
    slot_release_locked(io->s);
    pthread_mutex_unlock(&so_lock);
    if (result > 0)
        return result;
    if (closed)
        return SO_ENOTCONN;
    if (shut_down)
        return io->shutdown_error;
    return result;
}

void so_backend_acquire_pollfds(struct pollfd* dst, const SOPollFD* src, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
    {
        if (so_backend_acquire(src[i].fd, &dst[i].fd, NULL) != SO_SUCCESS)
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
