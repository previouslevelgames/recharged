// SO backend on host sockets, one call at a time: byte order, the library
// lifecycle, the socket table, bind and fcntl, UDP and TCP traffic, SOPoll,
// SOGetHostID and broadcast, each with the argument and descriptor errors the
// SDK's soBasic.c returns. The concurrent cases are in so_backend_threads.cpp.
#include <revolution/so.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "so_test_peer.h"

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void CheckResult(int actual, int expected, const char* message)
{
    ++checks;
    if (actual != expected)
        throw std::runtime_error(std::string(message) + " (got " + std::to_string(actual)
            + ", expected " + std::to_string(expected) + ")");
}
void CheckOneOf(int actual, std::initializer_list<int> allowed, const char* message)
{
    ++checks;
    std::string expected;
    for (int value : allowed)
    {
        if (actual == value)
            return;
        expected += (expected.empty() ? "" : " or ") + std::to_string(value);
    }
    throw std::runtime_error(std::string(message) + " (got " + std::to_string(actual)
        + ", expected " + expected + ")");
}

const SOLibraryConfig kValidConfig = { .alloc = (SOAlloc)0xf0cacc1a, .free = (SOFree)0xbadc0ffee0ddf00d };
const u8 kPayload[] = { 0x00, 0x01, 0x7f, 0x80, 0xff, 'M', 'S', 'C' };

SOSockAddrIn Loopback(u16 port)
{
    return SOSockAddrIn{
        .len = sizeof(SOSockAddrIn),
        .family = SO_PF_INET,
        .port = SOHtoNs(port),
        .addr = { .addr = SOHtoNl(0x7f000001) },
    };
}

struct UdpPair
{
    int rx, tx;
    SOSockAddrIn rx_addr;
};

void ByteOrder()
{
    u16 a = SOHtoNs(0x1234);
    CheckResult(((u8*)&a)[0], 0x12, "byteorder: SOHtoNs(0x1234) byte 0 in memory");
    CheckResult(((u8*)&a)[1], 0x34, "byteorder: SOHtoNs(0x1234) byte 1 in memory");

    u32 b = SOHtoNl(0x12345678);
    CheckResult(((u8*)&b)[0], 0x12, "byteorder: SOHtoNl(0x12345678) byte 0 in memory");
    CheckResult(((u8*)&b)[1], 0x34, "byteorder: SOHtoNl(0x12345678) byte 1 in memory");
    CheckResult(((u8*)&b)[2], 0x56, "byteorder: SOHtoNl(0x12345678) byte 2 in memory");
    CheckResult(((u8*)&b)[3], 0x78, "byteorder: SOHtoNl(0x12345678) byte 3 in memory");

    CheckResult(SONtoHs(a), 0x1234, "byteorder: SNtoH(SOHtoNs(0x1234))");
    Check(SONtoHl(b) == 0x12345678, "byteorder: SNtoHl(SOHtoNl(0x12345678))");
}

void Lifecycle()
{
    for (int i = 1; i <= 2; i++)
    {
        CheckResult(SOStartup(), SO_ENETRESET, "lifecycle: SOStartup before SOInit (TERMINATED)");
        CheckResult(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), SO_ENETRESET, "lifecycle: SOSocket2 before SOInit (TERMINATED)");
        CheckResult(SOFinish(), SO_EALREADY, "lifecycle: SOFinish before SOInit (TERMINATED)");
        Check(SOGetHostID() == 0, "lifecycle: SOGetHostID before SOInit (TERMINATED)");
        CheckResult(SOInit(NULL), SO_EINVAL, "lifecycle: SOInit(NULL)");

        SOLibraryConfig invalidCfg = { .alloc = NULL, .free = NULL };
        CheckResult(SOInit(&invalidCfg), SO_EINVAL, "lifecycle: SOInit with alloc and free both NULL");
        invalidCfg.alloc = (SOAlloc)0xdeadbeef;
        CheckResult(SOInit(&invalidCfg), SO_EINVAL, "lifecycle: SOInit with free NULL");

        SOLibraryConfig validCfg = kValidConfig;
        CheckResult(SOInit(&validCfg), SO_SUCCESS, "lifecycle: SOInit with a valid config (TERMINATED -> READY)");
        CheckResult(SOInit(&validCfg), SO_EALREADY, "lifecycle: second SOInit (already READY)");

        CheckResult(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), SO_EINVAL, "lifecycle: SOSocket2 after SOInit, before SOStartup (READY)");

        CheckResult(SOStartup(), SO_SUCCESS, "lifecycle: SOStartup after SOInit (READY -> ACTIVE)");
        CheckResult(SOStartup(), SO_EALREADY, "lifecycle: second SOStartup (already ACTIVE)");

        CheckResult(SOFinish(), SO_EINPROGRESS, "lifecycle: SOFinish while ACTIVE (SOCleanup must come first)");
        CheckResult(SOCleanup(), SO_SUCCESS, "lifecycle: SOCleanup while ACTIVE (ACTIVE -> READY)");
        CheckResult(SOCleanup(), SO_EALREADY, "lifecycle: second SOCleanup (already READY)");
        CheckResult(SOFinish(), SO_SUCCESS, "lifecycle: SOFinish after SOCleanup (READY -> TERMINATED)");
        CheckResult(SOFinish(), SO_EALREADY, "lifecycle: second SOFinish (already TERMINATED)");
    }
}

