// SO backend shared by several threads, as DWC, GameSpy and NHTTP share it:
// SOClose waking operations other threads are blocked in, slot ownership while
// many threads open and close sockets, SOCleanup racing with socket use, and
// parallel traffic on separate sockets. Workers only record what they saw; the
// main thread checks it after joining them. The ThreadSanitizer build of this
// suite (MSCHARGED_TSAN_TESTS) also reports unguarded accesses that happened
// not to break a check.
#include <revolution/so.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "so_test_peer.h"

namespace
{
using namespace std::chrono_literals;

constexpr int kMaxSockets = 32;
// Long enough for a started thread to be parked in the blocking call.
constexpr auto kBlockDelay = 100ms;
// XNU's soshutdown() returns ENOTCONN without waking anyone on a socket that
// is not connected, so SOClose cannot wake a thread blocked on an unconnected
// UDP socket on macOS. Those cases run there only with --udp-wakeups.
#if defined(__APPLE__)
constexpr bool kShutdownWakesDatagramSockets = false;
#else
constexpr bool kShutdownWakesDatagramSockets = true;
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

SOSockAddrIn Loopback(u16 network_port)
{
    SOSockAddrIn addr{};
    addr.len = sizeof(addr);
    addr.family = SO_PF_INET;
    addr.port = network_port;
    addr.addr.addr = SOHtoNl(0x7f000001);
    return addr;
}

int BoundUdp(SOSockAddrIn* bound)
{
    const int s = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(s >= 0, "SOSocket2 for a UDP socket failed");
    SOSockAddrIn any_port = Loopback(0);
    CheckResult(SOBind(s, &any_port), SO_SUCCESS, "SOBind on 127.0.0.1:0 failed");
    *bound = SOSockAddrIn{ .len = sizeof(SOSockAddrIn) };
    CheckResult(SOGetSockName(s, bound), SO_SUCCESS, "SOGetSockName on a bound UDP socket failed");
    return s;
}

// Every slot free and handed out lowest first, as the suites expect.
void CheckTableEmpty(const char* message)
{
    int sockets[kMaxSockets];
    for (int i = 0; i < kMaxSockets; ++i)
    {
        sockets[i] = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
        CheckResult(sockets[i], i, message);
    }
    CheckResult(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), SO_EMFILE, message);
    for (int s : sockets)
        CheckResult(SOClose(s), SO_SUCCESS, message);
}

void CloseWakesBlockedDatagramReceive()
{
    SOSockAddrIn bound;
    const int s = BoundUdp(&bound);
    std::atomic<int> received{1};
    std::thread reader([&] {
        u8 buf[16];
        received = SORecvFrom(s, buf, sizeof(buf), 0, nullptr);
    });
    std::this_thread::sleep_for(kBlockDelay);
    CheckResult(SOClose(s), SO_SUCCESS, "udp: SOClose while another thread waits in SORecvFrom");
    const int other = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    Check(other >= 0 && other != s, "udp: the slot of a blocked thread was handed out again");
    reader.join();
    CheckResult(received, SO_ENOTCONN, "udp: SORecvFrom interrupted by SOClose must fail like IOS");
    CheckResult(SOClose(other), SO_SUCCESS, "udp: SOClose on the second socket");
    CheckTableEmpty("udp: the interrupted slot must be free once its thread returned");
}

void CloseWakesBlockedStreamReceive()
{
    u16 port = 0;
    const int listener = so_test_peer_listen(&port);
    Check(listener >= 0, "tcp: POSIX listener on 127.0.0.1:0");
    const int s = SOSocket2(SO_PF_INET, SO_SOCK_STREAM, 0);
    Check(s >= 0, "tcp: SOSocket2 for the stream client");
    SOSockAddrIn server = Loopback(port);
    CheckResult(SOConnect(s, &server), SO_SUCCESS, "tcp: SOConnect to the listener");
    const int peer = so_test_peer_accept(listener, nullptr, nullptr);
    Check(peer >= 0, "tcp: POSIX accept of the SO client");

    std::atomic<int> received{1};
    std::thread reader([&] {
        u8 buf[16];
        received = SORecvFrom(s, buf, sizeof(buf), 0, nullptr);
    });
    std::this_thread::sleep_for(kBlockDelay);
    CheckResult(SOClose(s), SO_SUCCESS, "tcp: SOClose while another thread waits in SORecvFrom");
    reader.join();
    CheckResult(received, SO_ENOTCONN, "tcp: SORecvFrom interrupted by SOClose must fail like IOS");
    u8 byte;
    Check(so_test_peer_recv(peer, &byte, 1) == 0, "tcp: the peer must see the connection closed");
    so_test_peer_close(peer);
    so_test_peer_close(listener);
    CheckTableEmpty("tcp: the interrupted slot must be free once its thread returned");
}

