#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// One sound of the game's own sound banks, decoded and played by the host,
// outside the game's audio engine: the test builds' autopilot greets the
// player with the title screen's unlock-all sound, whose bank the game
// unloads for matches.
namespace mscharged::platform {
// The waves a cue picks from each time it plays (its sound event's choices,
// by weight), decoded to mono PCM.
struct BankCue {
    struct Wave {
        std::vector<std::int16_t> samples;
        int rate = 0; // samples per second, the voice's pitch included
        unsigned weight = 0;
    };
    std::vector<Wave> waves;
    float volume_db = 0.0f; // the voice's volume and its sequence's offset
};

// Cue `cue` of a bank, from its resources (<bank>.resbun) and its waves
// (<bank>.nlxwb): SoundMap -> cue -> voice -> sequence -> sound event ->
// AudioSourceInfo -> SP table entry and its ADPCM record. Only a cue of one
// voice, one sequence and one sound event is read (the kind of the menus'
// one-shot sounds); the event's random pitch and volume are not applied.
// Throws std::runtime_error for other cues and for data that do not hold
// together.
BankCue ReadBankCue(const std::vector<std::uint8_t>& resources, const std::vector<std::uint8_t>& waves,
                    std::uint32_t cue);

// Nintendo's DSP-ADPCM as the AX voices decode it: nibble addresses from
// `current` to `end` inclusive, a new header byte (coefficient pair << 4 |
// scale) at every 16th nibble, each sample from the pair and the two before.
struct DspAdpcm {
    std::int16_t coefficients[16]{};
    std::uint16_t header = 0; // the first frame's pair and scale
    std::int16_t history1 = 0, history2 = 0;
};
std::vector<std::int16_t> DecodeDspAdpcm(const std::vector<std::uint8_t>& data, std::uint32_t current,
                                         std::uint32_t end, const DspAdpcm& state);

// Plays a cue of a bank of the disc on an audio stream of its own on the
// default playback device, which SDL mixes with the game's output. Load reads
// and decodes it in the background; Play starts one of its waves, picked by
// weight as the game picks them, and does nothing while the cue is not decoded
// (or could not be: nothing is reported).
class BankCuePlayer {
public:
    BankCuePlayer();
    ~BankCuePlayer();
    void Load(const std::filesystem::path& disc, const std::string& bank, std::uint32_t cue);
    void Play(float gain);

private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace mscharged::platform
