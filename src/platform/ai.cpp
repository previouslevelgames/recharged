#include "platform/ai.h"
#include "platform/interrupts.h"

#include <dolphin/ai.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using mscharged::platform::NativeInterruptGuard;
using mscharged::platform::NativeInterruptRead;

struct AIState {
    std::mutex mutex;
    SDL_AudioStream* stream{};
    std::thread::id owner{};
    AIDCallback callback{};
    std::uintptr_t address{};
    u32 length{};
    u32 rate{};
    bool audio_reference{};
    bool initialized{};
    bool running{};
    bool pending{};
    bool callback_active{};
    bool output_started{};
    std::uint64_t output_start_cells{};
    mscharged::platform::NativeAIOutputStatus output{};
    std::uint64_t generation{};
    std::uint64_t submitted{};
    std::uint64_t consumed{};
    std::uint64_t cancelled{};
    std::uint64_t dispatched{};
    std::uint64_t retained{};
    std::uint64_t last_hash{};
    std::uintptr_t active_address{};
    u32 active_length{};
    u32 active_offset{};
    bool first_edge{};
    std::uint64_t clock_epoch{};
    std::uint64_t epoch_cells{};
    std::uint64_t transferred_cells{};
    std::uint64_t latch_edges{};
    std::uint64_t coalesced_edges{};
    std::uint64_t maximum_service_gap{};
    std::uint64_t last_service{};
    std::vector<u8> fifo;
    std::atomic_bool observing{};
    std::uint64_t observation_end_ns{};
    mscharged::platform::NativeAIObservationStatus observations{};
    // Delivery classification: registers rewritten since the last latch and
    // the DMA clock time of the currently pending hardware cause.
    bool next_programmed{};
    std::uint64_t cause_ns{};
    bool queue_observed{};
    // DMA waiting at a completed block until the owner takes the earlier cause.
    bool held{};
    std::uint64_t hold_start_ns{};
    mscharged::platform::NativeAIDeliveryStatus delivery{};
    char error[256]{};
};

AIState& State() { static AIState state; return state; }

struct Packet {
    AIState* state;
    std::uint64_t generation;
    int size;
    // malloc supplies the alignment required by native S16 PCM.
    u8* Data() { return reinterpret_cast<u8*>(this + 1); }
};

int Frequency(u32 rate) { return rate == AI_SAMPLERATE_32KHZ ? 32000 : 48000; }

