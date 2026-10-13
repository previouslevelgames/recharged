#include "so_test_peer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int so_test_peer_listen(uint16_t* port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0
        || getsockname(fd, (sockaddr*)&addr, &len) != 0)
    {
        close(fd);
        return -1;
    }
    *port = addr.sin_port;
    return fd;
}

int so_test_peer_accept(int listener, uint16_t* client_port, uint32_t* client_addr)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int fd = accept(listener, (sockaddr*)&addr, &len);
    if (fd >= 0 && client_port != nullptr)
        *client_port = addr.sin_port;
    if (fd >= 0 && client_addr != nullptr)
        *client_addr = addr.sin_addr.s_addr;
    return fd;
}

long so_test_peer_recv(int fd, void* buf, size_t len)
{
    return recv(fd, buf, len, MSG_WAITALL);
}

long so_test_peer_send(int fd, const void* buf, size_t len)
{
    return send(fd, buf, len, 0);
}

void so_test_peer_close(int fd)
{
    close(fd);
}
