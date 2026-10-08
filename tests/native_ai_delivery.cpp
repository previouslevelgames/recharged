#include "platform/ai.h"
#include "platform/interrupts.h"
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>
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
unsigned checks{};
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

void Serve(std::chrono::milliseconds duration, std::chrono::milliseconds stall_every,
           std::chrono::milliseconds stall) {
    const auto end = std::chrono::steady_clock::now() + duration;
    auto next_stall = std::chrono::steady_clock::now() + stall_every;
    while (std::chrono::steady_clock::now() < end) {
        if (stall.count() && std::chrono::steady_clock::now() >= next_stall) {
            // Host owner busy outside a source critical section; the device
            // worker keeps pulling and transferring.
            std::this_thread::sleep_for(stall);
            next_stall = std::chrono::steady_clock::now() + stall_every;
        }
        ServiceNativeAI();
        std::this_thread::sleep_for(200us);
    }
    ServiceNativeAI();
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
        for (auto& buffer : buffers) buffer.fill(0);
        AIRegisterDMACallback(Producer);
        AIInitDMA(reinterpret_cast<std::uintptr_t>(buffers[0].data()), sizeof(buffers[0]));
        BeginNativeAIObservations();
        AIStartDMA();
        // Repeated 9 ms owner delays: three DMA periods, within the allowance.
        Serve(800ms, 40ms, 9ms);
        auto delivery = GetNativeAIDeliveryStatus();
        const auto short_stalls = Contiguous();
        Check(short_stalls > 150, "timed fixture did not deliver enough authored PCM");
        Check(delivery.held_boundaries > 0, "owner delays never reached a completed DMA block");
        Check(!delivery.replayed_latches && !delivery.coalesced_causes,
              "late owner delivery replayed unprogrammed DMA registers");
        Check(!delivery.skipped_cells, "short owner delays dropped DMA clock time");
        Check(!delivery.sdl_short_pulls, "owner delays within the allowance starved the device");
        std::cout << "{\"phase\":\"short_stalls\",\"blocks\":" << short_stalls << ",\"held\":" << delivery.held_boundaries
                  << ",\"maximum_hold_ns\":" << delivery.maximum_hold_ns << ",\"pulls\":"
                  << GetNativeAIObservationStatus().sdl_pull_calls << "}\n";

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
        std::cout << "native AI late-delivery checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native AI delivery: " << error.what() << '\n';
        return 1;
    }
}