// Host delivery allowance. Native AI causes reach the original callback only at
// owner safe points; the output keeps this much transferred PCM beyond one
// device pull and one DMA block so a short owner delay is not audible. It also
// bounds DMA catch-up after a longer host stall.
constexpr std::uint64_t kDeliveryAllowanceNs = 12000000;
std::uint64_t AllowanceCells(int frequency) {
    return (kDeliveryAllowanceNs * std::uint64_t(frequency) / 1000000000ull + 7) / 8;
}
std::runtime_error SDLError(const char* operation) {
    return std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

std::uint64_t Hash(const u8* bytes, std::size_t size) noexcept {
    std::uint64_t value = 14695981039346656037ull;
    for (std::size_t n = 0; n < size; ++n) value = (value ^ bytes[n]) * 1099511628211ull;
    return value;
}

void SDLCALL Completed(void* userdata, const void*, int) noexcept {
    auto* packet = static_cast<Packet*>(userdata);
    auto& state = *packet->state;
    {
        std::lock_guard lock(state.mutex);
        --state.retained;
        // SDL also releases buffers on clear/destroy; those are cancellations,
        // not consumed DMA blocks and cannot generate source interrupts.
        if (packet->generation == state.generation && state.running) {
            ++state.consumed;

        } else {
            ++state.cancelled;
        }
    }
    std::free(packet);
}

// AI DMA has its own nominal sample clock. SDL's device pull size does not
// select a source buffer, create a hardware interrupt or repeat source PCM.
// One hardware cell is32 bytes/eight stereo S16 frames. The source-selected
// current transfer is latched independently from AIInitDMA's next registers.
void Latch(AIState& state) {
    if (!state.address || !state.length)
        throw std::invalid_argument("AI next DMA transfer has no source buffer/count");
    state.active_address = state.address;
    state.active_length = state.length;
    state.active_offset = 0;
    state.next_programmed = false;
    state.fifo.resize(state.active_length);
}

// Nominal DMA clock time of the current transferred-cell position.
std::uint64_t ClockTime(const AIState& state) {
    const auto frequency = std::uint64_t(Frequency(state.rate));
    const auto frames = state.epoch_cells * 8;
    return state.clock_epoch + frames / frequency * 1000000000ull +
           frames % frequency * 1000000000ull / frequency;
}

void Edge(AIState& state, std::uint64_t cause_ns) {
    ++state.latch_edges;
    if (state.pending) {
        ++state.coalesced_edges;
        if (state.observing) ++state.delivery.coalesced_causes;
    } else {
        state.cause_ns = cause_ns;
    }
    state.pending = true; // AID is a latched hardware cause, not a callback queue.
}
void Edge(AIState& state) { Edge(state, ClockTime(state)); }

// Latch the next registers at a completed block and raise its hardware cause.
void LatchNext(AIState& state, std::uint64_t cause_ns) {
    if (state.observing) {
        ++state.delivery.latched_blocks;
        if (!state.next_programmed) ++state.delivery.replayed_latches;
    }
    Latch(state);
    Edge(state, cause_ns);
}

// Observation only: exact digital zero frames at the end of a source block.
void ClassifyBlock(AIState& state, const u8* bytes, std::size_t size) {
    const std::size_t frames = size / 4;
    std::size_t tail = 0;
    for (; tail < frames; ++tail) {
        std::uint32_t frame;
        std::memcpy(&frame, bytes + (frames - 1 - tail) * 4, sizeof(frame));
        if (frame) break;
    }
    if (tail == frames) ++state.delivery.silent_blocks;
    else if (tail) {
        ++state.delivery.zero_tail_blocks;
        state.delivery.zero_tail_frames += tail;
    }
}

bool Queue(AIState& state, SDL_AudioStream* stream, Packet* packet) noexcept {
    if (!SDL_PutAudioStreamDataNoCopy(stream, packet->Data(), packet->size, Completed, packet)) {
        std::lock_guard lock(state.mutex);
        --state.retained;
        --state.submitted;
        state.running = false;
        std::snprintf(state.error, sizeof(state.error), "AI DMA queue: %s", SDL_GetError());
        std::free(packet);
        return false;
    }
    return true;
}

void StartOutputIfReady(AIState& state, SDL_AudioStream* stream) noexcept {
    u32 dma_frames;
    {
        std::lock_guard lock(state.mutex);
        if (!state.running || state.output_started || state.owner != std::this_thread::get_id()) return;
        dma_frames = state.active_length / 4;
    }
    // SDL availability is in its current destination format. In particular,
    // a real postmix callback selects F32 even when the device reports S16.
    // Query it rather than assuming input bytes are immediately playable.
    SDL_AudioSpec input{}, output{}, device{};
    int device_frames = 0;
    const bool formats = SDL_GetAudioStreamFormat(stream, &input, &output) &&
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, &device_frames);
    const int available = formats ? SDL_GetAudioStreamAvailable(stream) : -1;
    if (!formats || available < 0 || input.freq <= 0 || output.freq <= 0 ||
        device.freq <= 0 || device_frames <= 0 || SDL_AUDIO_FRAMESIZE(output) <= 0) {
        std::lock_guard lock(state.mutex);
        state.running = false;
        std::snprintf(state.error, sizeof(state.error), "AI output lead query: %s", SDL_GetError());
        return;
    }
    // One actual device pull, one source-selected completed DMA period and the
    // owner delivery allowance. Ceiling conversion keeps the margin in output
    // frames at either source rate, including a resampled physical device.
    const auto dma_output_frames = (std::uint64_t(dma_frames) * output.freq + input.freq - 1) / input.freq;
    const auto device_output_frames = (std::uint64_t(device_frames) * output.freq + device.freq - 1) / device.freq;
    const auto allowance_output_frames = (kDeliveryAllowanceNs * std::uint64_t(output.freq) + 999999999ull) / 1000000000ull;
    const auto required = device_output_frames + dma_output_frames + allowance_output_frames;
    const auto ready = std::uint64_t(available / SDL_AUDIO_FRAMESIZE(output));
    {
        std::lock_guard lock(state.mutex);
        state.output.required_output_frames = required;
        if (ready < required) return;
        state.output.ready_output_frames_at_start = ready;
        state.output.transferred_input_frames_at_start = (state.transferred_cells - state.output_start_cells) * 8;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        std::lock_guard lock(state.mutex);
        state.running = false;
        std::snprintf(state.error, sizeof(state.error), "AI output device resume: %s", SDL_GetError());
        return;
    }
    std::lock_guard lock(state.mutex);
    state.output_started = true;
    state.output.device_start_ns = SDL_GetTicksNS();
}

