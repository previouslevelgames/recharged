#include "platform/ai.h"
#include "platform/interrupts.h"
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// Late owner delivery of the AI cause. Native callbacks run only at owner safe
// points; a host delay must not make the DMA replay source buffers whose next
// registers the original callback has not yet written.
namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
std::atomic_uint checks{};
unsigned produced{};
constexpr unsigned kBlockIds = 30000;
std::thread::id owner;
alignas(32) std::array<s16, 192> buffers[2];
unsigned next_buffer{};
std::mutex recorded_mutex;
std::vector<std::array<s16, 192>> recorded;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }

// Source mode0 ordering: select the next buffer, program it, then fill it.
void Producer() {
    Check(std::this_thread::get_id() == owner && !NativeInterruptsEnabled(), "DMA callback left its owner");
    next_buffer ^= 1;
    auto* buffer = buffers[next_buffer].data();
    AIInitDMA(reinterpret_cast<std::uintptr_t>(buffer), sizeof(buffers[0]));
    // Nonzero block identity on the left, sample position on the right.
    for (unsigned n = 0; n < 96; ++n) {
        buffer[n * 2] = s16(produced % kBlockIds + 1);
        buffer[n * 2 + 1] = s16(n + 1);
    }
    ++produced;
}

struct OwnerTiming {
    std::uint64_t maximum_service_gap_ns{};
    std::uint64_t maximum_stall_ns{};
    unsigned stalls{};
};

OwnerTiming Serve(std::chrono::milliseconds duration, std::chrono::milliseconds stall_every,
                  std::chrono::milliseconds stall) {
    OwnerTiming timing;
    const auto end = std::chrono::steady_clock::now() + duration;
    auto next_stall = std::chrono::steady_clock::now() + stall_every;
    auto last_service_ns = SDL_GetTicksNS();
    auto service = [&] {
        const auto now = SDL_GetTicksNS();
        timing.maximum_service_gap_ns = std::max(timing.maximum_service_gap_ns, now - last_service_ns);
        last_service_ns = now;
        ServiceNativeAI();
    };
    while (std::chrono::steady_clock::now() < end) {
        if (stall.count() && std::chrono::steady_clock::now() >= next_stall) {
            // Host owner busy outside a source critical section; the device
            // worker keeps pulling and transferring. Measure actual elapsed
            // delay because a requested sleep can overshoot under contention.
            const auto before = SDL_GetTicksNS();
            std::this_thread::sleep_for(stall);
            timing.maximum_stall_ns = std::max(timing.maximum_stall_ns, SDL_GetTicksNS() - before);
            ++timing.stalls;
            next_stall = std::chrono::steady_clock::now() + stall_every;
        }
        service();
        std::this_thread::sleep_for(200us);
    }
    service();
    return timing;
}

// Every submitted block must be the next authored source block: no replay of a
// stale buffer and no skipped source block, whatever the owner timing.
unsigned Contiguous() {
    std::lock_guard lock(recorded_mutex);
    int previous = -1;
    unsigned audible = 0;
    for (const auto& block : recorded) {
        if (block[0] == 0) { Check(previous < 0, "source PCM became silent mid-stream"); continue; }
        const int index = int(block[0]) - 1;
        if (previous >= 0 && index != int((unsigned(previous) + 1) % kBlockIds))
            std::cerr << "block sequence " << previous << " -> " << index << " at record " << (&block - recorded.data())
                      << " of " << recorded.size() << '\n';
        Check(previous < 0 || index == int((unsigned(previous) + 1) % kBlockIds),
              "DMA replayed or skipped an authored source block");
        for (unsigned n = 0; n < 96; ++n)
            Check(block[n * 2] == s16(index + 1) && block[n * 2 + 1] == s16(n + 1),
                  "DMA changed authored sample order inside a block");
        previous = index;
        ++audible;
    }
    return audible;
}
} // namespace

extern "C" bool __real_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream*, const void*, int, SDL_AudioStreamDataCompleteCallback, void*);
extern "C" bool __wrap_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream* stream, const void* bytes, int size,
                                                    SDL_AudioStreamDataCompleteCallback done, void* context) {
    Check(size == 384, "96-frame source DMA length changed at the SDL boundary");
    std::array<s16, 192> block;
    std::memcpy(block.data(), bytes, size);
    { std::lock_guard lock(recorded_mutex); recorded.push_back(block); }
    return __real_SDL_PutAudioStreamDataNoCopy(stream, bytes, size, done, context);
}

