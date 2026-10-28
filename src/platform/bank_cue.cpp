#include "platform/bank_cue.h"

#include "platform/disc.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace mscharged::platform {
namespace {
using Bytes = std::vector<std::uint8_t>;

std::runtime_error Bad(const std::string& what) {
    return std::runtime_error("The sound bank " + what + ".");
}

std::uint32_t U32(const Bytes& d, std::size_t at) {
    if (at > d.size() || d.size() - at < 4) throw Bad("ends inside a record");
    return std::uint32_t(d[at]) << 24 | std::uint32_t(d[at + 1]) << 16 | std::uint32_t(d[at + 2]) << 8 | d[at + 3];
}
std::uint16_t U16(const Bytes& d, std::size_t at) {
    if (at > d.size() || d.size() - at < 2) throw Bad("ends inside a record");
    return std::uint16_t(d[at] << 8 | d[at + 1]);
}
float F32(const Bytes& d, std::size_t at) {
    const std::uint32_t bits = U32(d, at);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// An nlChunk: a u32 id and size (big-endian), then its data. Bits 24-27 of
// the id align the data (1 << n), the size counts from the header's end, and
// the next chunk starts on the following 4-byte boundary.
struct Chunk {
    std::uint32_t id; // without the alignment bits
    std::size_t data, end;
};
std::vector<Chunk> Children(const Bytes& d, std::size_t begin, std::size_t end) {
    std::vector<Chunk> chunks;
    for (std::size_t at = begin; at < end;) {
        if (end - at < 8) throw Bad("ends inside a chunk header");
        const std::uint32_t id = U32(d, at), size = U32(d, at + 4);
        const std::size_t payload = at + 8, alignment = std::size_t(1) << (id >> 24 & 0xF);
        const std::size_t data = (payload + alignment - 1) & ~(alignment - 1);
        if (size > end - payload || data > payload + size) throw Bad("has a chunk that leaves its parent");
        chunks.push_back({id & 0x80FFFFFFu, data, payload + size});
        at = (payload + size + 3) & ~std::size_t(3);
    }
    return chunks;
}
void Need(const Chunk& chunk, std::size_t bytes) {
    if (chunk.end - chunk.data < bytes) throw Bad("has a table shorter than its count");
}
// A record of the bundle as the game stored its address: its index in the
// array the bundle's header places at `origin`.
std::uint32_t Index(std::uint32_t address, std::uint32_t origin, std::uint32_t stride, std::uint32_t count) {
    const std::uint32_t delta = address - origin;
    if (delta % stride || delta / stride >= count) throw Bad("points outside its records");
    return delta / stride;
}
} // namespace

BankCue ReadBankCue(const Bytes& d, const Bytes& waves, std::uint32_t cue) {
    const auto root = Children(d, 0, d.size());
    if (root.empty() || root[0].id != 0x80000001u) throw Bad("has no root chunk");
    const auto top = Children(d, root[0].data, root[0].end);
    const Chunk *map = nullptr, *bundle = nullptr, *table = nullptr, *sources = nullptr;
    for (const auto& chunk : top) {
        if (chunk.id == 0x80023000u) map = &chunk;
        else if (chunk.id == 0x80023300u || chunk.id == 0x23701u) bundle = &chunk;
        else if (chunk.id == 0x23703u) table = &chunk;
        else if (chunk.id == 0x80023200u) sources = &chunk;
    }
    if (!map || !bundle || !table || !sources) throw Bad("lacks a part of a sound bank");

    // SoundMap: the key (cue, 0, 0, 0) holds the cue's index.
    const auto keys = Children(d, map->data, map->end);
    if (keys.size() < 2) throw Bad("has no sound map");
    const std::uint32_t keyCount = U32(d, keys[0].data);
    Need(keys[1], 20 * std::size_t(keyCount));
    std::uint32_t index = ~0u;
    for (std::uint32_t n = 0; n < keyCount && index == ~0u; ++n) {
        const std::size_t key = keys[1].data + 20 * std::size_t(n);
        if (U32(d, key) == cue && !U32(d, key + 4) && !U32(d, key + 8) && !U32(d, key + 12)) index = U32(d, key + 16);
    }
    if (index == ~0u) throw Bad("has no such cue");

    // AudioResourceBundle: its header, the arrays of cues (40 bytes), voices
    // (44), sequences (12) and sound events (48), two more parts, then each
    // cue's voices, each voice's sequences and RPC groups, each sequence's
    // events and each sound event's choices, in that order.
    const auto parts = Children(d, bundle->data, bundle->end);
    if (parts.size() < 7) throw Bad("has a short resource bundle");
    Need(parts[0], 56);
    const std::uint32_t cues = U32(d, parts[0].data + 8), voices = U32(d, parts[0].data + 16);
    const std::uint32_t sequences = U32(d, parts[0].data + 24), sounds = U32(d, parts[0].data + 32);
    Need(parts[1], 40 * std::size_t(cues));
    Need(parts[2], 44 * std::size_t(voices));
    Need(parts[3], 12 * std::size_t(sequences));
    Need(parts[4], 48 * std::size_t(sounds));
    if (index >= cues) throw Bad("maps the cue to no cue record");
    const auto list = [&](std::size_t n) -> const Chunk& {
        if (7 + n >= parts.size()) throw Bad("runs out of lists");
        return parts[7 + n];
    };
    const std::size_t cueRecord = parts[1].data + 40 * std::size_t(index);
    if (U32(d, cueRecord + 4) != 1) throw Bad("cue plays more than one voice");
    const Chunk& cueVoices = list(index);
    Need(cueVoices, 20);
    const auto voice = Index(U32(d, cueVoices.data), U32(d, parts[0].data + 20), 44, voices);
    const std::size_t voiceRecord = parts[2].data + 44 * std::size_t(voice);
    if (U32(d, voiceRecord + 16) != 1) throw Bad("voice plays more than one sequence");
    const Chunk& voiceSequences = list(cues + 2 * std::size_t(voice));
    Need(voiceSequences, 4);
    const auto sequence = Index(U32(d, voiceSequences.data), U32(d, parts[0].data + 28), 12, sequences);
    const std::size_t sequenceRecord = parts[3].data + 12 * std::size_t(sequence);
    if (U32(d, sequenceRecord + 4) != 1) throw Bad("sequence has more than one event");
    const Chunk& events = list(cues + 2 * std::size_t(voices) + sequence);
    Need(events, 8);
    if (U32(d, events.data) != 1) throw Bad("sequence plays no sound event");
    const auto sound = Index(U32(d, events.data + 4), U32(d, parts[0].data + 36), 48, sounds);
    const std::uint32_t choiceCount = U32(d, parts[4].data + 48 * std::size_t(sound) + 8);
    const Chunk& choices = list(cues + 2 * std::size_t(voices) + sequences + sound);
    Need(choices, 8 * std::size_t(choiceCount));

    // AudioSourceInfo: each choice's entry of the SP table. The table is a
    // count, entries of 28 bytes (type, rate, loop, loop end, end, current
    // nibble), then one ADPCM record of 46 bytes for each entry of type 0
    // (one-shot) or 1 (looped), in their order.
    const auto sourceParts = Children(d, sources->data, sources->end);
    if (sourceParts.size() < 2) throw Bad("has no source table");
    Need(sourceParts[0], 9);
    const std::uint32_t sourceCount = U32(d, sourceParts[0].data);
    if (d[sourceParts[0].data + 8]) throw Bad("is streamed");
    Need(sourceParts[1], 28 * std::size_t(sourceCount));
    const std::uint32_t entryCount = U32(d, table->data);
    const std::size_t entries = table->data + 4, records = entries + 28 * std::size_t(entryCount);
    if (records > table->end) throw Bad("has a short SP table");

    BankCue result;
    result.volume_db = F32(d, voiceRecord + 4) + F32(d, sequenceRecord);
    const float pitch = F32(d, voiceRecord + 8); // semitones
    for (std::uint32_t n = 0; n < choiceCount; ++n) {
        const std::uint32_t source = U32(d, choices.data + 8 * std::size_t(n));
        if (source >= sourceCount) throw Bad("chooses no source");
        const std::uint32_t entryIndex = U32(d, sourceParts[1].data + 28 * std::size_t(source));
        if (entryIndex >= entryCount) throw Bad("has a source outside the SP table");
        std::size_t record = records;
        for (std::uint32_t k = 0; k < entryIndex; ++k)
            if (U32(d, entries + 28 * std::size_t(k)) <= 1) record += 46;
        const std::size_t entry = entries + 28 * std::size_t(entryIndex);
        const std::uint32_t type = U32(d, entry);
        if (type > 1) throw Bad("chooses a sound that is not DSP-ADPCM");
        if (record + 46 > table->end) throw Bad("has a short SP table");
        DspAdpcm state;
        for (int k = 0; k < 16; ++k) state.coefficients[k] = std::int16_t(U16(d, record + 2 * k));
        state.header = U16(d, record + 34);
        state.history1 = std::int16_t(U16(d, record + 36));
        state.history2 = std::int16_t(U16(d, record + 38));
        // SPPrepareSound plays a looped sound to its loop end.
        const std::uint32_t end = type == 0 ? U32(d, entry + 16) : U32(d, entry + 12);
        BankCue::Wave wave;
        wave.samples = DecodeDspAdpcm(waves, U32(d, entry + 20), end, state);
        wave.rate = int(std::lround(double(U32(d, entry + 4)) * std::exp2(double(pitch) / 12.0)));
        wave.weight = U32(d, choices.data + 8 * std::size_t(n) + 4);
        if (wave.rate <= 0 || wave.rate > 192000) throw Bad("has a sound at an impossible rate");
        result.waves.push_back(std::move(wave));
    }
    if (result.waves.empty()) throw Bad("cue chooses no wave");
    return result;
}

std::vector<std::int16_t> DecodeDspAdpcm(const Bytes& data, std::uint32_t current, std::uint32_t end,
                                         const DspAdpcm& state) {
    if ((current & 15) < 2 || (end & 15) < 2 || end < current || (end >> 1) >= data.size())
        throw std::runtime_error("The sound's ADPCM addresses lie outside its wave bank.");
    std::vector<std::int16_t> pcm;
    pcm.reserve((end - current) / 16 * 14 + 14);
    int header = state.header & 0x7F;
    std::int64_t history1 = state.history1, history2 = state.history2;
    for (std::uint32_t address = current;;) {
        const std::uint8_t byte = data[address >> 1];
        int nibble = address & 1 ? byte & 0xF : byte >> 4;
        if (nibble >= 8) nibble -= 16;
        const int pair = header >> 4 & 7;
        const std::int64_t sum = std::int64_t(nibble) * (std::int64_t(1) << (header & 0xF)) * 2048 + 1024
            + state.coefficients[2 * pair] * history1 + state.coefficients[2 * pair + 1] * history2;
        const auto sample = std::clamp<std::int64_t>(sum >> 11, -32768, 32767);
        pcm.push_back(std::int16_t(sample));
        history2 = history1;
        history1 = sample;
        if (address == end) break;
        if ((++address & 15) == 0) {
            header = data[address >> 1] & 0x7F;
            address += 2;
        }
    }
    return pcm;
}

struct BankCuePlayer::State {
    std::mutex mutex;
    BankCue cue;
    std::minstd_rand random{std::uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    SDL_AudioStream* stream = nullptr;
};

BankCuePlayer::BankCuePlayer() : state_(std::make_shared<State>()) {}

// The stream stays open until the process ends (the host leaves through
// TerminateProcessNow): closing it here could come after SDL's own shutdown.
BankCuePlayer::~BankCuePlayer() = default;

void BankCuePlayer::Load(const std::filesystem::path& disc, const std::string& bank, std::uint32_t cue) {
    // The wave bank is a few MB of compressed disc: read away from the game's
    // loop. The thread owns its share of the state, so it may outlive the player.
    std::thread([state = state_, disc, bank, cue] {
        try {
            const auto resources = ReadDiscFile(disc, "audio/" + bank + ".resbun", 1u << 20);
            const auto waves = ReadDiscFile(disc, "audio/" + bank + ".nlxwb", 16u << 20);
            auto decoded = ReadBankCue(resources, waves, cue);
            std::lock_guard lock(state->mutex);
            state->cue = std::move(decoded);
        } catch (const std::exception&) {
            // No sound then; the autopilot works without it.
        }
    }).detach();
}

void BankCuePlayer::Play(float gain) {
    std::lock_guard lock(state_->mutex);
    const auto& waves = state_->cue.waves;
    std::uint64_t total = 0;
    for (const auto& wave : waves) total += wave.weight;
    if (!total || gain <= 0.0f) return;
    std::uint64_t pick = std::uniform_int_distribution<std::uint64_t>(0, total - 1)(state_->random);
    const BankCue::Wave* wave = &waves.front();
    for (const auto& candidate : waves) {
        if (pick < candidate.weight) {
            wave = &candidate;
            break;
        }
        pick -= candidate.weight;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, wave->rate};
    if (!state_->stream) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
        state_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!state_->stream) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return;
        }
    }
    // The game plays it centred: -3 dB on each of the two speakers, where SDL
    // gives a mono stream to both at full level.
    const float level = gain * std::pow(10.0f, state_->cue.volume_db / 20.0f) * 0.70710678f;
    SDL_ClearAudioStream(state_->stream);
    SDL_SetAudioStreamFormat(state_->stream, &spec, nullptr);
    SDL_SetAudioStreamGain(state_->stream, level);
    SDL_PutAudioStreamData(state_->stream, wave->samples.data(), int(wave->samples.size() * sizeof(std::int16_t)));
    SDL_FlushAudioStream(state_->stream);
    SDL_ResumeAudioStreamDevice(state_->stream);
}
} // namespace mscharged::platform