// Leaves every slot but the returned one open.
int SocketTable()
{
    SOLibraryConfig validCfg = kValidConfig;
    CheckResult(SOInit(&validCfg), SO_SUCCESS, "table: SOInit before the socket table tests");
    CheckResult(SOStartup(), SO_SUCCESS, "table: SOStartup before the socket table tests");
    const int s1 = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(s1 >= 0, "table: first SOSocket2 must return a descriptor >= 0");
    const int s2 = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(s2 >= 0, "table: second SOSocket2 must return a descriptor >= 0");
    Check(s1 != s2, "table: two open sockets must get different descriptors");
    CheckResult(SOClose(s1), SO_SUCCESS, "table: SOClose on the first socket");
    Check(SOFcntl(s2, SO_F_GETFL, 0) >= 0, "table: second socket must stay valid after closing the first (no compaction)");
    CheckResult(SOFcntl(s1, SO_F_GETFL, 0), SO_EBADF, "table: SOFcntl on the closed first socket must fail");
    CheckResult(SOClose(s1), SO_EBADF, "table: SOClose on an already closed descriptor");
    CheckResult(SOClose(-1), SO_EBADF, "table: SOClose(-1)");
    CheckResult(SOClose(99), SO_EBADF, "table: SOClose(99), out of range");
    CheckResult(SOSocket2(SO_PF_INET6, SO_SOCK_DGRAM, 0), SO_EAFNOSUPPORT, "table: SOSocket2 with SO_PF_INET6");
    CheckResult(SOClose(s2), SO_SUCCESS, "table: SOClose on the second socket");

    for (int i = 0; i < 32; i++)
        Check(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0) >= 0, "table: SOSocket2 while filling every slot");

    const int closed = 21;
    CheckResult(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), SO_EMFILE, "table: SOSocket2 with all 32 slots in use");
    CheckResult(SOClose(closed), SO_SUCCESS, "table: SOClose on slot 21 to free one slot");
    CheckResult(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), closed, "table: SOSocket2 must reuse the freed slot 21");
    CheckResult(SOClose(closed), SO_SUCCESS, "table: SOClose on slot 21, left free for the invalid-descriptor tests");
    return closed;
}

void BindAndFcntl(int closed)
{
    const int test_s = 0;
    SOSockAddrIn invalidLenAddr = Loopback(6767);
    invalidLenAddr.len = 100;
    CheckResult(SOBind(test_s, &invalidLenAddr), SO_EINVAL, "bind: SOBind with len=100 (must equal sizeof(SOSockAddr))");
    CheckResult(SOBind(test_s, NULL), SO_EINVAL, "bind: SOBind with a NULL address");
    SOSockAddrIn addr = Loopback(6767);
    CheckResult(SOBind(test_s, &addr), SO_SUCCESS, "bind: SOBind on 127.0.0.1:6767");
    CheckResult(SOBind(closed, &addr), SO_EBADF, "bind: SOBind on a closed descriptor");
    CheckResult(SOBind(1, &addr), SO_EADDRINUSE, "bind: second socket binding the same 127.0.0.1:6767");

    const int base_flags = SOFcntl(test_s, SO_F_GETFL, 0);
    CheckResult(base_flags, 0, "fcntl: SO_F_GETFL on a fresh socket (blocking)");
    SOFcntl(test_s, SO_F_SETFL, base_flags | SO_O_NONBLOCK);
    const int nonblock_flags = SOFcntl(test_s, SO_F_GETFL, 0);
    CheckResult(nonblock_flags, SO_O_NONBLOCK, "fcntl: SO_F_GETFL after setting SO_O_NONBLOCK");
    SOFcntl(test_s, SO_F_SETFL, nonblock_flags & (~SO_O_NONBLOCK));
    CheckResult(SOFcntl(test_s, SO_F_GETFL, 0), 0, "fcntl: SO_F_GETFL after clearing SO_O_NONBLOCK (GameSpy SetSockBlocking)");
    CheckResult(SOFcntl(test_s, 67, 0), SO_EINVAL, "fcntl: unknown command 67");
    CheckResult(SOFcntl(closed, SO_F_GETFL, 0), SO_EBADF, "fcntl: SO_F_GETFL on a closed descriptor");
}