int main() {
    try {
        owner = std::this_thread::get_id();
        AIInit(nullptr);
        const auto device = GetNativeAIStatus();
        Check(device.device_frequency == 44100 && device.device_frames == 441,
              "dummy fixture needs 441 frames at 44.1 kHz for an exact 10 ms nominal pull");
        for (auto& buffer : buffers) buffer.fill(0);
        AIRegisterDMACallback(Producer);
        AIInitDMA(reinterpret_cast<std::uintptr_t>(buffers[0].data()), sizeof(buffers[0]));
        BeginNativeAIObservations();
        AIStartDMA();
        // Repeated 9 ms owner delays: three DMA periods, within the allowance.
        const auto short_timing = Serve(800ms, 40ms, 9ms);
        auto delivery = GetNativeAIDeliveryStatus();
        const auto observed = GetNativeAIObservationStatus();
        const auto short_stalls = Contiguous();
        // Print real timing/queue evidence even when the following invariant
        // fails. SDL callback demand is in the unconverted input byte domain.
        std::cout << "{\"phase\":\"short_stalls\",\"device_rate\":" << device.device_frequency
                  << ",\"device_frames\":" << device.device_frames << ",\"blocks\":" << short_stalls
                  << ",\"stalls\":" << short_timing.stalls
                  << ",\"maximum_stall_ns\":" << short_timing.maximum_stall_ns
                  << ",\"maximum_owner_gap_ns\":" << short_timing.maximum_service_gap_ns
                  << ",\"held\":" << delivery.held_boundaries << ",\"maximum_hold_ns\":" << delivery.maximum_hold_ns
                  << ",\"sdl_short_pulls\":" << delivery.sdl_short_pulls
                  << ",\"sdl_short_input_bytes\":" << delivery.sdl_short_input_bytes
                  << ",\"minimum_queue_bytes\":" << delivery.minimum_queued_input_bytes
                  << ",\"maximum_queue_bytes\":" << delivery.maximum_queued_input_bytes
                  << ",\"maximum_pull_gap_ns\":" << observed.maximum_sdl_pull_gap_ns
                  << ",\"pulls\":" << observed.sdl_pull_calls << "}\n";
        Check(short_stalls > 150, "timed fixture did not deliver enough authored PCM");
        Check(delivery.held_boundaries > 0, "owner delays never reached a completed DMA block");
        Check(!delivery.replayed_latches && !delivery.coalesced_causes,
              "late owner delivery replayed unprogrammed DMA registers");
        Check(!delivery.skipped_cells, "short owner delays dropped DMA clock time");
        Check(!delivery.sdl_short_pulls, "device pull lacked transferred PCM (see actual owner/device timings above)");

        // One 100 ms owner stall: silence is allowed, replay and unbounded
        // catch-up latency are not.
        BeginNativeAIObservations();
        Serve(100ms, 1000ms, 0ms);
        std::this_thread::sleep_for(100ms);
        Serve(300ms, 1000ms, 0ms);
        delivery = GetNativeAIDeliveryStatus();
        Contiguous();
        Check(delivery.held_boundaries > 0 && delivery.skipped_cells > 0,
              "long owner stall was not bounded by skipping DMA clock time");
        Check(!delivery.replayed_latches && !delivery.coalesced_causes,
              "long owner stall replayed unprogrammed DMA registers");
        const auto status = GetNativeAIStatus();
        // Lead plus the bounded catch-up stays under 40 ms of 32 kHz stereo
        // input; replaying the full 100 ms backlog would exceed it.
        Check(status.queued_input_bytes <= 4 * 32000 * 40 / 1000,
              "catch-up after a long stall accumulated unbounded output latency");
        std::cout << "{\"phase\":\"long_stall\",\"skipped_cells\":" << delivery.skipped_cells
                  << ",\"maximum_hold_ns\":" << delivery.maximum_hold_ns << ",\"queued_input_bytes\":"
                  << status.queued_input_bytes << "}\n";

        // A masked owner keeps the hardware's replay of unrewritten registers.
        BeginNativeAIObservations();
        const auto masked = OSDisableInterrupts();
        const auto end = std::chrono::steady_clock::now() + 20ms;
        while (std::chrono::steady_clock::now() < end) { ServiceNativeAI(); std::this_thread::sleep_for(200us); }
        OSRestoreInterrupts(masked);
        delivery = GetNativeAIDeliveryStatus();
        Check(delivery.replayed_latches > 0 && delivery.coalesced_causes > 0,
              "masked owner no longer receives the hardware register replay");
        AIStopDMA();
        Check(GetNativeAIStatus().retained_blocks == 0, "stop retained queued PCM ownership");
        AIReset();
        SDL_Quit();
        std::cout << "native AI late-delivery checks=" << checks.load() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native AI delivery: " << error.what() << '\n';
        return 1;
    }
}