void CloseWakesBlockedPoll()
{
    SOSockAddrIn bound;
    const int s = BoundUdp(&bound);
    std::atomic<int> ready{-1000};
    std::thread poller([&] {
        SOPollFD pfd = { .fd = s, .events = SO_POLLRDNORM, .revents = 0 };
        ready = SOPoll(&pfd, 1, SO_INFTIM);
    });
    std::this_thread::sleep_for(kBlockDelay);
    CheckResult(SOClose(s), SO_SUCCESS, "poll: SOClose while another thread waits in SOPoll");
    poller.join();
    CheckResult(ready, 1, "poll: SOPoll must return when another thread closes its socket");
    CheckTableEmpty("poll: the polled slot must be free once its thread returned");
}

void ConcurrentOpenClose()
{
    constexpr int kThreads = 8;
    constexpr int kRounds = 2000;
    const int baseline = so_test_peer_open_fds();
    std::atomic<int> owners[kMaxSockets] = {};
    std::atomic<int> shared{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&] {
            for (int i = 0; i < kRounds; ++i)
            {
                const int s = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
                if (s < 0 || s >= kMaxSockets)
                {
                    ++failures;
                    continue;
                }
                if (owners[s].fetch_add(1) != 0)
                    ++shared;
                if (SOFcntl(s, SO_F_GETFL, 0) != SO_SUCCESS)
                    ++failures;
                owners[s].fetch_sub(1);
                if (SOClose(s) != SO_SUCCESS)
                    ++failures;
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    CheckResult(shared, 0, "stress: two threads were given the same slot at once");
    CheckResult(failures, 0, "stress: an open, use and close round failed");
    CheckResult(so_test_peer_open_fds(), baseline, "stress: native descriptors leaked");
    CheckTableEmpty("stress: the table must be empty after the workers closed everything");
}

void CleanupWhileInUse()
{
    constexpr int kThreads = 4;
    constexpr int kRestarts = 200;
    const int baseline = so_test_peer_open_fds();
    const SOSockAddrIn discard = Loopback(SOHtoNs(9));
    const u8 payload[4] = { 'M', 'S', 'C', 0 };
    std::atomic<bool> stop{false};
    std::atomic<int> unexpected{0};
    std::atomic<int> rounds{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&] {
            while (!stop)
            {
                const int s = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
                if (s < 0)
                {
                    if (s != SO_EINVAL)
                        ++unexpected;
                    continue;
                }
                // A restart lets another thread reopen the slot, so a stale
                // descriptor may legitimately reach someone else's socket.
                const int sent = SOSendTo(s, payload, sizeof(payload), 0, &discard);
                if (sent != (int)sizeof(payload) && sent != SO_EINVAL && sent != SO_EBADF && sent != SO_ENOTCONN)
                    ++unexpected;
                const int closed = SOClose(s);
                if (closed != SO_SUCCESS && closed != SO_EINVAL && closed != SO_EBADF)
                    ++unexpected;
                ++rounds;
            }
        });
    }
    for (int i = 0; i < kRestarts; ++i)
    {
        CheckResult(SOCleanup(), SO_SUCCESS, "cleanup race: SOCleanup while workers use sockets");
        std::this_thread::sleep_for(100us);
        CheckResult(SOStartup(), SO_SUCCESS, "cleanup race: SOStartup while workers use sockets");
        std::this_thread::sleep_for(100us);
    }
    stop = true;
    for (auto& worker : workers)
        worker.join();
    CheckResult(unexpected, 0, "cleanup race: a call returned an error no interleaving explains");
    Check(rounds > 0, "cleanup race: the workers never completed a round");
    CheckResult(SOCleanup(), SO_SUCCESS, "cleanup race: final SOCleanup");
    CheckResult(so_test_peer_open_fds(), baseline, "cleanup race: native descriptors leaked");
    CheckResult(SOStartup(), SO_SUCCESS, "cleanup race: SOStartup after the race");
    CheckTableEmpty("cleanup race: the table must be empty after the restarts");
}

