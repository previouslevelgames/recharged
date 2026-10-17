#pragma once

#include <cstddef>
#include <cstdint>

// The other end of the TCP tests, on plain POSIX sockets. It lives in its own
// TU because <sys/socket.h> and <revolution/so.h> both define SO_BROADCAST
// (6 on Linux, 9 in the SDK). Ports and addresses are in network order, like
// SOSockAddrIn.

// Listens on 127.0.0.1 with a port chosen by the kernel. Returns the fd or -1.
int so_test_peer_listen(uint16_t* port);
// Returns the accepted fd or -1; the client's port and address are optional.
int so_test_peer_accept(int listener, uint16_t* client_port, uint32_t* client_addr);
// Blocks until exactly len bytes arrived. Returns the bytes read or -1.
long so_test_peer_recv(int fd, void* buf, size_t len);
long so_test_peer_send(int fd, const void* buf, size_t len);
void so_test_peer_close(int fd);
// Descriptors open in the process, to catch the ones the backend leaks.
int so_test_peer_open_fds(void);
