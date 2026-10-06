#include "platform/ai.h"
#include "platform/interrupts.h"
#include <dolphin/ai.h>
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace mscharged::platform;
unsigned checks{}, callbacks{};
bool intentional_failure{}, active_reset_rejected{};
std::thread::id owner;
alignas(32) std::array<s16, 192> buffer;
std::chrono::steady_clock::time_point playback_start;
std::atomic_uint64_t captured_frames{}, silent_frames{}, invalid_frames{}, captured_pulls{};
void Check(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
void Source() {
    Check(std::this_thread::get_id() == owner && !NativeInterruptsEnabled(), "observations changed actual callback context");
    AIInitDMA(reinterpret_cast<std::uintptr_t>(buffer.data()), sizeof(buffer));
    ++callbacks;
    if (!active_reset_rejected) {
        try { BeginNativeAIObservations(); }
        catch (const std::logic_error&) { active_reset_rejected = true; }
        SDL_Delay(1); // Independently known real callback body duration.
    }
    if (intentional_failure) throw std::runtime_error("independent original-callback exception");
}
void SDLCALL Postmix(void*, const SDL_AudioSpec* spec, float* pcm, int size) {
    if (std::chrono::steady_clock::now() - playback_start < std::chrono::milliseconds(150)) return;
    ++captured_pulls;
    if (spec->format != SDL_AUDIO_F32 || spec->channels != 2) { ++invalid_frames; return; }
    for (int i = 0; i + 1 < size / int(sizeof(float)); i += 2) {
        ++captured_frames;
        if (pcm[i] == 0.0f && pcm[i+1] == 0.0f) { ++silent_frames; continue; }
        // Native decoded S16 R,L data must reach physical float L,R. Exact
        // constant levels test resampling/conversion without a THP source.
        if (pcm[i] >= 0.0f || pcm[i+1] <= 0.0f ||
            std::abs(pcm[i] * 1234.0f + pcm[i+1] * 8765.0f) > 0.2f) ++invalid_frames;
    }
}
}