std::size_t Transfer(AIState& state, SDL_AudioStream* stream, bool* blocked = nullptr) noexcept {
    // This same exclusion protects source DMA writes/stop/free. SDL workers
    // only try the lock; they never wait for source code while holding SDL's
    // stream lock. A source critical section defers this native safe point.
    NativeInterruptRead source_access;
    const auto now = SDL_GetTicksNS();
    // The owner's own masked section or IRQ context keeps hardware replay below.
    const bool unmasked = mscharged::platform::NativeInterruptsEnabled() &&
                          !mscharged::platform::NativeInterruptDispatchActive();
    std::vector<Packet*> packets;
    {
        std::lock_guard lock(state.mutex);
        if (!state.initialized || !state.running || state.callback_active) return 0;
        const auto elapsed = now - state.clock_epoch;
        const auto frames = (elapsed / 1000000000ull) * Frequency(state.rate) +
                            (elapsed % 1000000000ull) * Frequency(state.rate) / 1000000000ull;
        const auto due = frames / 8;
        // Latching the initial device cause does not read game memory and is
        // independent of the CPU mask. Source delivery remains masked below.
        if (state.first_edge && due) { Edge(state); state.first_edge = false; }
        if (!source_access) {
            if (blocked) *blocked = true;
            return 0;
        }
        state.maximum_service_gap = std::max(state.maximum_service_gap, now - state.last_service);
        state.last_service = now;
        // On the Wii the AID callback runs long before the next block ends. A
        // native cause waits for an owner safe point instead. While that owner
        // can take it, latch the next registers only after delivery; replaying
        // registers the source has not rewritten would repeat old PCM. A masked
        // owner, or no registered callback, keeps the hardware replay/latching.
        const bool deliverable = state.callback &&
            (state.owner != std::this_thread::get_id() || unmasked);
        try {
            if (state.held && !(state.pending && deliverable)) {
                // Bounded catch-up: the output lead or device silence already
                // covered a longer stall; more backlog would only add latency.
                const auto limit = AllowanceCells(Frequency(state.rate)) + state.active_length / 32;
                if (due > state.epoch_cells + limit) {
                    const auto skip = due - state.epoch_cells - limit;
                    state.epoch_cells += skip;
                    if (state.observing) state.delivery.skipped_cells += skip;
                }
                if (state.observing)
                    state.delivery.maximum_hold_ns = std::max(state.delivery.maximum_hold_ns, now - state.hold_start_ns);
                state.held = false;
                LatchNext(state, now);
            }
            while (!state.held && state.running && state.epoch_cells < due) {
                std::memcpy(state.fifo.data() + state.active_offset,
                            reinterpret_cast<const void*>(state.active_address + state.active_offset), 32);
                state.active_offset += 32;
                ++state.epoch_cells;
                ++state.transferred_cells;
                if (state.active_offset != state.active_length) continue;
                auto* packet = static_cast<Packet*>(std::malloc(sizeof(Packet) + state.fifo.size()));
                if (!packet) throw std::bad_alloc();
                packet->state = &state;
                packet->generation = state.generation;
                packet->size = static_cast<int>(state.fifo.size());
                std::memcpy(packet->Data(), state.fifo.data(), state.fifo.size());
                try { packets.push_back(packet); }
                catch (...) { std::free(packet); throw; }
                state.last_hash = Hash(packet->Data(), state.fifo.size());
                ++state.retained;
                ++state.submitted;
                if (state.observing) ClassifyBlock(state, packet->Data(), state.fifo.size());
                if (state.pending && deliverable) {
                    state.held = true;
                    state.hold_start_ns = now;
                    if (state.observing) ++state.delivery.held_boundaries;
                    break;
                }
                LatchNext(state, ClockTime(state));
            }
        } catch (const std::exception& error) {
            state.running = false;
            std::snprintf(state.error, sizeof(state.error), "AI hardware FIFO: %s", error.what());
        }
    }
    std::size_t queued = 0;
    for (auto* packet : packets) {
        const auto size = std::size_t(packet->size);
        if (Queue(state, stream, packet)) queued += size;
    }
    if (source_access) StartOutputIfReady(state, stream);
    return queued;
}

