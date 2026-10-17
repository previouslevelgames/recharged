#include "so_state.h"

#include <pthread.h>
#include <stdbool.h>

#include "so_convert.h"

#define SO_BACKEND_MAX_SOCKET 32

enum slot_state { SLOT_FREE, SLOT_OPEN, SLOT_CLOSING };
struct slot
{
    so_sys_socket fd;
    int refs, type;
    bool nonblocking;
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

// Closes the socket when it fails.
static int so_configure_native_socket(so_sys_socket sock, int type)
{
    int result = SO_SUCCESS;
    int enable = 1;
    // Windows raises no SIGPIPE; Linux passes MSG_NOSIGNAL to each send.
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) < 0)
        result = so_last_error();
#endif
    // Winsock takes the option value as const char*.
    if (result == SO_SUCCESS && type == SO_SOCK_DGRAM
        && setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&enable, sizeof(enable)) < 0)
        result = so_last_error();
    if (result != SO_SUCCESS)
        so_sys_close(sock);
    return result;
}

static int so_open_native_socket(int pf, int type, int protocol, so_sys_socket* sock)
{
    // protocol is safe since it's always called with protocol=0
    *sock = socket(to_native_family(pf), to_native_type(type), protocol);
    if (*sock == SO_SYS_INVALID_SOCKET)
        return so_last_error();
    return so_configure_native_socket(*sock, type);
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
        so_sys_socket sock;
        const int opened = so_open_native_socket(pf, type, protocol, &sock);
        if (opened != SO_SUCCESS)
            result = opened;
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

// The accepted socket starts blocking, as on Linux; Windows and the BSDs would
// give it the listening socket's mode.
int so_backend_insert_accepted(so_sys_socket fd)
{
    int result = so_configure_native_socket(fd, SO_SOCK_STREAM);
    if (result != SO_SUCCESS)
        return result;
    if (so_sys_set_nonblocking(fd, false) == -1)
    {
        result = so_last_error();
        so_sys_close(fd);
        return result;
    }
    pthread_mutex_lock(&so_lock);
    result = so_backend_check_active_locked();
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
        so_sys_close(fd);
    }
    pthread_mutex_unlock(&so_lock);
    return result;
}

// close() does not wake a thread blocked on the same descriptor,
// so_sys_wake does; the last so_backend_release then closes it.
int so_backend_close_locked(int s)
{
    if (!slot_is_open_locked(s))
        return SO_EBADF;
    if (slots[s].refs > 0)
    {
        slots[s].state = SLOT_CLOSING;
        so_sys_wake(slots[s].fd);
        return SO_SUCCESS;
    }
    slots[s].state = SLOT_FREE;
    if (so_sys_close(slots[s].fd) == -1)
        return so_last_error();
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

int so_backend_acquire(int s, so_sys_socket* fd, int* type)
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
    so_sys_socket* fd, struct so_backend_io* io)
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

bool so_backend_nonblocking(int s)
{
    pthread_mutex_lock(&so_lock);
    const bool nonblocking = slots[s].nonblocking;
    pthread_mutex_unlock(&so_lock);
    return nonblocking;
}

int so_backend_set_nonblocking(int s, bool nonblocking)
{
    pthread_mutex_lock(&so_lock);
    int result = SO_SUCCESS;
    if (so_sys_set_nonblocking(slots[s].fd, nonblocking) == -1)
        result = so_last_error();
    else
        slots[s].nonblocking = nonblocking;
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
        so_sys_close(slots[s].fd);
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

bool so_backend_closing(int s)
{
    pthread_mutex_lock(&so_lock);
    const bool closing = slots[s].state == SLOT_CLOSING;
    pthread_mutex_unlock(&so_lock);
    return closing;
}

void so_backend_acquire_pollfds(struct pollfd* dst, const SOPollFD* src, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
    {
        if (so_backend_acquire(src[i].fd, &dst[i].fd, NULL) != SO_SUCCESS)
            dst[i].fd = SO_SYS_INVALID_SOCKET;
        dst[i].events = so_pollevent_to_native(src[i].events);
        dst[i].revents = 0;
    }
}

void so_backend_release_pollfds(const struct pollfd* native, const SOPollFD* fds, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
        if (native[i].fd != SO_SYS_INVALID_SOCKET)
            so_backend_release(fds[i].fd);
}