void CleanupAndRestart(int closed)
{
    const int test_s = 0;
    CheckResult(SOCleanup(), SO_SUCCESS, "cleanup: SOCleanup with sockets still open (ACTIVE -> READY)");
    CheckResult(SOClose(closed), SO_EINVAL, "cleanup: SOClose on a free slot after SOCleanup (READY)");
    CheckResult(SOClose(test_s), SO_EINVAL, "cleanup: SOClose on a socket already closed by SOCleanup (READY)");
    CheckResult(SOStartup(), SO_SUCCESS, "cleanup: SOStartup after SOCleanup (READY -> ACTIVE)");
    const int restarted = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    CheckResult(restarted, 0, "cleanup: first SOSocket2 after restart must get slot 0");
    CheckResult(SOClose(restarted), SO_SUCCESS, "cleanup: SOClose on the restarted socket");
    CheckResult(SOCleanup(), SO_SUCCESS, "cleanup: SOCleanup while ACTIVE (ACTIVE -> READY)");
}

UdpPair Udp(int closed)
{
    CheckResult(SOStartup(), SO_SUCCESS, "udp: SOStartup before the UDP tests");
    UdpPair udp = { .rx_addr = Loopback(6768) };
    SOSockAddrIn tx_addr = Loopback(6769);

    udp.rx = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(udp.rx >= 0, "udp: SOSocket2 for the receiver");
    udp.tx = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(udp.tx >= 0, "udp: SOSocket2 for the sender");
    CheckResult(SOBind(udp.rx, &udp.rx_addr), SO_SUCCESS, "udp: SOBind receiver on 127.0.0.1:6768");
    CheckResult(SOBind(udp.tx, &tx_addr), SO_SUCCESS, "udp: SOBind sender on 127.0.0.1:6769");
    // Before any recv: a broken send must not hang the suite
    CheckResult(SOFcntl(udp.rx, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "udp: make the receiver non-blocking");

    u8 buf[64];
    SOSockAddrIn from = { .len = sizeof(SOSockAddrIn) };
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, &from), SO_EAGAIN, "udp: SORecvFrom on an empty non-blocking socket");

    CheckResult(SOSendTo(udp.tx, kPayload, sizeof(kPayload), 0, &udp.rx_addr), (int)sizeof(kPayload), "udp: SOSendTo returns the bytes sent");

    from = { .len = sizeof(SOSockAddrIn) };
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, &from), (int)sizeof(kPayload), "udp: SORecvFrom returns the datagram size");
    Check(std::memcmp(buf, kPayload, sizeof(kPayload)) == 0, "udp: received bytes differ from the payload");
    CheckResult(from.len, (int)sizeof(SOSockAddrIn), "udp: SORecvFrom sender len");
    CheckResult(from.family, SO_PF_INET, "udp: SORecvFrom sender family");
    CheckResult(from.port, SOHtoNs(6769), "udp: SORecvFrom sender port (network order)");
    Check(from.addr.addr == SOHtoNl(0x7f000001), "udp: SORecvFrom sender address (network order)");
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, &from), SO_EAGAIN, "udp: queue must be empty after one recv");

    // A zero-length datagram is a real packet: 0, not SO_EAGAIN
    CheckResult(SOSendTo(udp.tx, NULL, 0, 0, &udp.rx_addr), 0, "udp: SOSendTo of an empty datagram");
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, &from), 0, "udp: SORecvFrom of an empty datagram");

    // sockFrom == NULL is the SORecv path in the SDK: data, no sender address
    CheckResult(SOSendTo(udp.tx, kPayload, sizeof(kPayload), 0, &udp.rx_addr), (int)sizeof(kPayload), "udp: SOSendTo before the NULL-sockFrom recv");
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, NULL), (int)sizeof(kPayload), "udp: SORecvFrom with sockFrom NULL (SORecv path)");

    // Argument validation, same order as SendTo/RecvFrom in soBasic.c
    SOSockAddrIn bad_len = udp.rx_addr;
    bad_len.len = 100;
    CheckResult(SOSendTo(udp.tx, kPayload, sizeof(kPayload), 1, &udp.rx_addr), SO_EINVAL, "udp: SOSendTo with flags != 0");
    CheckResult(SOSendTo(udp.tx, kPayload, sizeof(kPayload), 0, &bad_len), SO_EINVAL, "udp: SOSendTo with len=100 address");
    CheckResult(SOSendTo(udp.tx, kPayload, -1, 0, &udp.rx_addr), SO_EINVAL, "udp: SOSendTo with negative len");
    CheckResult(SOSendTo(udp.tx, NULL, 4, 0, &udp.rx_addr), SO_EINVAL, "udp: SOSendTo with NULL buf and len > 0");
    CheckResult(SOSendTo(closed, kPayload, sizeof(kPayload), 0, &udp.rx_addr), SO_EBADF, "udp: SOSendTo on a closed descriptor");
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 1, &from), SO_EINVAL, "udp: SORecvFrom with flags != 0");
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, &bad_len), SO_EINVAL, "udp: SORecvFrom with len=100 address");
    CheckResult(SORecvFrom(udp.rx, NULL, 4, 0, &from), SO_EINVAL, "udp: SORecvFrom with NULL buf and len > 0");
    CheckResult(SORecvFrom(closed, buf, sizeof(buf), 0, &from), SO_EBADF, "udp: SORecvFrom on a closed descriptor");
    return udp;
}

