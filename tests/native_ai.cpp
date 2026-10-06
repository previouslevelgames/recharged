#include "platform/ai.h"
#include "platform/interrupts.h"
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <dolphin/os/OSContext.h>
#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
unsigned checks{};
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void Await(Predicate predicate, const char* message) {
    const auto limit = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= limit) throw std::runtime_error(message);
        std::this_thread::sleep_for(1ms);
    }
    ++checks;
}

struct Cleanup {
    ~Cleanup() { try { ShutdownNativeAI(); } catch (...) {} SDL_Quit(); }
};
std::thread::id owner;
OSContext* owner_context;
alignas(32) std::array<s16,256> first{};
alignas(32) std::array<s16,256> second{};
unsigned callback_count{}, predecessor_count{};
AIDCallback predecessor{};
bool request_throw{};

void BufferCallback() {
    Check(std::this_thread::get_id() == owner, "source callback ran on a worker thread");
    Check(!NativeInterruptsEnabled(), "interrupt entry did not mask interrupts");
    Check(OSGetCurrentContext() != owner_context, "interrupt context did not change");
    const BOOL previous = OSEnableInterrupts();
    Check(previous == FALSE, "callback enable returned incorrect prior mask");
    Check(!ServiceNativeAI(), "active source callback was reentered");
    AIInitDMA(reinterpret_cast<std::uintptr_t>((callback_count & 1) ? first.data() : second.data()),
              sizeof(first));
    OSRestoreInterrupts(previous);
    ++callback_count;
    if (request_throw) throw std::runtime_error("intentional callback failure");
}
void ChainedCallback() {
    ++predecessor_count;
    predecessor(); // An actual retained PCM-producing callback, not an AX substitute.
}

struct Capture {
    std::atomic<unsigned> nonzero_frames{};
    std::atomic<unsigned> settled_frames{};
    std::atomic<bool> incorrect{};
    std::atomic<unsigned> calls{};
    std::atomic<float> first_left{}, first_right{};
    std::atomic<float> bad_left{}, bad_right{};
    std::atomic<unsigned> bad_frames{};
};
void SDLCALL CaptureMix(void* context, const SDL_AudioSpec* spec, float* samples, int size) {
    auto& capture = *static_cast<Capture*>(context);
    ++capture.calls;
    if (spec->channels != 2 || spec->format != SDL_AUDIO_F32) {
        capture.incorrect = true;
        return;
    }
    for (int n = 0; n + 1 < size / int(sizeof(float)); n += 2) {
        const float left = samples[n], right = samples[n+1];
        if (std::abs(left) < 0.001f && std::abs(right) < 0.001f) continue;
        // Constant authored R/L S16 data becomes native L/R device floats.
        // The real device may resample; startup filter ringing scales both
        // channels. Check their independently authored polarity/ratio, then
        // require512 settled frames at the exact S16-to-float levels.
        if (left >= 0 || right <= 0 || std::abs(left*1234.0f + right*8765.0f) > 0.2f) {
            capture.incorrect = true;
            if(++capture.bad_frames == 1) { capture.bad_left=left; capture.bad_right=right; }
        }
        if (std::abs(left - (-8765.0f / 32768.0f)) <= 0.0002f &&
            std::abs(right - (1234.0f / 32768.0f)) <= 0.0002f) ++capture.settled_frames;
        if (++capture.nonzero_frames == 1) { capture.first_left=left; capture.first_right=right; }
    }
}

void InterruptContract() {
    Check(NativeInterruptsEnabled(), "initial host interrupt mask");
    const auto previous = OSDisableInterrupts();
    Check(previous == TRUE && !NativeInterruptsEnabled(), "disable previous state");
    Check(OSDisableInterrupts() == FALSE, "nested disable previous state");
    Check(OSRestoreInterrupts(FALSE) == FALSE && !NativeInterruptsEnabled(), "nested restore mask");
    Check(!DispatchNativeInterrupt(BufferCallback), "masked interrupt dispatched");
    std::atomic<bool> entered{}, attempted{};
    std::thread worker([&] {
        attempted = true;
        const auto old = OSDisableInterrupts();
        entered = true;
        OSRestoreInterrupts(old);
    });
    Await([&]{ return attempted.load(); }, "worker did not attempt critical section");
    std::this_thread::sleep_for(20ms);
    const bool excluded = !entered;
    const auto restored = OSRestoreInterrupts(previous);
    worker.join();
    Check(excluded && restored == FALSE && entered && NativeInterruptsEnabled(),
          "source critical section did not exclude other thread");
    Check(OSEnableInterrupts() == TRUE, "idempotent enable previous state");

    OSContext context;
    std::memset(&context, 0x5a, sizeof(context));
    OSClearContext(&context);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&context);
    for (std::size_t n=0; n<sizeof(context); ++n)
        Check(bytes[n] == (n>=OS_CONTEXT_MODE && n<OS_CONTEXT_MODE+4 ? 0 : 0x5a),
              "OSClearContext changed opaque fields beyond source mode/state");
    OSSetCurrentContext(&context);
    Check(OSGetCurrentContext() == &context, "native context pointer association");
    OSSetCurrentContext(owner_context);
}