void SDLCALL Feed(void* userdata, SDL_AudioStream* stream, int additional, int total) noexcept {
    auto& state = *static_cast<AIState*>(userdata);
    if (state.observing.load(std::memory_order_relaxed)) {
        std::lock_guard lock(state.mutex);
        if (state.observing) {
            auto& observed = state.observations;
            const auto now = SDL_GetTicksNS();
            if (observed.last_sdl_pull_ns)
                observed.maximum_sdl_pull_gap_ns = std::max(observed.maximum_sdl_pull_gap_ns, now - observed.last_sdl_pull_ns);
            observed.last_sdl_pull_ns = now;
            ++observed.sdl_pull_calls;
            // SDL reports estimates in the unconverted INPUT byte domain.
            // Neither demand nor a short read is a source DMA completion.
            const auto extra = static_cast<std::uint64_t>(std::max(additional, 0));
            observed.total_additional_input_bytes_requested += extra;
            observed.maximum_additional_input_bytes_requested = std::max(observed.maximum_additional_input_bytes_requested, extra);
            observed.maximum_total_input_bytes_requested = std::max(observed.maximum_total_input_bytes_requested,
                static_cast<std::uint64_t>(std::max(total, 0)));
        }
    }
    // Device pull granularity can exceed the source's96-frame interval. Only
    // real elapsed DMA transfers enter SDL; demand never repeats a buffer.
    bool blocked = false;
    const auto queued = Transfer(state, stream, &blocked);
    if (state.observing.load(std::memory_order_relaxed)) {
        // SDL holds its recursive stream lock for this callback. Demand left
        // unsatisfied here becomes device silence; it is observed, not filled.
        const int remaining = SDL_GetAudioStreamQueued(stream);
        std::lock_guard lock(state.mutex);
        if (state.observing) {
            auto& delivery = state.delivery;
            if (blocked) ++delivery.sdl_pulls_without_source_access;
            const auto demand = static_cast<std::uint64_t>(std::max(additional, 0));
            if (queued < demand) {
                ++delivery.sdl_short_pulls;
                delivery.sdl_short_input_bytes += demand - queued;
            }
            if (remaining >= 0) {
                const auto depth = static_cast<std::uint64_t>(remaining);
                delivery.minimum_queued_input_bytes = state.queue_observed ?
                    std::min(delivery.minimum_queued_input_bytes, depth) : depth;
                delivery.maximum_queued_input_bytes = std::max(delivery.maximum_queued_input_bytes, depth);
                state.queue_observed = true;
            }
        }
    }
}

void RequireInitialized(AIState& state) {
    if (!state.initialized || !state.stream) throw std::logic_error("AI audio device is not initialized");
}

SDL_AudioStream* Stop(AIState& state) {
    SDL_AudioStream* stream;
    {
        std::lock_guard lock(state.mutex);
        state.running = false;
        state.pending = false;
        state.held = false;
        state.output_started = false;
        ++state.generation;
        stream = state.stream;
    }
    if (stream) {
        const bool paused = SDL_PauseAudioStreamDevice(stream);
        const bool cleared = SDL_ClearAudioStream(stream);
        if (!paused || !cleared) throw SDLError("AI audio DMA stop/drain failed");
    }
    return stream;
}
} // namespace

extern "C" AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    const auto previous = state.callback;
    state.callback = callback;
    return previous;
}

