// SO backend on host sockets, one call at a time: byte order, the library
// lifecycle, the socket table, bind and fcntl, UDP and TCP traffic, socket
// options, SOPoll, SOShutdown, SOGetHostID, broadcast, the address
// conversions, the name lookups with the online.server redirect and
// SOGetInterfaceOpt, each with the argument
// and descriptor errors the SDK's soBasic.c, SOOption.c and SOInformation.c
// return.
// The concurrent cases are in so_backend_threads.cpp.
#include <revolution/so.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "net/so_resolve.h"
#include "so_test_peer.h"

namespace
{
// The loopback interface as RECHARGED_NET_IFACE names it on each host.
#ifdef _WIN32
constexpr const char* kLoopbackIface = "Loopback Pseudo-Interface 1";
void SetEnv(const char* name, const char* value) { _putenv_s(name, value); }
void UnsetEnv(const char* name) { _putenv_s(name, ""); }
#else
constexpr const char* kLoopbackIface = "lo";
void SetEnv(const char* name, const char* value) { setenv(name, value, 1); }
void UnsetEnv(const char* name) { unsetenv(name); }
#endif

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

// The options NHTTP and GameSpy set: SO_SO_RCVBUF and SO_SO_SNDBUF. Linux
// doubles a buffer size it is given and reports the doubled value, so the
// value read back is only checked to be at least the one set.
void SockOpt(const UdpPair& udp, int closed)
{
    for (int optname : { SO_SO_RCVBUF, SO_SO_SNDBUF })
    {
        const int size = 0x8000;
        CheckResult(SOSetSockOpt(udp.rx, SO_SOL_SOCKET, optname, &size, sizeof(size)), SO_SUCCESS,
                    "sockopt: SOSetSockOpt of a buffer size");
        int value = 0;
        int len = sizeof(value);
        CheckResult(SOGetSockOpt(udp.rx, SO_SOL_SOCKET, optname, &value, &len), SO_SUCCESS,
                    "sockopt: SOGetSockOpt of a buffer size");
        CheckResult(len, (int)sizeof(int), "sockopt: SOGetSockOpt length of a buffer size");
        Check(value >= size, "sockopt: the buffer size read back is smaller than the one set");
    }

    // Argument handling copied from SOOption.c: the SDK zero-fills a NULL
    // value, rejects lengths above its 20-byte buffer, skips the copy without
    // optlen, and reports the needed length with SO_EINVAL when it is short.
    const int size = 0x8000;
    CheckResult(SOSetSockOpt(udp.rx, SO_SOL_SOCKET, SO_SO_RCVBUF, NULL, sizeof(int)), SO_SUCCESS,
                "sockopt: SOSetSockOpt with a NULL value (zero-filled)");
    CheckResult(SOSetSockOpt(udp.rx, SO_SOL_SOCKET, SO_SO_RCVBUF, &size, -1), SO_EINVAL,
                "sockopt: SOSetSockOpt with a negative length");
    CheckResult(SOSetSockOpt(udp.rx, SO_SOL_SOCKET, SO_SO_RCVBUF, &size, 21), SO_EINVAL,
                "sockopt: SOSetSockOpt longer than the SDK's 20-byte buffer");
    int value = 0;
    CheckResult(SOGetSockOpt(udp.rx, SO_SOL_SOCKET, SO_SO_RCVBUF, &value, NULL), SO_SUCCESS,
                "sockopt: SOGetSockOpt without optlen");
    int short_len = 2;
    CheckResult(SOGetSockOpt(udp.rx, SO_SOL_SOCKET, SO_SO_RCVBUF, &value, &short_len), SO_EINVAL,
                "sockopt: SOGetSockOpt into a 2-byte buffer");
    CheckResult(short_len, (int)sizeof(int), "sockopt: a short SOGetSockOpt must report the needed length");

    // so.h's SO_BROADCAST is 9, which is SO_KEEPALIVE on Linux: an option the
    // backend does not know must fail rather than reach the host.
    const int sdk_broadcast = 9;
    const int enable = 1;
    int len = sizeof(value);
    CheckResult(SOSetSockOpt(udp.rx, SO_SOL_SOCKET, sdk_broadcast, &enable, sizeof(enable)), SO_ENOPROTOOPT,
                "sockopt: SOSetSockOpt of an option the backend does not translate");
    CheckResult(SOGetSockOpt(udp.rx, SO_SOL_SOCKET, sdk_broadcast, &value, &len), SO_ENOPROTOOPT,
                "sockopt: SOGetSockOpt of an option the backend does not translate");
    CheckResult(SOSetSockOpt(udp.rx, SO_SOL_TCP, 0x2001, &enable, sizeof(enable)), SO_ENOPROTOOPT,
                "sockopt: SOSetSockOpt at a level the backend does not translate");
    CheckResult(SOSetSockOpt(closed, SO_SOL_SOCKET, SO_SO_RCVBUF, &size, sizeof(size)), SO_EBADF,
                "sockopt: SOSetSockOpt on a closed descriptor");
    CheckResult(SOGetSockOpt(closed, SO_SOL_SOCKET, SO_SO_RCVBUF, &value, &len), SO_EBADF,
                "sockopt: SOGetSockOpt on a closed descriptor");
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

// The Wii ignores SOShutdown on UDP (Dolphin PR 8758's hwtest): the receiver
// must keep receiving. so.h names no modes, so only 0, 1 and 2 are valid.
void Shutdown(const UdpPair& udp, int closed)
{
    u8 buf[64];
    CheckResult(SOShutdown(udp.rx, 2), SO_SUCCESS, "shutdown: SOShutdown on a UDP socket");
    CheckResult(SOSendTo(udp.tx, kPayload, sizeof(kPayload), 0, &udp.rx_addr), (int)sizeof(kPayload),
                "shutdown: SOSendTo to the shut down UDP receiver");
    CheckResult(SORecvFrom(udp.rx, buf, sizeof(buf), 0, NULL), (int)sizeof(kPayload),
                "shutdown: a UDP socket must keep receiving after SOShutdown");
    CheckResult(SOShutdown(udp.rx, 3), SO_EINVAL, "shutdown: SOShutdown with how=3");
    CheckResult(SOShutdown(udp.rx, -1), SO_EINVAL, "shutdown: SOShutdown with how=-1");
    CheckResult(SOShutdown(closed, 2), SO_EBADF, "shutdown: SOShutdown on a closed descriptor");
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
        Check(((unsigned long long)(unsigned long)SOGetHostID() >> 32) == 0, "hostid: no sign extension in the upper 32 bits of long");

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
    SetEnv("RECHARGED_NET_IFACE", kLoopbackIface);
    CheckResult(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before forcing the interface");
    Check(SOGetHostID() == 0, "hostid: SOGetHostID after SOCleanup (READY)");
    CheckResult(SOStartup(), SO_SUCCESS, "hostid: SOStartup with RECHARGED_NET_IFACE=lo");
    Check((u32)SOGetHostID() == SOHtoNl(0x7f000001), "hostid: RECHARGED_NET_IFACE=lo must give 127.0.0.1");

    SetEnv("RECHARGED_NET_IFACE", "msc-does-not-exist");
    CheckResult(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before forcing a missing interface");
    CheckResult(SOStartup(), SO_SUCCESS, "hostid: SOStartup with a missing interface");
    Check((u32)SOGetHostID() == SOHtoNl(0x7f000001), "hostid: a missing interface must fall back to 127.0.0.1");

    if (env_iface != nullptr)
        SetEnv("RECHARGED_NET_IFACE", saved_iface.c_str());
    else
        UnsetEnv("RECHARGED_NET_IFACE");
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
    int value = 0;
    int len = sizeof(value);
    CheckResult(SOSetSockOpt(0, SO_SOL_SOCKET, SO_SO_RCVBUF, &value, sizeof(value)), SO_EINVAL,
                "post tests: SOSetSockOpt after SOCleanup (READY)");
    CheckResult(SOGetSockOpt(0, SO_SOL_SOCKET, SO_SO_RCVBUF, &value, &len), SO_EINVAL,
                "post tests: SOGetSockOpt after SOCleanup (READY)");
    CheckResult(SOShutdown(0, 2), SO_EINVAL, "post tests: SOShutdown after SOCleanup (READY)");
    CheckResult(SOFinish(), SO_SUCCESS, "post tests: SOFinish after SOCleanup (READY -> TERMINATED)");
    Check(SOGetHostID() == 0, "post tests: SOGetHostID after SOFinish (TERMINATED)");
}

// The allocator SOInit receives. SOGetAddrInfo takes its block from it, so the
// other tests' placeholder pointers would crash here.
struct Allocations
{
    unsigned allocs, frees;
    int live;
    bool fail_next;
    u32 last_name;
    s32 last_size;
    void* last_ptr;
};
Allocations allocations;

void* CountingAlloc(u32 name, s32 size)
{
    allocations.allocs++;
    allocations.last_name = name;
    allocations.last_size = size;
    if (allocations.fail_next)
    {
        allocations.fail_next = false;
        return nullptr;
    }
    allocations.last_ptr = std::malloc((size_t)size);
    allocations.live++;
    return allocations.last_ptr;
}

void CountingFree(u32 name, void* ptr, s32 size)
{
    allocations.frees++;
    allocations.last_name = name;
    allocations.last_size = size;
    allocations.last_ptr = ptr;
    allocations.live--;
    std::free(ptr);
}

const SOLibraryConfig kCountingConfig = { .alloc = CountingAlloc, .free = CountingFree };

const u32 kLoopbackAddr = 0x7f000001;

bool IsLoopback(const u8* addr)
{
    return addr[0] == 127;
}

u32 AddrOf(const u8* bytes)
{
    u32 addr;
    std::memcpy(&addr, bytes, sizeof(addr));
    return addr;
}

// Called in TERMINATED: SOInetAtoN and SOGetInterfaceOpt also work in READY,
// the lookups only in ACTIVE (SOiPrepareTempRm against SOiConclude).
void ResolverStates()
{
    SOInAddr in = { .addr = 0 };
    SOAddrInfo* res = nullptr;
    int count = 0;
    int len = sizeof(count);
    CheckResult(SOInetAtoN("1.2.3.4", &in), SO_ENETRESET, "resolver: SOInetAtoN before SOInit (TERMINATED)");
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_NUMBER, &count, &len), SO_ENETRESET,
                "resolver: SOGetInterfaceOpt before SOInit (TERMINATED)");
    Check(SOGetHostByName("127.0.0.1") == nullptr, "resolver: SOGetHostByName before SOInit (TERMINATED)");
    CheckResult(SOGetAddrInfo("127.0.0.1", nullptr, nullptr, &res), SO_ENETRESET,
                "resolver: SOGetAddrInfo before SOInit (TERMINATED)");

    SOLibraryConfig config = kCountingConfig;
    CheckResult(SOInit(&config), SO_SUCCESS, "resolver: SOInit with the counting allocator (TERMINATED -> READY)");
    CheckResult(SOInetAtoN("1.2.3.4", &in), 1, "resolver: SOInetAtoN after SOInit (READY)");
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_NUMBER, &count, &len), SO_SUCCESS,
                "resolver: SOGetInterfaceOpt after SOInit (READY)");
    CheckResult(count, 1, "resolver: SO_CONFIG_IP_ADDR_NUMBER in READY");
    Check(SOGetHostByName("127.0.0.1") == nullptr, "resolver: SOGetHostByName after SOInit (READY)");
    CheckResult(SOGetAddrInfo("127.0.0.1", nullptr, nullptr, &res), SO_EINVAL,
                "resolver: SOGetAddrInfo after SOInit (READY)");
    CheckResult(SOStartup(), SO_SUCCESS, "resolver: SOStartup (READY -> ACTIVE)");
}