void DeviceContract(const char* pcm_path, std::uint64_t expected_hash) {
    std::ifstream input(pcm_path, std::ios::binary);
    std::vector<unsigned char> raw(std::istreambuf_iterator<char>(input), {});
    Check(raw.size() == sizeof(first), "independent PCM fixture size");
    for (std::size_t n=0; n<first.size(); ++n) {
        const unsigned sample = raw[2*n] | (unsigned(raw[2*n+1])<<8);
        first[n] = static_cast<s16>(sample);
        second[n] = static_cast<s16>(-first[n]);
    }
    Check(reinterpret_cast<std::uintptr_t>(first.data()) > 0xffffffffull,
          "pointer fixture did not exercise native address width");
    AIInit(nullptr);
    Check(AICheckInit() && SDL_WasInit(SDL_INIT_AUDIO), "real SDK audio initialization");
    auto status = GetNativeAIStatus();
    Check(status.initialized && !status.running && status.input_frequency == 32000 &&
          status.device_frequency > 0 && status.device_frames > 0 && status.device_id &&
          status.maps_right_left_to_left_right, "actual stream/device format/channel mapping");
    Check(!ServiceNativeAI(), "idle device fabricated a DMA interrupt");
    Check(AIRegisterDMACallback(BufferCallback) == nullptr, "first callback previous value");
    AIInit(nullptr);
    Check(AIRegisterDMACallback(BufferCallback) == BufferCallback, "repeat init reset callback");
    AIInitDMA(reinterpret_cast<std::uintptr_t>(first.data())+7, sizeof(first)+15);
    Check(AIGetDMAStartAddr() == reinterpret_cast<std::uintptr_t>(first.data()) &&
          AIGetDMALength() == sizeof(first), "native pointer/32-byte hardware stride");
    AIStartDMA();
    status = GetNativeAIStatus();
    Check(status.running, "actual DMA did not start");
    Await([&]{ return GetNativeAIStatus().submitted_blocks > 0; }, "clocked DMA did not submit its source buffer");
    Check(GetNativeAIStatus().last_input_hash == expected_hash,
          "source-selected latched DMA bytes were not submitted unchanged");
    Await([&]{ return GetNativeAIStatus().consumed_blocks > 0; }, "dummy device did not consume real PCM");
    Check(callback_count == 0, "source callback ran before explicit game-thread service");
    const auto disabled = OSDisableInterrupts();
    Check(!ServiceNativeAI(), "masked latched AI interrupt dispatched");
    OSRestoreInterrupts(disabled);
    Check(ServiceNativeAI() && callback_count == 1 && NativeInterruptsEnabled() &&
          OSGetCurrentContext() == owner_context, "latched hardware edge source/context restoration");

    predecessor = AIRegisterDMACallback(ChainedCallback);
    Check(predecessor == BufferCallback, "previous source callback chaining");
    Await([&]{ return ServiceNativeAI(); }, "chained callback did not receive clocked DMA latch interrupt");
    Check(predecessor_count == 1 && callback_count == 2, "previous callback was not executed exactly once");
    std::atomic<bool> rejected{};
    std::thread foreign([&]{ try { ServiceNativeAI(); } catch(const std::logic_error&) { rejected=true; } });
    foreign.join();
    Check(rejected, "foreign thread was allowed to service game callback");

    request_throw = true;
    bool threw = false;
    Await([&]{
        try { ServiceNativeAI(); }
        catch(const std::runtime_error&) { threw=true; }
        return threw;
    }, "intentional callback failure did not propagate");
    request_throw = false;
    Check(!GetNativeAIStatus().callback_active && NativeInterruptsEnabled() &&
          OSGetCurrentContext() == owner_context, "throwing source callback poisoned interrupt state");

    for (const u32 selection : {0u,1u,7u,0u}) {
        AISetDSPSampleRate(selection);
        const auto expected = selection ? 1u : 0u;
        status = GetNativeAIStatus();
        Check(AIGetDSPSampleRate() == expected && status.input_frequency == (expected ? 48000 : 32000),
              "source DSP rate selection did not reach actual SDL input");
        Check((AIGetDMABytesLeft() & 31) == 0 && AIGetDMABytesLeft() <= AIGetDMALength(),
              "actual FIFO phase exceeded hardware counter bounds");
    }
    AIStopDMA();
    status = GetNativeAIStatus();
    Check(!status.running && !AIGetDMAEnableFlag() && !status.interrupt_pending &&
          status.retained_blocks == 0 && status.queued_input_bytes == 0 &&
          status.submitted_blocks == status.consumed_blocks + status.cancelled_blocks,
          "stop did not drain copied PCM ownership or classified cancellation as completion");
    const auto count = status.dispatched_callbacks;
    std::this_thread::sleep_for(30ms);
    Check(!ServiceNativeAI() && GetNativeAIStatus().dispatched_callbacks == count,
          "stopped audio fabricated a later callback");

    // Actual SDL postmix samples independently prove channel order; no source
    // callback is needed to play a constant DMA buffer.
    AIRegisterDMACallback(nullptr);
    for(std::size_t n=0; n<first.size(); n+=2) { first[n]=1234; first[n+1]=-8765; }
    AIInitDMA(reinterpret_cast<std::uintptr_t>(first.data()), sizeof(first));
    Capture capture;
    Check(SDL_SetAudioPostmixCallback(status.device_id, CaptureMix, &capture), "device sample recorder installation");
    AIStartDMA();
    Await([&]{ return capture.settled_frames >= 512; }, "device did not receive settled postmix PCM");
    AIStopDMA();
    Check(SDL_SetAudioPostmixCallback(status.device_id, nullptr, nullptr), "device sample recorder removal");
    if(capture.incorrect) std::cerr << "postmix first=" << capture.first_left << ',' << capture.first_right << " frames=" << capture.nonzero_frames << " bad=" << capture.bad_left << ',' << capture.bad_right << " count=" << capture.bad_frames << '\n';
    Check(!capture.incorrect && capture.calls > 0, "Wii right/left samples reached wrong device channels");

    // Preserve AIInitDMA's original DSP CSR count bit15/PLAY overlap.
    AIInitDMA(reinterpret_cast<std::uintptr_t>(first.data()), 0x100020);
    Check(AIGetDMAEnableFlag() && AIGetDMALength()==32, "original oversized DMA count PLAY quirk");
    AIStopDMA();
    struct alignas(32) DMASource { unsigned char bytes[512]; };
    auto* temporary = new DMASource;
    Check((reinterpret_cast<std::uintptr_t>(temporary)&31)==0, "temporary DMA source alignment");
    std::memset(temporary->bytes,0x46,sizeof(*temporary));
    AIInitDMA(reinterpret_cast<std::uintptr_t>(temporary),512);
    const auto consumed_before=GetNativeAIStatus().consumed_blocks;
    AIStartDMA();
    Await([&]{return GetNativeAIStatus().consumed_blocks>consumed_before;},
          "temporary source buffer did not reach real device");
    AIStopDMA();
    delete temporary;
    std::this_thread::sleep_for(20ms);
    Check(GetNativeAIStatus().retained_blocks==0 && !ServiceNativeAI(),
          "stopped worker retained or accessed released source memory");
    AIReset();
    Check(!AICheckInit() && !SDL_WasInit(SDL_INIT_AUDIO) && GetNativeAIStatus().retained_blocks==0,
          "native SDK reset retained device/buffers/subsystem reference");
}
} // namespace

int main(int argc, char** argv) {
    Cleanup cleanup;
    try {
        owner = std::this_thread::get_id();
        owner_context = OSGetCurrentContext();
        if (argc==2 && std::strcmp(argv[1], "--bad-driver")==0) {
            bool failed=false;
            try { AIInit(nullptr); } catch(const std::runtime_error&) { failed=true; }
            Check(failed && !AICheckInit() && !SDL_WasInit(SDL_INIT_AUDIO),
                  "device failure fabricated initialization or leaked audio reference");
            std::cout << "native AI failure checks=" << checks << '\n';
            return 0;
        }
        if(argc != 3) throw std::invalid_argument("native_ai_tests PCM_FILE FNV1A64");
        InterruptContract();
        DeviceContract(argv[1], std::stoull(argv[2]));
        std::cout << "native AI checks=" << checks << " device-consumption/mask/context/channel/owner/stop pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native AI failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