extern "C" void AIInit(u8*) {
    NativeInterruptGuard scope;
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (state.initialized) return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) throw SDLError("AI SDL audio initialization failed");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, Frequency(AI_SAMPLERATE_32KHZ)};
    auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, &state);
    if (!stream) {
        const auto error = SDLError("AI audio device creation failed");
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw error;
    }
    const int channel_map[2]{1, 0};
    if (!SDL_SetAudioStreamInputChannelMap(stream, channel_map, 2)) {
        const auto error = SDLError("AI hardware stereo channel mapping failed");
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw error;
    }
    std::lock_guard lock(state.mutex);
    state.stream = stream;
    state.owner = std::this_thread::get_id();
    state.audio_reference = true;
    state.initialized = true;
    state.running = false;
    state.pending = false;
    state.rate = AI_SAMPLERATE_32KHZ;
    state.callback = nullptr; // Original AIInit resets the callback on first init.
    state.error[0] = 0;
}

extern "C" BOOL AICheckInit() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.initialized;
}

extern "C" void AIInitDMA(std::uintptr_t address, u32 length) {
    NativeInterruptGuard scope;
    auto& state = State();
    bool start;
    {
        std::lock_guard lock(state.mutex);
        state.address = address & ~std::uintptr_t(31);
        const u32 count = (length / 32) & 0xffff;
        state.length = (count & 0x7fff) * 32;
        state.next_programmed = true;
        // The original SDK ORs all16 count bits into the DSP CSR. Bit15
        // therefore also sets PLAY for an oversized request; retain this
        // hardware quirk instead of treating it as a normal larger DMA.
        start = (count & 0x8000) && !state.running;
    }
    if (start) AIStartDMA();
}

extern "C" std::uintptr_t AIGetDMAStartAddr() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.address;
}

extern "C" u32 AIGetDMALength() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.length;
}

extern "C" void AIStartDMA() {
    NativeInterruptGuard scope;
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        RequireInitialized(state);
        if (state.running) return;
        if (!state.address || !state.length) throw std::invalid_argument("AI DMA needs a source buffer and nonzero hardware length");
        Latch(state);
        state.clock_epoch = state.last_service = SDL_GetTicksNS();
        state.output_started = false;
        state.output = {};
        state.output.dma_start_ns = state.clock_epoch;
        state.output_start_cells = state.transferred_cells;
        state.epoch_cells = 0;
        state.first_edge = true;
        state.error[0] = 0;
        state.pending = false;
        state.held = false;
        ++state.generation;
        state.running = true;
    }
}

extern "C" void AIStopDMA() {
    NativeInterruptGuard scope;
    Stop(State());
}

extern "C" BOOL AIGetDMAEnableFlag() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.running;
}

extern "C" u32 AIGetDMABytesLeft() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.running || !state.active_length) return 0;
    // The DSP exposes the zero-based remaining32-byte cell counter. This is
    // independent of SDL's conversion/queued-output latency.
    const u32 cells = (state.active_length - state.active_offset) / 32;
    return cells ? (cells - 1) * 32 : 0;
}

extern "C" u32 AIGetDSPSampleRate() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.rate;
}

extern "C" void AISetDSPSampleRate(u32 rate) {
    NativeInterruptGuard scope;
    auto& state = State();
    SDL_AudioStream* stream;
    const u32 selected = rate == AI_SAMPLERATE_32KHZ ? AI_SAMPLERATE_32KHZ : AI_SAMPLERATE_48KHZ;
    {
        std::lock_guard lock(state.mutex);
        if (state.rate == selected) return;
        stream = state.stream;
    }
    if (stream) {
        Transfer(state, stream);
        const SDL_AudioSpec input{SDL_AUDIO_S16, 2, Frequency(selected)};
        if (!SDL_SetAudioStreamFormat(stream, &input, nullptr)) throw SDLError("AI DSP rate selection failed");
    }
    std::lock_guard lock(state.mutex);
    state.rate = selected;
    state.clock_epoch = state.last_service = SDL_GetTicksNS();
    state.epoch_cells = 0;
}

extern "C" void AIReset() { mscharged::platform::ShutdownNativeAI(); }