void InetConversions()
{
    SOInAddr in = { .addr = 0 };
    CheckResult(SOInetAtoN("1.2.3.4", &in), 1, "inet: SOInetAtoN(\"1.2.3.4\")");
    const u8* bytes = (const u8*)&in.addr;
    Check(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4,
          "inet: SOInetAtoN must store the address in network order");
    CheckResult(SOInetAtoN("255.255.255.255", &in), 1, "inet: SOInetAtoN(\"255.255.255.255\")");
    Check(in.addr == 0xffffffff, "inet: SOInetAtoN(\"255.255.255.255\") value");

    // gsSocketRevolution.c's inet_addr reads 0 as "not an address" and
    // leaves the output alone.
    in.addr = 0x12345678;
    CheckResult(SOInetAtoN("gpcm.gs.nintendowifi.net", &in), 0, "inet: SOInetAtoN on a host name");
    CheckResult(SOInetAtoN("1.2.3.4.5", &in), 0, "inet: SOInetAtoN on five components");
    CheckResult(SOInetAtoN("", &in), 0, "inet: SOInetAtoN on an empty string");
    Check(in.addr == 0x12345678, "inet: a failed SOInetAtoN must not touch the output");
    CheckResult(SOInetAtoN("1.2.3.4", nullptr), 1, "inet: SOInetAtoN with no output");
    CheckResult(SOInetAtoN(nullptr, &in), SO_EINVAL, "inet: SOInetAtoN(NULL)");

    SOInAddr local = { .addr = SOHtoNl(0xc0a80001) };
    Check(std::strcmp(SOInetNtoA(local), "192.168.0.1") == 0, "inet: SOInetNtoA(192.168.0.1)");
    SOInAddr all = { .addr = 0xffffffff };
    Check(std::strcmp(SOInetNtoA(all), "255.255.255.255") == 0, "inet: SOInetNtoA(255.255.255.255)");
    SOInAddr zero = { .addr = 0 };
    Check(std::strcmp(SOInetNtoA(zero), "0.0.0.0") == 0, "inet: SOInetNtoA(0.0.0.0)");

    SOInAddr back = { .addr = 0 };
    CheckResult(SOInetAtoN(SOInetNtoA(local), &back), 1, "inet: SOInetAtoN(SOInetNtoA(x))");
    Check(back.addr == local.addr, "inet: SOInetAtoN(SOInetNtoA(x)) must give x back");

    // Each thread has its own buffer: a call on another thread must not
    // overwrite the string this one still holds.
    const char* mine = SOInetNtoA(local);
    std::string theirs;
    std::thread([&] { theirs = SOInetNtoA(all); }).join();
    Check(theirs == "255.255.255.255", "inet: SOInetNtoA on another thread");
    Check(std::strcmp(mine, "192.168.0.1") == 0, "inet: SOInetNtoA on another thread must not overwrite this thread's result");
}

