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
#include <thread>

// Host speaker volume: the gain scales only the actual device output of the
// unchanged DMA PCM. Constant S16 source levels make the expected float level
// exact after resampling. The DMA buffer holds R,L pairs, so the device's
// left channel carries -8765/32768.
namespace {
using namespace mscharged::platform;
unsigned checks{};
alignas(32) std::array<s16, 192> buffer;
std::atomic<double> left_sum{0.0};
std::atomic<unsigned long long> frames{0};
std::atomic_bool capturing{false};

void Check(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }

void Source() { AIInitDMA(reinterpret_cast<std::uintptr_t>(buffer.data()), sizeof(buffer)); }

void SDLCALL Postmix(void*, const SDL_AudioSpec* spec, float* pcm, int size) {
    if (!capturing || spec->format != SDL_AUDIO_F32 || spec->channels != 2) return;
    double sum = 0.0;
    unsigned long long count = 0;
    for (int i = 0; i + 1 < size / int(sizeof(float)); i += 2) { sum += pcm[i]; ++count; }
    left_sum = left_sum + sum;
    frames += count;
}

// Average left level over a playback window after the gain change settled.
double Measure() {
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
    while (std::chrono::steady_clock::now() < settle) { ServiceNativeAI(); std::this_thread::sleep_for(std::chrono::microseconds(200)); }
    left_sum = 0.0; frames = 0; capturing = true;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < stop) { ServiceNativeAI(); std::this_thread::sleep_for(std::chrono::microseconds(200)); }
    capturing = false;
    Check(frames > 512, "no device output was captured");
    return left_sum / double(frames);
}

template <typename F> void Reject(F function, const char* text) {
    try { function(); } catch (const std::invalid_argument&) { ++checks; return; }
    throw std::runtime_error(text);
}
}

int main() {
    try {
        for (unsigned i = 0; i < 96; ++i) { buffer[2*i] = 1234; buffer[2*i+1] = -8765; }
        Reject([] { SetNativeAIOutputGain(std::nanf("")); }, "NaN gain accepted");
        Reject([] { SetNativeAIOutputGain(-0.01f); }, "negative gain accepted");
        Reject([] { SetNativeAIOutputGain(1.01f); }, "amplifying gain accepted");
        Check(GetNativeAIOutputGain() == 1.0f, "default host volume is not unity");

        SetNativeAIOutputGain(0.5f); // before the original AIInit creates the device stream
        AIInit(nullptr);
        Check(std::fabs(GetNativeAIOutputGain() - 0.5f) < 1e-6f, "pending volume was not applied to the new stream");
        AIRegisterDMACallback(Source);
        AIInitDMA(reinterpret_cast<std::uintptr_t>(buffer.data()), sizeof(buffer));
        const auto status = GetNativeAIStatus();
        Check(SDL_SetAudioPostmixCallback(status.device_id, Postmix, nullptr), "postmix recorder failed");
        AIStartDMA();
        const double expected = -8765.0 / 32768.0;
        const double half = Measure();
        Check(std::fabs(half - 0.5 * expected) < std::fabs(0.03 * expected), "50 % volume did not halve the device output");

        SetNativeAIOutputGain(1.0f); // live change while the source plays
        const double full = Measure();
        Check(std::fabs(full - expected) < std::fabs(0.03 * expected), "unity volume changed the device output");

        SetNativeAIOutputGain(0.0f);
        const double silent = Measure();
        Check(std::fabs(silent) < 1e-6, "zero volume did not mute the device output");

        AIStopDMA();
        Check(SDL_SetAudioPostmixCallback(status.device_id, nullptr, nullptr), "postmix recorder retirement failed");
        ShutdownNativeAI();
        Check(GetNativeAIOutputGain() == 0.0f, "host volume was not kept for the next stream");
        SetNativeAIOutputGain(1.0f);
        std::printf("AI host volume: %u checks, half %.5f, full %.5f, muted %.5f (expected full %.5f).\n",
                    checks, half, full, silent, expected);
        SDL_Quit();
        return 0;
    } catch (const std::exception& e) {
        try { ShutdownNativeAI(); } catch (...) {}
        SDL_Quit();
        std::fprintf(stderr, "AI host volume: %s\n", e.what());
        return 1;
    }
}