namespace mscharged::platform {
bool ServiceNativeAI() {
    auto& state = State();
    SDL_AudioStream* stream;
    {
        std::lock_guard lock(state.mutex);
        if (!state.initialized) return false;
        if (state.owner != std::this_thread::get_id())
            throw std::logic_error("AI interrupts must be serviced on the initialization/game thread");
        stream = state.stream;
    }
    Transfer(state, stream);
    {
        std::lock_guard lock(state.mutex);
        if (!state.initialized) return false;
        if (state.owner != std::this_thread::get_id()) throw std::logic_error("AI interrupts must be serviced on the initialization/game thread");
        if (state.error[0]) throw std::runtime_error(state.error);
        if (!state.running || !state.pending || state.callback_active || !state.callback) return false;
    }
    bool invoked = false;
    // Dispatch itself changes native interrupt/context state and acquires the
    // shared exclusion. The trampoline has no source/game readiness behavior.
    struct Call {
        AIState& state;
        bool& invoked;
        static void Run(void* context) {
            auto& call = *static_cast<Call*>(context);
            AIDCallback callback;
            std::uint64_t observation_start = 0;
            {
                std::lock_guard lock(call.state.mutex);
                if (!call.state.running || !call.state.pending || call.state.callback_active || !call.state.callback) return;
                callback = call.state.callback;
                call.state.pending = false;
                call.state.callback_active = true;
                if (call.state.observing) {
                    auto& observed = call.state.observations;
                    observation_start = SDL_GetTicksNS();
                    auto& delivery = call.state.delivery;
                    const auto latency = observation_start > call.state.cause_ns ?
                        observation_start - call.state.cause_ns : 0;
                    const auto block_ns = std::uint64_t(call.state.active_length / 4) * 1000000000ull /
                        std::uint64_t(Frequency(call.state.rate));
                    ++delivery.dispatched_callbacks;
                    delivery.total_dispatch_latency_ns += latency;
                    delivery.maximum_dispatch_latency_ns = std::max(delivery.maximum_dispatch_latency_ns, latency);
                    if (block_ns && latency >= block_ns) ++delivery.callbacks_after_one_block;
                    if (block_ns && latency >= 2 * block_ns) ++delivery.callbacks_after_two_blocks;
                    if (block_ns && latency >= 4 * block_ns) ++delivery.callbacks_after_four_blocks;
                    if (observed.last_callback_start_ns)
                        observed.maximum_callback_start_gap_ns = std::max(observed.maximum_callback_start_gap_ns,
                            observation_start - observed.last_callback_start_ns);
                    if (!observed.first_callback_start_ns) observed.first_callback_start_ns = observation_start;
                    observed.last_callback_start_ns = observation_start;
                    ++observed.callbacks_started;
                }
            }
            try { callback(); }
            catch (...) {
                std::lock_guard lock(call.state.mutex);
                call.state.callback_active = false;
                if (observation_start) {
                    auto& observed = call.state.observations;
                    const auto duration = SDL_GetTicksNS() - observation_start;
                    ++observed.callbacks_failed;
                    observed.total_callback_duration_ns += duration;
                    observed.maximum_callback_duration_ns = std::max(observed.maximum_callback_duration_ns, duration);
                }
                throw;
            }
            std::lock_guard lock(call.state.mutex);
            call.state.callback_active = false;
            if (observation_start) {
                auto& observed = call.state.observations;
                const auto duration = SDL_GetTicksNS() - observation_start;
                ++observed.callbacks_completed;
                observed.total_callback_duration_ns += duration;
                observed.maximum_callback_duration_ns = std::max(observed.maximum_callback_duration_ns, duration);
            }
            ++call.state.dispatched;
            call.invoked = true;
        }
    } call{state, invoked};
    DispatchNativeInterrupt(Call::Run, &call);
    return invoked;
}

NativeAIStatus GetNativeAIStatus() {
    NativeInterruptGuard scope;
    auto& state = State();
    NativeAIStatus status;
    SDL_AudioStream* stream;
    {
        std::lock_guard lock(state.mutex);
        status.initialized = state.initialized;
        status.running = state.running;
        status.interrupt_pending = state.pending;
        status.callback_active = state.callback_active;
        status.source_address = state.address;
        status.dma_bytes = state.length;
        status.dsp_rate = state.rate;
        status.submitted_blocks = state.submitted;
        status.consumed_blocks = state.consumed;
        status.cancelled_blocks = state.cancelled;
        status.dispatched_callbacks = state.dispatched;
        status.last_input_hash = state.last_hash;
        status.retained_blocks = state.retained;
        stream = state.stream;
    }
    if (stream) {
        SDL_AudioSpec input{}, device{};
        if (!SDL_GetAudioStreamFormat(stream, &input, nullptr) ||
            !SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, &status.device_frames))
            throw SDLError("AI device format query failed");
        status.input_frequency = input.freq;
        status.device_frequency = device.freq;
        status.queued_input_bytes = SDL_GetAudioStreamQueued(stream);
        status.device_id = SDL_GetAudioStreamDevice(stream);
        int channels = 0;
        int* mapping = SDL_GetAudioStreamInputChannelMap(stream, &channels);
        status.maps_right_left_to_left_right = mapping && channels == 2 && mapping[0] == 1 && mapping[1] == 0;
        SDL_free(mapping);
    }
    return status;
}