int main() {
    using namespace mscharged::platform;
    static_assert(sizeof(NativeAIObservationStatus) == 128);
    try {
        owner = std::this_thread::get_id();
        bool not_initialized = false;
        try { BeginNativeAIObservations(); }
        catch (const std::logic_error&) { not_initialized = true; }
        Check(not_initialized, "observations invented an initialized audio device");
        Check(GetNativeAIObservationStatus().observation_start_ns == 0, "observations defaulted on");
        for (unsigned i = 0; i < 96; ++i) { buffer[2*i] = 1234; buffer[2*i+1] = -8765; }
        AIInit(nullptr);
        Check(AIRegisterDMACallback(Source) == nullptr, "observation manufactured an AX predecessor");
        AIInitDMA(reinterpret_cast<std::uintptr_t>(buffer.data()), sizeof(buffer));
        BeginNativeAIObservations();
        Check(AIGetDMAStartAddr() == reinterpret_cast<std::uintptr_t>(buffer.data()) &&
              AIGetDMALength() == sizeof(buffer) && !AIGetDMAEnableFlag(), "begin changed source-selected DMA registers");
        auto status = GetNativeAIStatus();
        const std::string driver = SDL_GetCurrentAudioDriver();
        playback_start = std::chrono::steady_clock::now();
        Check(SDL_SetAudioPostmixCallback(status.device_id, Postmix, nullptr), "actual SDL postmix recorder failed");
        AIStartDMA();
        const auto stop = playback_start + std::chrono::milliseconds(700);
        while (std::chrono::steady_clock::now() < stop) {
            ServiceNativeAI();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        ServiceNativeAI();
        const auto observed = GetNativeAIObservationStatus();
        status = GetNativeAIStatus();
        const auto clock = GetNativeAIClockStatus();
        Check(observed.callbacks_started == callbacks && observed.callbacks_completed == callbacks && !observed.callbacks_failed,
              "observation count differs from genuine source callback entries");
        Check(active_reset_rejected, "source callback reset its active observation window");
        Check(observed.first_callback_start_ns >= observed.observation_start_ns &&
              observed.last_callback_start_ns > observed.first_callback_start_ns &&
              observed.maximum_callback_start_gap_ns > 0, "callback-only timestamps did not capture actual elapsed starts");
        Check(observed.maximum_callback_duration_ns >= 800000 &&
              observed.total_callback_duration_ns >= observed.maximum_callback_duration_ns,
              "independent 1 ms callback duration was not measured");
        // Already-transferred queued PCM can cover every actual device pull:
        // zero ADDITIONAL input demand is valid, with nonzero TOTAL requests.
        Check(observed.sdl_pull_calls && observed.maximum_total_input_bytes_requested &&
              observed.maximum_additional_input_bytes_requested<=observed.maximum_total_input_bytes_requested,
              "actual SDL input-domain total/differential demand was not recorded");
        Check(observed.observed_elapsed_ns >= 650000000, "observation elapsed time differs from the actual window");
        Check(captured_frames > 512 && !invalid_frames, "postmix channel/format/resampled constant PCM changed");

        bool foreign_rejected = false;
        std::thread foreign([&] { try { BeginNativeAIObservations(); } catch (const std::logic_error&) { foreign_rejected = true; } });
        foreign.join();
        Check(foreign_rejected, "foreign observer reset the source owner window");
        const auto before = callbacks;
        BeginNativeAIObservations();
        Check(callbacks == before && AIGetDMAEnableFlag() && AIGetDMAStartAddr() == reinterpret_cast<std::uintptr_t>(buffer.data()),
              "observation reset changed playback/source callback flow");
        intentional_failure = true;
        bool source_failure = false;
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!source_failure && std::chrono::steady_clock::now() < limit) {
            try { ServiceNativeAI(); } catch (const std::runtime_error&) { source_failure = true; }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        intentional_failure = false;
        const auto failed = GetNativeAIObservationStatus();
        Check(source_failure && failed.callbacks_started == 1 && failed.callbacks_failed == 1 && !failed.callbacks_completed,
              "observation hid/changed a source callback exception");
        Check(!GetNativeAIStatus().callback_active && NativeInterruptsEnabled(), "observed source failure poisoned actual IRQ lifetime");
        EndNativeAIObservations();
        const auto ended = GetNativeAIObservationStatus();
        SDL_Delay(5);
        ServiceNativeAI();
        const auto after = GetNativeAIObservationStatus();
        Check(after.observed_elapsed_ns == ended.observed_elapsed_ns && after.callbacks_started == ended.callbacks_started,
              "ended observations changed with genuine subsequent playback");
        AIStopDMA();
        Check(SDL_SetAudioPostmixCallback(status.device_id, nullptr, nullptr), "postmix recorder retirement failed");
        ShutdownNativeAI();
        Check(!GetNativeAIStatus().retained_blocks && !GetNativeAIStatus().initialized, "observation changed stop/drain ownership");
        std::printf("{\"checks\":%u,\"callbacks\":%llu,\"observation_ns\":%llu,\"callback_max_gap_ns\":%llu,"
            "\"callback_max_duration_ns\":%llu,\"device_rate\":%d,\"device_frames\":%d,\"input_rate\":%d,"
            "\"sdl_pull_calls\":%llu,\"maximum_total_input_bytes_requested\":%llu,\"maximum_additional_input_bytes_requested\":%llu,"
            "\"dma_coalesced\":%llu,\"dma_max_service_gap_ns\":%llu,\"postmix_frames_after150ms\":%llu,"
            "\"zero_postmix_frames_after150ms\":%llu,\"postmix_pulls_after150ms\":%llu,\"driver\":\"%s\"}\n",
            checks, (unsigned long long)observed.callbacks_started, (unsigned long long)observed.observed_elapsed_ns,
            (unsigned long long)observed.maximum_callback_start_gap_ns, (unsigned long long)observed.maximum_callback_duration_ns,
            status.device_frequency, status.device_frames, status.input_frequency,
            (unsigned long long)observed.sdl_pull_calls, (unsigned long long)observed.maximum_total_input_bytes_requested,
            (unsigned long long)observed.maximum_additional_input_bytes_requested,
            (unsigned long long)clock.coalesced_edges, (unsigned long long)clock.maximum_service_gap_ns,
            (unsigned long long)captured_frames.load(), (unsigned long long)silent_frames.load(),
            (unsigned long long)captured_pulls.load(), driver.c_str());
        SDL_Quit(); return 0;
    } catch (const std::exception& e) {
        try { ShutdownNativeAI(); } catch (...) {}
        SDL_Quit(); std::fprintf(stderr,"AI observations: %s\n",e.what()); return 1;
    }
}