void HostByName()
{
    const SOHostEnt* host = SOGetHostByName("127.0.0.1");
    Check(host != nullptr, "hostbyname: SOGetHostByName(\"127.0.0.1\")");
    CheckResult(host->addrType, SO_PF_INET, "hostbyname: addrType");
    CheckResult(host->length, (int)sizeof(SOInAddr), "hostbyname: length");
    Check(host->name != nullptr && std::strcmp(host->name, "127.0.0.1") == 0, "hostbyname: name of a numeric host");
    Check(host->aliases != nullptr && host->aliases[0] == nullptr, "hostbyname: empty alias list");
    Check(host->addrList != nullptr && host->addrList[0] != nullptr, "hostbyname: at least one address");
    Check(AddrOf(host->addrList[0]) == SOHtoNl(kLoopbackAddr), "hostbyname: 127.0.0.1 in network order");
    Check(host->addrList[1] == nullptr, "hostbyname: a numeric host has one address");

    // NHTTP_socket_RVL.c and gsSocketRevolution.c resolve names, not numbers
    host = SOGetHostByName("localhost");
    Check(host != nullptr, "hostbyname: SOGetHostByName(\"localhost\")");
    for (int i = 0; host->addrList[i] != nullptr; i++)
        Check(IsLoopback(host->addrList[i]), "hostbyname: every address of localhost is in 127.0.0.0/8");

    // Like the SDK's soWork buffer: one result per thread, overwritten by the
    // next call on the same thread.
    const SOHostEnt* first = SOGetHostByName("127.0.0.1");
    const SOHostEnt* second = SOGetHostByName("127.0.0.2");
    Check(first == second, "hostbyname: one result buffer per thread");
    Check(AddrOf(second->addrList[0]) == SOHtoNl(0x7f000002), "hostbyname: the second call's address");
    const SOHostEnt* other = nullptr;
    u32 other_addr = 0;
    std::thread([&] {
        other = SOGetHostByName("127.0.0.3");
        other_addr = other != nullptr ? AddrOf(other->addrList[0]) : 0;
    }).join();
    Check(other != nullptr && other != second, "hostbyname: another thread gets its own buffer");
    Check(other_addr == SOHtoNl(0x7f000003), "hostbyname: the other thread's address");
    Check(AddrOf(second->addrList[0]) == SOHtoNl(0x7f000002), "hostbyname: another thread must not overwrite this one's result");

    Check(SOGetHostByName(nullptr) == nullptr, "hostbyname: SOGetHostByName(NULL)");
}

