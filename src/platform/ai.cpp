#include "platform/ai.h"
#include "platform/interrupts.h"

#include <dolphin/ai.h>
#include <SDL3/SDL.h>

#include <algorithm>
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
    std::uint64_t generation{};
    std::uint64_t submitted{};
    std::uint64_t consumed{};
    std::uint64_t cancelled{};
    std::uint64_t dispatched{};
    std::uint64_t retained{};
    std::uint64_t last_hash{};
    std::vector<u8> cache;
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
            state.pending = true;
        } else {
            ++state.cancelled;
        }
    }
    std::free(packet);
}

// Called only by SDL while its stream lock is held. It never executes game
// callbacks or waits for a game critical section. A cached FIFO copy supplies
// the previous DMA block while the source handler is writing its next buffer.
int QueueBlock(AIState& state, SDL_AudioStream* stream) noexcept {
    Packet* packet = nullptr;
    {
        NativeInterruptRead source_access;
        std::lock_guard lock(state.mutex);
        if (!state.initialized || !state.running || !state.address || !state.length) return false;
        try {
            if (source_access) {
                state.cache.resize(state.length);
                std::memcpy(state.cache.data(), reinterpret_cast<const void*>(state.address), state.length);
            }
        } catch (...) {
            std::snprintf(state.error, sizeof(state.error), "AI DMA FIFO allocation failed");
            state.running = false;
            return false;
        }
        if (state.cache.empty()) return false;
        packet = static_cast<Packet*>(std::malloc(sizeof(Packet) + state.cache.size()));
        if (!packet) {
            std::snprintf(state.error, sizeof(state.error), "AI DMA packet allocation failed");
            state.running = false;
            return false;
        }
        packet->state = &state;
        packet->generation = state.generation;
        packet->size = static_cast<int>(state.cache.size());
        std::memcpy(packet->Data(), state.cache.data(), state.cache.size());
        state.last_hash = Hash(packet->Data(), state.cache.size());
        ++state.retained;
        ++state.submitted;
    }
    const int size = packet->size;
    if (!SDL_PutAudioStreamDataNoCopy(stream, packet->Data(), size, Completed, packet)) {
        std::lock_guard lock(state.mutex);
        --state.retained;
        --state.submitted;
        state.running = false;
        std::snprintf(state.error, sizeof(state.error), "AI DMA queue: %s", SDL_GetError());
        std::free(packet);
        return false;
    }
    return size;
}

void SDLCALL Feed(void* userdata, SDL_AudioStream* stream, int additional, int) noexcept {
    auto& state = *static_cast<AIState*>(userdata);
    while (additional > 0) {
        {
            std::lock_guard lock(state.mutex);
            if (!state.running) return;
        }
        const int size = QueueBlock(state, stream);
        if (!size) return;
        additional -= size;
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
    SDL_AudioStream* stream;
    {
        std::lock_guard lock(state.mutex);
        RequireInitialized(state);
        if (state.running) return;
        if (!state.address || !state.length) throw std::invalid_argument("AI DMA needs a source buffer and nonzero hardware length");
        state.cache.resize(state.length);
        std::memcpy(state.cache.data(), reinterpret_cast<const void*>(state.address), state.length);
        state.error[0] = 0;
        state.pending = false;
        ++state.generation;
        state.running = true;
        stream = state.stream;
    }
    if (!QueueBlock(state, stream) || !SDL_ResumeAudioStreamDevice(stream)) {
        const auto error = SDLError("AI audio DMA start failed");
        Stop(state);
        throw error;
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
    SDL_AudioStream* stream;
    u32 length;
    bool running;
    {
        std::lock_guard lock(state.mutex);
        stream = state.stream;
        length = state.length;
        running = state.running;
    }
    if (!stream || !running || !length) return 0;
    const int queued = SDL_GetAudioStreamQueued(stream);
    if (queued < 0) throw SDLError("AI DMA queue counter failed");
    // SDL consumes whole device pulls. This is the actual remaining input
    // queue's current DMA-block phase, expressed in hardware32-byte units.
    const u32 phase = static_cast<u32>(queued) % length;
    return (phase ? phase : (queued ? length : 0)) & ~u32(31);
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
        const SDL_AudioSpec input{SDL_AUDIO_S16, 2, Frequency(selected)};
        if (!SDL_SetAudioStreamFormat(stream, &input, nullptr)) throw SDLError("AI DSP rate selection failed");
    }
    std::lock_guard lock(state.mutex);
    state.rate = selected;
}

extern "C" void AIReset() { mscharged::platform::ShutdownNativeAI(); }

namespace mscharged::platform {
bool ServiceNativeAI() {
    auto& state = State();
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
            {
                std::lock_guard lock(call.state.mutex);
                if (!call.state.running || !call.state.pending || call.state.callback_active || !call.state.callback) return;
                callback = call.state.callback;
                call.state.pending = false;
                call.state.callback_active = true;
            }
            try { callback(); }
            catch (...) {
                std::lock_guard lock(call.state.mutex);
                call.state.callback_active = false;
                throw;
            }
            std::lock_guard lock(call.state.mutex);
            call.state.callback_active = false;
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
        state.stream = nullptr;
        state.initialized = false;
        state.callback = nullptr;
        state.address = 0;
        state.length = 0;
        state.rate = AI_SAMPLERATE_32KHZ;
        state.cache.clear();
        state.error[0] = 0;
    }
    if (audio_reference) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (stop_error) std::rethrow_exception(stop_error);
}
} // namespace mscharged::platform