void ParallelTraffic()
{
    constexpr int kPairs = 4;
    constexpr int kDatagrams = 200;
    struct Datagram { int pair, sequence; };
    struct Pair { int rx, tx; SOSockAddrIn rx_addr; std::atomic<int> acked{0}; };
    Pair pairs[kPairs];
    for (auto& pair : pairs)
    {
        SOSockAddrIn tx_addr;
        pair.rx = BoundUdp(&pair.rx_addr);
        pair.tx = BoundUdp(&tx_addr);
        CheckResult(SOFcntl(pair.rx, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "traffic: make a receiver non-blocking");
    }

    std::atomic<int> misrouted{0};
    std::atomic<int> failures{0};
    std::atomic<int> delivered{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < kPairs; ++p)
    {
        threads.emplace_back([&, p] {
            Pair& pair = pairs[p];
            for (int i = 0; i < kDatagrams; ++i)
            {
                while (pair.acked != i)
                    std::this_thread::yield();
                const Datagram out = { p, i };
                if (SOSendTo(pair.tx, &out, sizeof(out), 0, &pair.rx_addr) != (int)sizeof(out))
                    ++failures;
            }
        });
        threads.emplace_back([&, p] {
            Pair& pair = pairs[p];
            for (int i = 0; i < kDatagrams; ++i)
            {
                SOPollFD pfd = { .fd = pair.rx, .events = SO_POLLRDNORM, .revents = 0 };
                Datagram in{};
                if (SOPoll(&pfd, 1, SO_INFTIM) != 1
                    || SORecvFrom(pair.rx, &in, sizeof(in), 0, nullptr) != (int)sizeof(in))
                    ++failures;
                else if (in.pair != p || in.sequence != i)
                    ++misrouted;
                else
                    ++delivered;
                pair.acked = i + 1;
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    CheckResult(failures, 0, "traffic: a send, poll or receive failed");
    CheckResult(misrouted, 0, "traffic: a datagram reached the wrong socket or arrived out of order");
    CheckResult(delivered, kPairs * kDatagrams, "traffic: datagrams were lost");
    for (auto& pair : pairs)
    {
        CheckResult(SOClose(pair.rx), SO_SUCCESS, "traffic: SOClose on a receiver");
        CheckResult(SOClose(pair.tx), SO_SUCCESS, "traffic: SOClose on a sender");
    }
    CheckTableEmpty("traffic: the table must be empty after closing every pair");
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const bool udp_wakeups = kShutdownWakesDatagramSockets
            || (argc == 2 && std::string(argv[1]) == "--udp-wakeups");
        SOLibraryConfig config = { .alloc = (SOAlloc)0xf0cacc1a, .free = (SOFree)0xbadc0ffee0ddf00d };
        CheckResult(SOInit(&config), SO_SUCCESS, "SOInit before the thread tests");
        CheckResult(SOStartup(), SO_SUCCESS, "SOStartup before the thread tests");
        if (udp_wakeups)
            CloseWakesBlockedDatagramReceive();
        CloseWakesBlockedStreamReceive();
        if (udp_wakeups)
            CloseWakesBlockedPoll();
        ConcurrentOpenClose();
        CleanupWhileInUse();
        ParallelTraffic();
        CheckResult(SOCleanup(), SO_SUCCESS, "SOCleanup after the thread tests");
        CheckResult(SOFinish(), SO_SUCCESS, "SOFinish after the thread tests");
        std::cout << "SO backend threads: " << checks << " checks"
                  << (udp_wakeups ? "" : ", SOClose wake-ups on UDP sockets skipped on macOS") << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SO backend threads failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