void AddrInfo()
{
    // No socket type: one entry for TCP and one for UDP, raw sockets skipped
    SOAddrInfo* res = nullptr;
    const Allocations before = allocations;
    CheckResult(SOGetAddrInfo("127.0.0.1", "80", nullptr, &res), SO_SUCCESS, "addrinfo: SOGetAddrInfo(\"127.0.0.1\", \"80\")");
    CheckResult((int)(allocations.allocs - before.allocs), 1, "addrinfo: one block from SOInit's allocator");
    CheckResult((int)allocations.last_name, 10, "addrinfo: allocation name 10, like SOiAlloc in SOGetAddrInfo");
    Check(res == allocations.last_ptr, "addrinfo: the list starts at the allocated block");
    const s32 block_size = allocations.last_size;
    bool stream = false, dgram = false;
    int entries = 0;
    for (const SOAddrInfo* ai = res; ai != nullptr; ai = ai->next)
    {
        entries++;
        CheckResult(ai->family, SO_PF_INET, "addrinfo: entry family");
        CheckResult((int)ai->addrLen, (int)sizeof(SOSockAddrIn), "addrinfo: entry addrLen");
        Check(ai->canonName == nullptr, "addrinfo: no canonName");
        Check((const u8*)ai->addr >= (const u8*)res && (const u8*)ai->addr < (const u8*)res + block_size,
              "addrinfo: the address lives in the same block, so SOFreeAddrInfo frees it too");
        const SOSockAddrIn* sin = (const SOSockAddrIn*)ai->addr;
        CheckResult(sin->len, (int)sizeof(SOSockAddrIn), "addrinfo: address len");
        CheckResult(sin->family, SO_PF_INET, "addrinfo: address family");
        Check(sin->port == SOHtoNs(80), "addrinfo: port 80 in network order");
        Check(sin->addr.addr == SOHtoNl(kLoopbackAddr), "addrinfo: 127.0.0.1 in network order");
        if (ai->sockType == SO_SOCK_STREAM)
        {
            stream = true;
            CheckResult(ai->protocol, SO_IPPROTO_TCP, "addrinfo: stream entry protocol");
        }
        else if (ai->sockType == SO_SOCK_DGRAM)
        {
            dgram = true;
            CheckResult(ai->protocol, SO_IPPROTO_UDP, "addrinfo: datagram entry protocol");
        }
        else
            Check(false, "addrinfo: entry socket type must be stream or datagram");
    }
    CheckResult(entries, 2, "addrinfo: one entry per socket type");
    Check(stream && dgram, "addrinfo: a stream and a datagram entry");

    const unsigned frees = allocations.frees;
    SOFreeAddrInfo(res);
    CheckResult((int)(allocations.frees - frees), 1, "addrinfo: SOFreeAddrInfo frees once");
    Check(allocations.last_ptr == res, "addrinfo: SOFreeAddrInfo frees the allocated block");
    CheckResult((int)allocations.last_name, 10, "addrinfo: SOFreeAddrInfo frees with name 10");
    CheckResult(allocations.last_size, block_size, "addrinfo: SOFreeAddrInfo frees with the allocated size");

    SOAddrInfo hints = { .family = 0, .sockType = SO_SOCK_DGRAM };
    CheckResult(SOGetAddrInfo("127.0.0.1", "27900", &hints, &res), SO_SUCCESS, "addrinfo: SOGetAddrInfo with a datagram hint");
    Check(res->next == nullptr && res->sockType == SO_SOCK_DGRAM, "addrinfo: a datagram hint gives one datagram entry");
    Check(((const SOSockAddrIn*)res->addr)->port == SOHtoNs(27900), "addrinfo: port 27900 in network order");
    SOFreeAddrInfo(res);

    // getaddrinfo picks loopback for a missing node without AI_PASSIVE
    CheckResult(SOGetAddrInfo(nullptr, "80", nullptr, &res), SO_SUCCESS, "addrinfo: SOGetAddrInfo with no node");
    Check(IsLoopback((const u8*)&((const SOSockAddrIn*)res->addr)->addr), "addrinfo: no node gives a loopback address");
    SOFreeAddrInfo(res);

    hints = { .family = SO_PF_INET6 };
    CheckResult(SOGetAddrInfo("127.0.0.1", nullptr, &hints, &res), SO_EPROTONOSUPPORT, "addrinfo: SO_PF_INET6 hint");
    Check(res == nullptr, "addrinfo: a failed call leaves *res NULL");
    hints = { .family = 99 };
    CheckResult(SOGetAddrInfo("127.0.0.1", nullptr, &hints, &res), SO_EAFNOSUPPORT, "addrinfo: unknown family hint");
    hints = { .sockType = 99 };
    CheckResult(SOGetAddrInfo("127.0.0.1", nullptr, &hints, &res), SO_EPROTOTYPE, "addrinfo: unknown socket type hint");
    CheckResult(SOGetAddrInfo("127.0.0.1", "msc-no-such-service", nullptr, &res), SO_EINVAL,
                "addrinfo: unknown service name");
    CheckResult(SOGetAddrInfo("127.0.0.1", nullptr, nullptr, nullptr), SO_EINVAL, "addrinfo: no res");

    const unsigned allocs = allocations.allocs;
    allocations.fail_next = true;
    CheckResult(SOGetAddrInfo("127.0.0.1", "80", nullptr, &res), SO_ENOMEM, "addrinfo: the allocator fails");
    CheckResult((int)(allocations.allocs - allocs), 1, "addrinfo: the failed allocation was attempted once");
    Check(res == nullptr, "addrinfo: *res stays NULL when the allocator fails");
}

