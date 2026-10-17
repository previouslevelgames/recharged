#ifndef CHARGED_NET_SO_STATE_H
#define CHARGED_NET_SO_STATE_H

// Library state and socket table shared by the SO entry points. Functions
// ending in _locked expect so_backend_lock() to be held; the others take it.

#include <revolution/so.h>
#include <poll.h>

void so_backend_lock(void);
void so_backend_unlock(void);

u8 so_backend_state_locked(void);
void so_backend_set_state_locked(u8 state);
u32 so_backend_host_id_locked(void);
void so_backend_set_host_id_locked(u32 host_id);
int so_backend_check_active_locked(void);
int so_backend_check_active(void);

int so_backend_open(int pf, int type, int protocol);
int so_backend_close_locked(int s);
void so_backend_close_all_locked(void);

int so_backend_acquire(int s, int* fd);
void so_backend_release(int s);
int so_backend_release_io(int s, int result);
void so_backend_acquire_pollfds(struct pollfd* dst, const SOPollFD* src, unsigned int n);
void so_backend_release_pollfds(const struct pollfd* native, const SOPollFD* fds, unsigned int n);

#endif // CHARGED_NET_SO_STATE_H
