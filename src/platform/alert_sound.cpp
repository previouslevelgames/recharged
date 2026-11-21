#include "platform/alert_sound.h"

#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mscharged::platform
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

// One note of a soft bell: its tone with two quieter overtones, a quick
// attack and a long fade.
void AddNote(std::vector<float>& samples, int rate, double at, double frequency, double level)
{
    const std::size_t first = std::size_t(at * rate);
    const double attack = 0.006, decay = 0.17;
    for (std::size_t n = first; n < samples.size(); ++n)
    {
        const double t = double(n - first) / rate;
        const double envelope = std::min(1.0, t / attack) * std::exp(-t / decay);
        if (envelope < 1e-4 && t > attack) break;
        const double phase = 2.0 * kPi * frequency * t;
        samples[n] += float(level * envelope
                            * (std::sin(phase) + 0.30 * std::sin(2.0 * phase) + 0.08 * std::sin(3.0 * phase)));
    }
}
}

std::vector<float> ChallengeChime(int rate, float gain)
{
    std::vector<float> samples(std::size_t(rate * 0.8), 0.0f);
    if (rate <= 0 || gain <= 0.0f) return samples;
    AddNote(samples, rate, 0.00, 783.99, 1.0);  // G5
    AddNote(samples, rate, 0.13, 1046.50, 1.0); // C6
    float peak = 0.0f;
    for (const float sample : samples) peak = std::max(peak, std::fabs(sample));
    // At most a third of full scale: a cue, not an alarm.
    const float scale = peak > 0.0f ? 0.33f * std::min(gain, 1.0f) / peak : 0.0f;
    for (float& sample : samples) sample *= scale;
    // The fade ends at silence.
    const std::size_t tail = std::min<std::size_t>(samples.size(), std::size_t(rate * 0.02));
    for (std::size_t n = 0; n < tail; ++n) samples[samples.size() - 1 - n] *= float(n) / float(tail);
    return samples;
}

AlertSound::~AlertSound()
{
    if (stream_) SDL_DestroyAudioStream(stream_);
    if (opened_audio_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AlertSound::Play(float gain)
{
    if (gain <= 0.0f || failed_) return;
    if (!stream_)
    {
        if (!SDL_WasInit(SDL_INIT_AUDIO))
        {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            {
                std::fprintf(stderr, "Challenge sound: no audio (%s)\n", SDL_GetError());
                failed_ = true;
                return;
            }
            opened_audio_ = true;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, rate_};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream_)
        {
            std::fprintf(stderr, "Challenge sound: no playback device (%s)\n", SDL_GetError());
            failed_ = true;
            return;
        }
        SDL_ResumeAudioStreamDevice(stream_);
    }
    const auto samples = ChallengeChime(rate_, gain);
    SDL_ClearAudioStream(stream_);
    SDL_PutAudioStreamData(stream_, samples.data(), int(samples.size() * sizeof(float)));
}
}