// The first IPv4 address each lookup gives for name, in network order; 0 when
// the lookup fails. The SOGetAddrInfo one also checks the port it was asked for.
u32 HostByNameAddr(const char* name)
{
    const SOHostEnt* host = SOGetHostByName(name);
    return host != nullptr && host->addrList[0] != nullptr ? AddrOf(host->addrList[0]) : 0;
}

u32 AddrInfoAddr(const char* name, const char* service, u16 port, const char* message)
{
    SOAddrInfo hints = { .family = SO_PF_INET, .sockType = SO_SOCK_DGRAM };
    SOAddrInfo* res = nullptr;
    if (SOGetAddrInfo(name, service, &hints, &res) != SO_SUCCESS)
        return 0;
    const SOSockAddrIn* sin = (const SOSockAddrIn*)res->addr;
    Check(sin->port == SOHtoNs(port), message);
    const u32 addr = sin->addr.addr;
    SOFreeAddrInfo(res);
    return addr;
}

// online.server: every *.nintendowifi.net name resolves to the configured
// server instead, by both lookups. 127.0.0.2 is the server so that a result
// can be told apart from an /etc/hosts entry sending the WFC names to 127.0.0.1.
void ServerOverride()
{
    const u32 server = SOHtoNl(0x7f000002);
    so_set_server_override("127.0.0.2");
    Check(HostByNameAddr("gpcm.gs.nintendowifi.net") == server,
          "override: SOGetHostByName sends a WFC name to the server");
    Check(AddrInfoAddr("natneg1.gs.nintendowifi.net", "27901", 27901,
                       "override: SOGetAddrInfo keeps the requested port") == server,
          "override: SOGetAddrInfo sends a WFC name to the server");
    Check(HostByNameAddr("mschargedwii.ms11.gs.nintendowifi.net") == server,
          "override: a name built at run time (server browser) goes to the server");
    Check(HostByNameAddr("NASWII.NintendoWifi.NET") == server, "override: names compare without case");

    // Only names under the domain: the dot before it is part of the rule
    Check(HostByNameAddr("127.0.0.1") == SOHtoNl(kLoopbackAddr), "override: a numeric host is not redirected");
    const u32 local = HostByNameAddr("localhost");
    Check(local != 0 && local != server, "override: localhost is not redirected");
    Check(HostByNameAddr("evilnintendowifi.net") != server, "override: a name ending in the domain without its dot");
    Check(HostByNameAddr("nintendowifi.net.invalid") != server, "override: a name only starting with the domain");

    // The server may be a name, resolved by the host resolver
    so_set_server_override("localhost");
    const u32 named = HostByNameAddr("gpsp.gs.nintendowifi.net");
    Check(named != 0 && IsLoopback((const u8*)&named), "override: a server given by name");

    // A name that does not fit is refused and the previous server stays
    so_set_server_override("127.0.0.2");
    const std::string too_long(256, 'a');
    so_set_server_override(too_long.c_str());
    Check(HostByNameAddr("gpcm.gs.nintendowifi.net") == server, "override: a 256-character server is refused");
    // 255 characters fit the buffer: accepted, though no resolver knows it
    const std::string longest(255, 'a');
    so_set_server_override(longest.c_str());
    Check(HostByNameAddr("gpcm.gs.nintendowifi.net") != server, "override: a 255-character server is accepted");

    // An empty server and NULL turn the redirect off: the names are looked up
    // as they are, so /etc/hosts or DNS answer for them again
    so_set_server_override("127.0.0.2");
    so_set_server_override("");
    Check(HostByNameAddr("gpcm.gs.nintendowifi.net") != server, "override: an empty server turns the redirect off");
    so_set_server_override("127.0.0.2");
    so_set_server_override(nullptr);
    Check(HostByNameAddr("gpcm.gs.nintendowifi.net") != server, "override: NULL turns the redirect off");
}

