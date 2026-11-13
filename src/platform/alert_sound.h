#pragma once

#include <SDL3/SDL_audio.h>

#include <vector>

namespace mscharged::platform
{
// The chime for a challenge (two soft rising notes), made in code: mono float
// samples at `rate` Hz, peaking at `gain` (0..1), about 0.8 s.
std::vector<float> ChallengeChime(int rate, float gain);

// Plays the chime on the default playback device: for a challenge to the
// player, and for their challenge accepted. Opens SDL's audio (and the
// device) at the first Play, keeps them until destroyed; destroy it before
// SDL_Quit. A chime replaces one still playing. Without a device it stays
// silent.
class AlertSound
{
public:
    AlertSound() = default;
    ~AlertSound();
    AlertSound(const AlertSound&) = delete;
    AlertSound& operator=(const AlertSound&) = delete;

    // `gain` 0..1 (the master volume): 0 plays nothing.
    void Play(float gain);

private:
    SDL_AudioStream* stream_ = nullptr;
    bool opened_audio_ = false; // SDL's audio was started here
    bool failed_ = false;
    int rate_ = 48000;
};
}