NativeAIClockStatus GetNativeAIClockStatus() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return {state.active_address, state.active_length, state.active_offset,
            state.transferred_cells, state.latch_edges, state.coalesced_edges,
            state.maximum_service_gap, state.last_service - state.clock_epoch,
            state.epoch_cells};
}

void BeginNativeAIObservations() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireInitialized(state);
    if (state.owner != std::this_thread::get_id() || state.callback_active)
        throw std::logic_error("AI observations must start on the idle initialization/game owner");
    state.observations = {};
    state.observations.observation_start_ns = SDL_GetTicksNS();
    state.observation_end_ns = 0;
    state.delivery = {};
    state.queue_observed = false;
    state.observing = true;
}

void EndNativeAIObservations() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireInitialized(state);
    if (state.owner != std::this_thread::get_id() || state.callback_active)
        throw std::logic_error("AI observations must end on the idle initialization/game owner");
    if (state.observing) state.observation_end_ns = SDL_GetTicksNS();
    state.observing = false;
}

NativeAIObservationStatus GetNativeAIObservationStatus() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto observed = state.observations;
    if (observed.observation_start_ns) {
        const auto end = state.observing ? SDL_GetTicksNS() : state.observation_end_ns;
        observed.observed_elapsed_ns = end - observed.observation_start_ns;
    }
    return observed;
}

NativeAIOutputStatus GetNativeAIOutputStatus() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.output;
}

NativeAIDeliveryStatus GetNativeAIDeliveryStatus() {
    NativeInterruptGuard scope;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.delivery;
}

void ShutdownNativeAI() {
    NativeInterruptGuard scope;
    auto& state = State();
    SDL_AudioStream* stream;
    { std::lock_guard lock(state.mutex); stream = state.stream; }
    std::exception_ptr stop_error;
    try { Stop(state); }
    catch (...) { stop_error = std::current_exception(); }
    if (stream) {
        SDL_SetAudioStreamGetCallback(stream, nullptr, nullptr);
        // Pinned SDL_DestroyAudioStream does not release its custom channel
        // map. Unconfigure the map through the owning public API after drain.
        if (!SDL_SetAudioStreamInputChannelMap(stream, nullptr, 2) && !stop_error)
            stop_error = std::make_exception_ptr(SDLError("AI channel map release failed"));
        SDL_DestroyAudioStream(stream);
    }
    bool audio_reference;
    {
        std::lock_guard lock(state.mutex);
        audio_reference = state.audio_reference;
        state.audio_reference = false;
        if (state.observing) state.observation_end_ns = SDL_GetTicksNS();
        state.observing = false;
        state.stream = nullptr;
        state.initialized = false;
        state.callback = nullptr;
        state.address = 0;
        state.length = 0;
        state.rate = AI_SAMPLERATE_32KHZ;
        state.fifo.clear();
        state.active_address = 0;
        state.active_length = state.active_offset = 0;
        state.first_edge = false;
        state.error[0] = 0;
    }
    if (audio_reference) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (stop_error) std::rethrow_exception(stop_error);
}
} // namespace mscharged::platform