// getlocalhost (gsPlatformSocket.c) asks for the count, sizes the table with
// it and ignores both return values: these two calls must always work.
void InterfaceOpt(u32 host)
{
    int count = -1;
    int len = sizeof(count);
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_NUMBER, &count, &len), SO_SUCCESS,
                "ifopt: SO_CONFIG_IP_ADDR_NUMBER");
    CheckResult(count, 1, "ifopt: one address");
    CheckResult(len, (int)sizeof(int), "ifopt: SO_CONFIG_IP_ADDR_NUMBER length");

    IPAddrEntry table[2];
    std::memset(table, 0xaa, sizeof(table));
    len = count * (int)sizeof(IPAddrEntry);
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_TABLE, table, &len), SO_SUCCESS,
                "ifopt: SO_CONFIG_IP_ADDR_TABLE");
    CheckResult(len, (int)sizeof(IPAddrEntry), "ifopt: SO_CONFIG_IP_ADDR_TABLE length");
    const u32 addr = AddrOf(table[0].addr), mask = AddrOf(table[0].netMask), bcast = AddrOf(table[0].bcastAddr);
    Check(addr == host, "ifopt: the table address must be SOGetHostID's");
    Check(mask != 0, "ifopt: a netmask");
    Check((bcast & mask) == (addr & mask), "ifopt: the broadcast address is in the address's subnet");
    Check((bcast | mask) == 0xffffffff, "ifopt: the broadcast address has every host bit set");

    // A bigger buffer reports the real length and leaves the rest alone
    std::memset(table, 0xaa, sizeof(table));
    len = sizeof(table);
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_TABLE, table, &len), SO_SUCCESS,
                "ifopt: SO_CONFIG_IP_ADDR_TABLE with room for two entries");
    CheckResult(len, (int)sizeof(IPAddrEntry), "ifopt: the length of one entry");
    CheckResult(table[1].addr[0], 0xaa, "ifopt: the second entry is untouched");

    // Too short: nothing copied, *optlen says how much is needed
    for (int short_len : { 0, (int)sizeof(IPAddrEntry) - 1, -1 })
    {
        std::memset(table, 0xaa, sizeof(table));
        len = short_len;
        CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_TABLE, table, &len), SO_EINVAL,
                    "ifopt: SO_CONFIG_IP_ADDR_TABLE with a short or negative length");
        CheckResult(len, (int)sizeof(IPAddrEntry), "ifopt: a short length reports the needed one");
        CheckResult(table[0].addr[0], 0xaa, "ifopt: nothing is copied into a short buffer");
    }
    count = -1;
    len = -1;
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_NUMBER, &count, &len), SO_EINVAL,
                "ifopt: SO_CONFIG_IP_ADDR_NUMBER with a negative length");
    CheckResult(len, (int)sizeof(int), "ifopt: SO_CONFIG_IP_ADDR_NUMBER needed length");
    CheckResult(count, -1, "ifopt: nothing is copied with a negative length");

    // No buffer: only the length
    len = sizeof(int);
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_NUMBER, nullptr, &len), SO_SUCCESS,
                "ifopt: SO_CONFIG_IP_ADDR_NUMBER with no buffer");
    len = sizeof(IPAddrEntry);
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_TABLE, nullptr, &len), SO_SUCCESS,
                "ifopt: SO_CONFIG_IP_ADDR_TABLE with no buffer");

    len = sizeof(count);
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_NUMBER, &count, nullptr), SO_EINVAL,
                "ifopt: no optlen");
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_FILTER_INPUT, &count, &len), SO_EINVAL,
                "ifopt: SO_CONFIG_FILTER_INPUT");
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_FILTER_OUTPUT, &count, &len), SO_EINVAL,
                "ifopt: SO_CONFIG_FILTER_OUTPUT");
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_MAC_ADDRESS, &count, &len), SO_ENOPROTOOPT,
                "ifopt: SO_CONFIG_MAC_ADDRESS is not supported");
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_SOCKET, SO_CONFIG_IP_ADDR_NUMBER, &count, &len), SO_ENOPROTOOPT,
                "ifopt: SO_CONFIG_IP_ADDR_NUMBER at SO_SOL_SOCKET");

    // The interface is read on every call, so the override needs no restart.
    // Loopback has no broadcast address: addr | ~netmask stands in for it.
    const char* env_iface = std::getenv("RECHARGED_NET_IFACE");
    const std::string saved_iface = env_iface != nullptr ? env_iface : "";
    SetEnv("RECHARGED_NET_IFACE", kLoopbackIface);
    len = sizeof(IPAddrEntry);
    CheckResult(SOGetInterfaceOpt(nullptr, SO_SOL_CONFIG, SO_CONFIG_IP_ADDR_TABLE, table, &len), SO_SUCCESS,
                "ifopt: SO_CONFIG_IP_ADDR_TABLE with RECHARGED_NET_IFACE=lo");
    Check(AddrOf(table[0].addr) == SOHtoNl(kLoopbackAddr), "ifopt: lo address 127.0.0.1");
    Check(AddrOf(table[0].netMask) == SOHtoNl(0xff000000), "ifopt: lo netmask 255.0.0.0");
    Check(AddrOf(table[0].bcastAddr) == SOHtoNl(0x7fffffff), "ifopt: lo broadcast 127.255.255.255");
    if (env_iface != nullptr)
        SetEnv("RECHARGED_NET_IFACE", saved_iface.c_str());
    else
        UnsetEnv("RECHARGED_NET_IFACE");
}

