// The challenge chime: its length, level and quiet ends, and that playing it
// without a real audio device is harmless.
#include "platform/alert_sound.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
int failures = 0;

void Check(bool ok, const char* what)
{
    if (ok) return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
}
}

int main()
{
    using mscharged::platform::ChallengeChime;
    const auto chime = ChallengeChime(48000, 1.0f);
    Check(chime.size() == 38400, "0.8 s at 48 kHz");
    float peak = 0.0f;
    for (const float sample : chime) peak = std::max(peak, std::fabs(sample));
    Check(std::fabs(peak - 0.33f) < 0.001f, "peaks at a third of full scale");
    Check(std::fabs(chime.front()) < 0.01f && chime.back() == 0.0f, "starts and ends quietly");
    // The second note starts 0.13 s in: louder there than just before.
    float before = 0.0f, after = 0.0f;
    for (std::size_t n = 5800; n < 6240; ++n) before = std::max(before, std::fabs(chime[n]));
    for (std::size_t n = 6400; n < 6900; ++n) after = std::max(after, std::fabs(chime[n]));
    Check(after > before, "two notes");

    const auto half = ChallengeChime(48000, 0.5f);
    float half_peak = 0.0f;
    for (const float sample : half) half_peak = std::max(half_peak, std::fabs(sample));
    Check(std::fabs(half_peak - 0.165f) < 0.001f, "the gain scales it");
    const auto silent = ChallengeChime(48000, 0.0f);
    Check(std::all_of(silent.begin(), silent.end(), [](float s) { return s == 0.0f; }), "no gain, no sound");

    // On SDL's dummy driver: plays, plays again over itself, cleans up.
    {
        mscharged::platform::AlertSound sound;
        sound.Play(1.0f);
        sound.Play(0.5f);
        sound.Play(0.0f);
    }
    Check(!SDL_WasInit(SDL_INIT_AUDIO), "audio closed again");
    if (failures) return EXIT_FAILURE;
    std::puts("alert_sound: all checks passed");
    return EXIT_SUCCESS;
}
