#ifndef CHARGED_NET_SO_STATE_H
#define CHARGED_NET_SO_STATE_H

// Library state and socket table shared by the SO entry points. Functions
// ending in _locked expect so_backend_lock() to be held; the others take it.

#include "so_sys.h"

void so_backend_lock(void);
void so_backend_unlock(void);

u8 so_backend_state_locked(void);
void so_backend_set_state_locked(u8 state);
u32 so_backend_host_id_locked(void);
void so_backend_set_host_id_locked(u32 host_id);
int so_backend_check_active_locked(void);
int so_backend_check_active(void);

void so_backend_set_allocator_locked(SOAlloc alloc_fn, SOFree free_fn);
void* so_backend_alloc(u32 name, s32 size);
void so_backend_free(u32 name, void* ptr, s32 size);

int so_backend_open(int pf, int type, int protocol);
int so_backend_insert_accepted(so_sys_socket fd);
int so_backend_close_locked(int s);
void so_backend_close_all_locked(void);

int so_backend_acquire(int s, so_sys_socket* fd, int* type);
void so_backend_release(int s);

// SO_F_GETFL and SO_F_SETFL on an acquired socket. The table keeps the
// blocking mode, which Windows cannot read back from the host socket.
bool so_backend_nonblocking(int s);
int so_backend_set_nonblocking(int s, bool nonblocking);

// For the operations that can block: the acquire records how many shutdowns
// the operation's direction had, so the release can tell whether SOClose or
// SOShutdown ended it while it waited and return the error IOS returns.
enum so_backend_direction { SO_BACKEND_READ, SO_BACKEND_WRITE };
struct so_backend_io
{
    int s;
    enum so_backend_direction direction;
    int shutdown_error;
    unsigned shutdowns;
};
int so_backend_acquire_io(int s, enum so_backend_direction direction, int shutdown_error,
    so_sys_socket* fd, struct so_backend_io* io);
int so_backend_release_io(const struct so_backend_io* io, int result);
// Call it before the native shutdown() that wakes the pending operations.
void so_backend_record_shutdown(int s, bool shut_read, bool shut_write);
// Whether SOClose closed the acquired socket while it was in use.
bool so_backend_closing(int s);
void so_backend_acquire_pollfds(struct pollfd* dst, const SOPollFD* src, unsigned int n);
void so_backend_release_pollfds(const struct pollfd* native, const SOPollFD* fds, unsigned int n);

#endif // CHARGED_NET_SO_STATE_H