// SOFreeAddrInfo after SOFinish frees nothing, like SOiFree behind it.
void ResolverFinish()
{
    SOAddrInfo* res = nullptr;
    CheckResult(SOGetAddrInfo("127.0.0.1", "80", nullptr, &res), SO_SUCCESS, "resolver: SOGetAddrInfo before SOFinish");
    CheckResult(SOCleanup(), SO_SUCCESS, "resolver: SOCleanup (ACTIVE -> READY)");
    CheckResult(SOFinish(), SO_SUCCESS, "resolver: SOFinish (READY -> TERMINATED)");
    const unsigned frees = allocations.frees;
    SOFreeAddrInfo(res);
    CheckResult((int)(allocations.frees - frees), 0, "resolver: SOFreeAddrInfo after SOFinish frees nothing");
    CheckResult(allocations.live, 1, "resolver: every block but the one left after SOFinish went back to the allocator");
    std::free(res);
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
        SockOpt(udp, closed);
        Poll(udp, closed);
        Shutdown(udp, closed);
        const u32 host = HostId();
        Tcp(closed);
        const bool broadcast = Broadcast();
        AfterCleanup(udp);
        ResolverStates();
        InetConversions();
        HostByName();
        AddrInfo();
        ServerOverride();
        InterfaceOpt((u32)SOGetHostID());
        ResolverFinish();
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