void SockName(const UdpPair& udp, int closed)
{
    SOSockAddrIn name = { .len = sizeof(SOSockAddrIn) };
    CheckResult(SOGetSockName(udp.rx, &name), SO_SUCCESS, "getsockname: SOGetSockName on the bound receiver");
    CheckResult(name.len, (int)sizeof(SOSockAddrIn), "getsockname: len");
    CheckResult(name.family, SO_PF_INET, "getsockname: family");
    CheckResult(name.port, SOHtoNs(6768), "getsockname: port (network order)");
    Check(name.addr.addr == SOHtoNl(0x7f000001), "getsockname: address (network order)");

    CheckResult(SOGetSockName(udp.rx, NULL), SO_EINVAL, "getsockname: NULL address (unlike SORecvFrom, not optional)");
    name = { .len = 100 };
    CheckResult(SOGetSockName(udp.rx, &name), SO_EINVAL, "getsockname: len=100 address");
    name = { .len = sizeof(SOSockAddrIn) };
    CheckResult(SOGetSockName(closed, &name), SO_EBADF, "getsockname: closed descriptor");

    // Port 0 lets the kernel pick a free port; SOGetSockName is how you learn it
    u8 buf[64];
    const int eph = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(eph >= 0, "getsockname: SOSocket2 for the ephemeral-port socket");
    SOSockAddrIn eph_addr = Loopback(0);
    CheckResult(SOBind(eph, &eph_addr), SO_SUCCESS, "getsockname: SOBind on 127.0.0.1:0");
    eph_addr = { .len = sizeof(SOSockAddrIn) };
    CheckResult(SOGetSockName(eph, &eph_addr), SO_SUCCESS, "getsockname: SOGetSockName after binding to port 0");
    Check(eph_addr.port != 0, "getsockname: the kernel must have assigned a real port");
    CheckResult(SOFcntl(eph, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "getsockname: make the ephemeral socket non-blocking");
    CheckResult(SOSendTo(udp.tx, kPayload, sizeof(kPayload), 0, &eph_addr), (int)sizeof(kPayload), "getsockname: SOSendTo to the address SOGetSockName returned");
    CheckResult(SORecvFrom(eph, buf, sizeof(buf), 0, NULL), (int)sizeof(kPayload), "getsockname: the datagram must arrive on the ephemeral port");
    CheckResult(SOClose(eph), SO_SUCCESS, "getsockname: SOClose on the ephemeral-port socket");
}

// The receiver must be non-blocking with an empty queue.
void Poll(const UdpPair& udp, int closed)
{
    u8 buf[64];
    SOPollFD pfd[3];
    CheckResult(SOPoll(NULL, 1, 0), SO_EINVAL, "poll: NULL fds");
    CheckResult(SOPoll(pfd, 0, 0), 0, "poll: nfds == 0");
    // Positive timeouts need OSTicksToMilliseconds (bus clock): rejected for now
    pfd[0] = { .fd = udp.rx, .events = SO_POLLRDNORM, .revents = 0 };
    CheckResult(SOPoll(pfd, 1, 1), SO_EINVAL, "poll: positive timeout (unsupported for now)");

    // revents is pre-filled with garbage: SOPoll must always overwrite it
    pfd[0] = { .fd = udp.rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    CheckResult(SOPoll(pfd, 1, 0), 0, "poll: empty socket, timeout 0, nothing ready");
    CheckResult(pfd[0].revents, 0, "poll: empty socket revents");

    pfd[0] = { .fd = udp.tx, .events = SO_POLLWRNORM, .revents = 0xffff };
    CheckResult(SOPoll(pfd, 1, 0), 1, "poll: a UDP socket is always writable");
    CheckResult(pfd[0].revents, SO_POLLWRNORM, "poll: writable revents (WRNORM is 0x8, POLLOUT would be 0x4)");

    // SO_POLLIN is RDNORM|RDBAND: only RDNORM must come back for normal data
    CheckResult(SOSendTo(udp.tx, kPayload, sizeof(kPayload), 0, &udp.rx_addr), (int)sizeof(kPayload), "poll: SOSendTo to make the receiver readable");
    pfd[0] = { .fd = udp.rx, .events = SO_POLLIN, .revents = 0xffff };
    CheckResult(SOPoll(pfd, 1, 0), 1, "poll: socket with a pending datagram");
    CheckResult(pfd[0].revents, SO_POLLRDNORM, "poll: readable revents (RDNORM only, no RDBAND)");
    // Data is already there, so the infinite timeout must return immediately
    pfd[0] = { .fd = udp.rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    CheckResult(SOPoll(pfd, 1, SO_INFTIM), 1, "poll: SO_INFTIM with data ready returns at once");
    CheckResult(pfd[0].revents, SO_POLLRDNORM, "poll: SO_INFTIM revents");

    // Invalid descriptors don't fail the call: they get NVAL and count as ready
    pfd[0] = { .fd = udp.rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[1] = { .fd = closed, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[2] = { .fd = udp.tx, .events = SO_POLLWRNORM, .revents = 0xffff };
    CheckResult(SOPoll(pfd, 3, 0), 3, "poll: readable + closed + writable");
    CheckResult(pfd[0].revents, SO_POLLRDNORM, "poll: mixed array, readable socket revents");
    CheckResult(pfd[1].revents, SO_POLLNVAL, "poll: mixed array, closed descriptor revents");
    CheckResult(pfd[2].revents, SO_POLLWRNORM, "poll: mixed array, writable socket revents");

    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, NULL), (int)sizeof(kPayload), "poll: drain the receiver");

    // Empty + invalid: only the NVAL entry counts
    pfd[0] = { .fd = udp.rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[1] = { .fd = -1, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[2] = { .fd = 99, .events = SO_POLLRDNORM, .revents = 0xffff };
    CheckResult(SOPoll(pfd, 3, 0), 2, "poll: empty + fd -1 + fd 99 out of range");
    CheckResult(pfd[0].revents, 0, "poll: empty socket next to invalid ones");
    CheckResult(pfd[1].revents, SO_POLLNVAL, "poll: fd -1 revents");
    CheckResult(pfd[2].revents, SO_POLLNVAL, "poll: fd 99 revents");
}

// The detected address depends on the machine running the suite (it is
// loopback in a sandbox without network), so the default case only checks
// properties that hold everywhere. Returns the detected address.
u32 HostId()
{
    u8 buf[64];
    const u32 host = (u32)SOGetHostID();
    Check(host != 0, "hostid: SOGetHostID while ACTIVE must not be 0");
    Check((u32)SOGetHostID() == host, "hostid: SOGetHostID must be stable while ACTIVE");
    if (sizeof(long) > sizeof(u32))
        Check(((unsigned long)SOGetHostID() >> 32) == 0, "hostid: no sign extension in the upper 32 bits of long");

    // Whatever was detected must belong to this host: bind to it and loop a datagram
    SOSockAddrIn self = { .len = sizeof(SOSockAddrIn), .family = SO_PF_INET, .port = 0, .addr = { .addr = host } };
    const int self_s = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(self_s >= 0, "hostid: SOSocket2 for the self socket");
    CheckResult(SOBind(self_s, &self), SO_SUCCESS, "hostid: SOBind on the SOGetHostID address (must be local)");
    CheckResult(SOGetSockName(self_s, &self), SO_SUCCESS, "hostid: SOGetSockName on the self socket");
    CheckResult(SOFcntl(self_s, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "hostid: make the self socket non-blocking");
    CheckResult(SOSendTo(self_s, kPayload, sizeof(kPayload), 0, &self), (int)sizeof(kPayload), "hostid: SOSendTo to our own SOGetHostID address");
    CheckResult(SORecvFrom(self_s, buf, sizeof(buf), 0, NULL), (int)sizeof(kPayload), "hostid: the datagram sent to ourselves must arrive");
    CheckResult(SOClose(self_s), SO_SUCCESS, "hostid: SOClose on the self socket");

    // The address is captured by SOStartup, so the override needs a restart.
    // The caller may already have set the variable: keep it to restore later.
    const char* env_iface = std::getenv("RECHARGED_NET_IFACE");
    const std::string saved_iface = env_iface != nullptr ? env_iface : "";
    setenv("RECHARGED_NET_IFACE", "lo", 1);
    CheckResult(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before forcing the interface");
    Check(SOGetHostID() == 0, "hostid: SOGetHostID after SOCleanup (READY)");
    CheckResult(SOStartup(), SO_SUCCESS, "hostid: SOStartup with RECHARGED_NET_IFACE=lo");
    Check((u32)SOGetHostID() == SOHtoNl(0x7f000001), "hostid: RECHARGED_NET_IFACE=lo must give 127.0.0.1");

    setenv("RECHARGED_NET_IFACE", "msc-does-not-exist", 1);
    CheckResult(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before forcing a missing interface");
    CheckResult(SOStartup(), SO_SUCCESS, "hostid: SOStartup with a missing interface");
    Check((u32)SOGetHostID() == SOHtoNl(0x7f000001), "hostid: a missing interface must fall back to 127.0.0.1");

    if (env_iface != nullptr)
        setenv("RECHARGED_NET_IFACE", saved_iface.c_str(), 1);
    else
        unsetenv("RECHARGED_NET_IFACE");
    CheckResult(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before restoring the environment");
    CheckResult(SOStartup(), SO_SUCCESS, "hostid: SOStartup with the original environment");
    Check((u32)SOGetHostID() == host, "hostid: with the original environment the first detected address comes back");
    return host;
}

// The backend has no SOListen/SOAccept, so the server side is a plain POSIX
// socket (so_test_peer.cpp) on 127.0.0.1 with a port chosen by the kernel.
void Tcp(int closed)
{
    u8 buf[64];
    u16 listen_port = 0;
    const int listener = so_test_peer_listen(&listen_port);
    Check(listener >= 0, "tcp: POSIX listener on 127.0.0.1:0");
    SOSockAddrIn server = Loopback(0);
    server.port = listen_port;

    const int c = SOSocket2(SO_PF_INET, SO_SOCK_STREAM, 0);
    Check(c >= 0, "tcp: SOSocket2 for the stream client");

    // Argument validation, same as SOConnect in soBasic.c
    SOSockAddrIn bad_server = server;
    bad_server.len = 100;
    CheckResult(SOConnect(c, NULL), SO_EINVAL, "tcp: SOConnect with a NULL address");
    CheckResult(SOConnect(c, &bad_server), SO_EINVAL, "tcp: SOConnect with len=100 address");
    CheckResult(SOConnect(closed, &server), SO_EBADF, "tcp: SOConnect on a closed descriptor");

    // A blocking connect returns once the handshake is done; the connection
    // waits in the listen backlog, so accept can come afterwards
    CheckResult(SOConnect(c, &server), SO_SUCCESS, "tcp: blocking SOConnect to the listener");
    CheckResult(SOConnect(c, &server), SO_EISCONN, "tcp: second SOConnect on a connected socket");

    u16 peer_port = 0;
    u32 peer_host = 0;
    const int peer = so_test_peer_accept(listener, &peer_port, &peer_host);
    Check(peer >= 0, "tcp: POSIX accept of the SO client");

    // Both ends must agree on the client's address: catches byte order slips
    // in the sockaddr conversion
    SOSockAddrIn local = { .len = sizeof(SOSockAddrIn) };
    CheckResult(SOGetSockName(c, &local), SO_SUCCESS, "tcp: SOGetSockName on the connected client");
    CheckResult(local.port, peer_port, "tcp: client port seen by SOGetSockName and by accept");
    Check(local.addr.addr == peer_host, "tcp: client address seen by SOGetSockName and by accept");

    u8 peer_buf[64];
    CheckResult(SOSend(c, kPayload, sizeof(kPayload), 0), (int)sizeof(kPayload), "tcp: SOSend returns the bytes sent");
    long peer_got = so_test_peer_recv(peer, peer_buf, sizeof(kPayload));
    Check(peer_got == (long)sizeof(kPayload) && std::memcmp(peer_buf, kPayload, sizeof(kPayload)) == 0,
          "tcp: bytes received by the peer differ from the SOSend payload");

    // In soBasic.c SOSend is SendTo(..., NULL): SOSendTo with a NULL address is the same call
    CheckResult(SOSendTo(c, kPayload, sizeof(kPayload), 0, NULL), (int)sizeof(kPayload), "tcp: SOSendTo with a NULL address (SOSend path)");
    peer_got = so_test_peer_recv(peer, peer_buf, sizeof(kPayload));
    Check(peer_got == (long)sizeof(kPayload) && std::memcmp(peer_buf, kPayload, sizeof(kPayload)) == 0,
          "tcp: bytes received by the peer differ from the SOSendTo(NULL) payload");

    CheckResult(SOSend(c, NULL, 0, 0), 0, "tcp: SOSend of zero bytes");
    CheckResult(SOSend(c, kPayload, sizeof(kPayload), 1), SO_EINVAL, "tcp: SOSend with flags != 0");
    CheckResult(SOSend(c, kPayload, -1, 0), SO_EINVAL, "tcp: SOSend with negative len");
    CheckResult(SOSend(c, NULL, 4, 0), SO_EINVAL, "tcp: SOSend with NULL buf and len > 0");
    CheckResult(SOSend(closed, kPayload, sizeof(kPayload), 0), SO_EBADF, "tcp: SOSend on a closed descriptor");

    // The other direction, as NetworkStats reads the HTTP reply
    const char reply[] = "HTTP/1.0 200 OK\r\n\r\n";
    Check(so_test_peer_send(peer, reply, sizeof(reply) - 1) == (long)(sizeof(reply) - 1), "tcp: POSIX send of the reply");
    CheckResult(SORecvFrom(c, buf, sizeof(buf), 0, NULL), (int)(sizeof(reply) - 1), "tcp: SORecvFrom of the reply on the stream");
    Check(std::memcmp(buf, reply, sizeof(reply) - 1) == 0, "tcp: received reply differs from what the peer sent");

    // NetworkStats: non-blocking socket, SOConnect must answer -26 and the
    // connection completes later. SOPoll with WRNORM tells when.
    const int nb = SOSocket2(SO_PF_INET, SO_SOCK_STREAM, 0);
    Check(nb >= 0, "tcp: SOSocket2 for the non-blocking client");
    CheckResult(SOFcntl(nb, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "tcp: make the client non-blocking");
    CheckResult(SOConnect(nb, &server), SO_EINPROGRESS, "tcp: non-blocking SOConnect (NetworkStats requires -26)");
    SOPollFD pfd = { .fd = nb, .events = SO_POLLWRNORM, .revents = 0 };
    CheckResult(SOPoll(&pfd, 1, SO_INFTIM), 1, "tcp: SOPoll until the non-blocking connect completes");
    CheckResult(pfd.revents, SO_POLLWRNORM, "tcp: a completed connect is writable, without ERR/HUP");
    // Linux answers the first connect after completion with 0 (it reports the
    // finished connect), BSD/macOS with EISCONN. From then on it's EISCONN.
    const int again = SOConnect(nb, &server);
    CheckOneOf(again, { SO_SUCCESS, SO_EISCONN },
               "tcp: SOConnect after the non-blocking connect completed (SO_SUCCESS on Linux, SO_EISCONN on BSD/macOS)");
    CheckResult(SOConnect(nb, &server), SO_EISCONN, "tcp: SOConnect on the established non-blocking socket");
    const int nb_peer = so_test_peer_accept(listener, NULL, NULL);
    Check(nb_peer >= 0, "tcp: POSIX accept of the non-blocking client");

    // Closing a listener frees its port: nobody is listening there now
    u16 dead_port = 0;
    const int dead = so_test_peer_listen(&dead_port);
    Check(dead >= 0, "tcp: second listener, closed right away");
    so_test_peer_close(dead);
    SOSockAddrIn dead_addr = Loopback(0);
    dead_addr.port = dead_port;
    const int refused = SOSocket2(SO_PF_INET, SO_SOCK_STREAM, 0);
    Check(refused >= 0, "tcp: SOSocket2 for the refused client");
    CheckResult(SOConnect(refused, &dead_addr), SO_ECONNREFUSED, "tcp: SOConnect to a port with no listener");

    // Writing to a dead TCP connection raises SIGPIPE, whose default action
    // kills the process. The backend must suppress it (MSG_NOSIGNAL): if the
    // suite dies here with SIGPIPE, that is the bug.
    const int unconnected = SOSocket2(SO_PF_INET, SO_SOCK_STREAM, 0);
    Check(unconnected >= 0, "tcp: SOSocket2 for the never connected client");
    int sent = SOSend(unconnected, kPayload, sizeof(kPayload), 0);
    CheckOneOf(sent, { SO_EPIPE, SO_ENOTCONN },
               "tcp: SOSend on a never connected stream (SO_EPIPE on Linux)");

    // After the peer closes, the first send usually still succeeds; the
    // peer's RST arrives a moment later and the next one fails
    so_test_peer_close(peer);
    sent = 0;
    for (int i = 0; i < 100 && sent >= 0; ++i)
    {
        sent = SOSend(c, kPayload, sizeof(kPayload), 0);
        if (sent >= 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CheckOneOf(sent, { SO_EPIPE, SO_ECONNRESET }, "tcp: SOSend after the peer closed");

    CheckResult(SOClose(unconnected), SO_SUCCESS, "tcp: SOClose on the never connected client");
    CheckResult(SOClose(refused), SO_SUCCESS, "tcp: SOClose on the refused client");
    CheckResult(SOClose(nb), SO_SUCCESS, "tcp: SOClose on the non-blocking client");
    CheckResult(SOClose(c), SO_SUCCESS, "tcp: SOClose on the blocking client");
    so_test_peer_close(nb_peer);
    so_test_peer_close(listener);
}

// The Wii stack lets any UDP socket send to 255.255.255.255; Linux answers
// EACCES unless SO_BROADCAST is set, which SOSocket2 does. The effect is
// checked rather than the option: so.h and <sys/socket.h> both define
// SO_BROADCAST (9 in the SDK, 6 on Linux), and with the SDK value setsockopt
// would still succeed, turning on SO_KEEPALIVE instead. Only the send is
// checked: the local copy of a broadcast re-enters through the network
// interface, so a host firewall may drop it. Returns false when skipped.
bool Broadcast()
{
    const int bc = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(bc >= 0, "broadcast: SOSocket2 for the sender");
    SOSockAddrIn bc_everyone = Loopback(6770);
    bc_everyone.addr.addr = 0xffffffff;
    const int bc_sent = SOSendTo(bc, kPayload, sizeof(kPayload), 0, &bc_everyone);
    // Without a default route (sandbox, no network) there is nowhere to send it
    const bool routed = bc_sent != SO_ENETUNREACH;
    if (routed)
        CheckResult(bc_sent, (int)sizeof(kPayload),
                    "broadcast: SOSendTo 255.255.255.255 (SO_EACCES: SO_BROADCAST missing or set with the SDK value)");
    CheckResult(SOClose(bc), SO_SUCCESS, "broadcast: SOClose on the sender");
    return routed;
}

void AfterCleanup(const UdpPair& udp)
{
    SOPollFD pfd = { .fd = udp.rx, .events = SO_POLLRDNORM, .revents = 0 };
    SOSockAddrIn name = { .len = sizeof(SOSockAddrIn) };
    SOSockAddrIn server = Loopback(6771);
    CheckResult(SOCleanup(), SO_SUCCESS, "post tests: SOCleanup while ACTIVE (ACTIVE -> READY)");
    Check(SOGetHostID() == 0, "post tests: SOGetHostID after SOCleanup (READY)");
    CheckResult(SOPoll(&pfd, 1, 0), SO_EINVAL, "post tests: SOPoll after SOCleanup (READY)");
    CheckResult(SOGetSockName(udp.rx, &name), SO_EINVAL, "post tests: SOGetSockName after SOCleanup (READY)");
    CheckResult(SOConnect(0, &server), SO_EINVAL, "post tests: SOConnect after SOCleanup (READY)");
    CheckResult(SOSend(0, kPayload, sizeof(kPayload), 0), SO_EINVAL, "post tests: SOSend after SOCleanup (READY)");
    CheckResult(SOFinish(), SO_SUCCESS, "post tests: SOFinish after SOCleanup (READY -> TERMINATED)");
    Check(SOGetHostID() == 0, "post tests: SOGetHostID after SOFinish (TERMINATED)");
}
} // namespace

int main()
{
    try
    {
        ByteOrder();
        Lifecycle();
        const int closed = SocketTable();
        BindAndFcntl(closed);
        CleanupAndRestart(closed);
        const UdpPair udp = Udp(closed);
        SockName(udp, closed);
        Poll(udp, closed);
        const u32 host = HostId();
        Tcp(closed);
        const bool broadcast = Broadcast();
        AfterCleanup(udp);
        std::cout << "SO backend: " << checks << " checks, host address " << (host & 0xff) << '.'
                  << ((host >> 8) & 0xff) << '.' << ((host >> 16) & 0xff) << '.' << (host >> 24)
                  << (broadcast ? "" : ", broadcast skipped (no route for 255.255.255.255)") << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SO backend failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
