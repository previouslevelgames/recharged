// A cue of the game's sound banks decoded by the host (src/platform/bank_cue.h):
// the DSP-ADPCM arithmetic, a small bank laid out as the game's, data that do
// not hold together, and with a disc (argument), the title screen's unlock-all
// cue, whose four waves must match what the game itself played (decoded
// independently and compared with the game's recorded output).
#include "platform/bank_cue.h"
#include "platform/disc.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mscharged::platform;
using Bytes = std::vector<std::uint8_t>;
unsigned checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template <class F> void Reject(F f, const char* message) {
    bool failed{};
    try { f(); } catch (const std::runtime_error&) { failed = true; }
    Check(failed, message);
}

void Word(Bytes& b, std::uint32_t v) {
    for (int n = 3; n >= 0; --n) b.push_back(std::uint8_t(v >> (8 * n)));
}
void Half(Bytes& b, std::uint16_t v) {
    b.push_back(std::uint8_t(v >> 8));
    b.push_back(std::uint8_t(v));
}
Bytes Words(std::initializer_list<std::uint32_t> words) {
    Bytes b;
    for (const auto w : words) Word(b, w);
    return b;
}
// An nlChunk with its data right after the header (no alignment bits).
Bytes Chunk(std::uint32_t id, const Bytes& body) {
    Bytes b;
    Word(b, id);
    Word(b, std::uint32_t(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    while (b.size() % 4) b.push_back(0);
    return b;
}
Bytes Join(std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const auto& part : parts) b.insert(b.end(), part.begin(), part.end());
    return b;
}

constexpr std::uint32_t kCue = 0xCF37DAC7u;

// A bank with one cue: one voice, one sequence, one sound event choosing
// source 0 (weight 100) or 1 (weight 300). Source 0 is SP entry 2 (wave B),
// source 1 entry 0 (wave A); entry 1 is no ADPCM and has no record, so B's
// record is the second. `voices` is the cue's voice count.
Bytes Resources(std::uint32_t voices = 1) {
    const std::uint32_t cueAt = 0x1000, voiceAt = 0x2000, sequenceAt = 0x3000, soundAt = 0x4000;
    const auto map = Chunk(0x80023000u, Join({Chunk(0x23001, Words({2, 0, 0})),
                                              Chunk(0x23003, Words({0x11111111, 0, 0, 0, 5, kCue, 0, 0, 0, 0}))}));
    Bytes cue = Words({0x1234, voices});
    cue.resize(40);
    Bytes voice = Words({0x5678, 0, 0, 0, 1, 0, 0});
    voice.resize(44);
    Bytes sound = Words({0, 1, 2});
    sound.resize(48);
    const auto bundle = Chunk(0x80023300u, Join({
        Chunk(0x23301, Words({0, 0, 1, cueAt, 1, voiceAt, 1, sequenceAt, 1, soundAt, 0, 0, 0, 0})),
        Chunk(0x23302, cue), Chunk(0x23303, voice), Chunk(0x23304, Words({0, 1, 0})), Chunk(0x23305, sound),
        Chunk(0x23306, {}), Chunk(0x23307, {}),
        Chunk(0x23308, Words({voiceAt, 0, 0x3F800000, 0, 0})), // the cue's voice, weight 1.0
        Chunk(0x23309, Words({sequenceAt})), Chunk(0x2330C, {}), // the voice's sequence, no RPC
        Chunk(0x2330A, Words({1, soundAt})),                    // the sequence's sound event
        Chunk(0x2330B, Words({0, 100, 1, 300})),               // the event's choices
    }));
    Bytes table = Words({3,
                         0, 32000, 0, 0, 15, 2, 0,     // A: bytes 0-7
                         5, 0, 0, 0, 0, 0, 0,          // not ADPCM
                         0, 22050, 0, 0, 31, 18, 0});  // B: bytes 8-15
    for (const std::uint16_t header : {0x00, 0x01}) {
        Half(table, 2048); // pair 0: the last sample once
        for (int n = 1; n < 16; ++n) Half(table, 0);
        Half(table, 0);      // gain
        Half(table, header); // the first frame's pair and scale
        for (int n = 0; n < 5; ++n) Half(table, 0);
    }
    const auto sources = Chunk(0x80023200u, Join({Chunk(0x23201, Words({2, 0, 0})),
                                                  Chunk(0x23202, Words({2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}))}));
    return Chunk(0x80000001u, Join({map, bundle, Chunk(0x23703, table), sources}));
}

// Wave A: a frame of scale 1, each nibble 1. Wave B: scale 2, each nibble -1.
Bytes Waves() {
    return {0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

void RunDecoder() {
    DspAdpcm state;
    state.coefficients[0] = 2048; // pair 0 adds the last sample
    const auto waves = Waves();
    auto pcm = DecodeDspAdpcm(waves, 2, 31, state);
    Check(pcm.size() == 28, "Two frames do not give 28 samples");
    for (int n = 0; n < 14; ++n) Check(pcm[n] == n + 1, "Scale 1, nibble 1: not one more each sample");
    for (int n = 0; n < 14; ++n) Check(pcm[14 + n] == 14 - 2 * (n + 1), "Scale 2, nibble -1: not two less each sample");
    // From inside a frame, and the second frame's header read at its start.
    pcm = DecodeDspAdpcm(waves, 9, 18, state);
    Check(pcm.size() == 8 && pcm[6] == 7 && pcm[7] == 5, "Nibble addresses across a frame boundary");
    // Saturation: the largest nibbles at the largest scale, on a large history
    // (sums that leave 32 bits).
    const Bytes loud{0x0F, 0x77, 0x88};
    pcm = DecodeDspAdpcm(loud, 2, 5, DspAdpcm{{4096}, 0x0F, 30000, 0});
    Check(pcm.size() == 4 && pcm[0] == 32767 && pcm[1] == 32767 && pcm[2] == -32768 && pcm[3] == -32768,
          "A loud frame does not clamp at the 16-bit limits");
    Reject([&] { DecodeDspAdpcm(waves, 1, 15, state); }, "A header nibble was decoded as a sample");
    Reject([&] { DecodeDspAdpcm(waves, 2, 33, state); }, "A sound past the wave bank was decoded");
    Reject([&] { DecodeDspAdpcm(waves, 18, 3, state); }, "A sound ending before its start was decoded");
}

void RunBank() {
    const auto resources = Resources(), waves = Waves();
    const auto cue = ReadBankCue(resources, waves, kCue);
    Check(cue.waves.size() == 2 && cue.volume_db == 0.0f, "The cue's choices");
    const auto& b = cue.waves[0];
    const auto& a = cue.waves[1];
    Check(b.rate == 22050 && b.weight == 100 && a.rate == 32000 && a.weight == 300,
          "Choices not read as their sources' entries and weights");
    Check(a.samples.size() == 14 && a.samples.front() == 1 && a.samples.back() == 14, "Wave A");
    Check(b.samples.size() == 14 && b.samples.front() == -2 && b.samples.back() == -28,
          "Wave B (the second ADPCM record: entry 1 has none)");

    Reject([&] { ReadBankCue(resources, waves, 0x12345678); }, "A cue the map does not have was read");
    Reject([&] { ReadBankCue(Resources(2), waves, kCue); }, "A cue of two voices was read");
    for (const std::size_t cut : {std::size_t(7), resources.size() / 2, resources.size() - 6})
        Reject([&] { ReadBankCue(Bytes(resources.begin(), resources.begin() + std::ptrdiff_t(cut)), waves, kCue); },
               "A cut bank was read");
    Reject([&] { ReadBankCue(resources, Bytes(waves.begin(), waves.begin() + 12), kCue); },
           "A cut wave bank was read");
    auto wrong = resources;
    wrong[wrong.size() - 4 * 7 * 2] = 9; // source 0 points past the SP table
    Reject([&] { ReadBankCue(wrong, waves, kCue); }, "A source outside the SP table was read");
}

std::uint32_t Crc32(const std::vector<std::int16_t>& pcm) {
    std::uint32_t crc = ~0u;
    for (const std::int16_t sample : pcm)
        for (const std::uint8_t byte : {std::uint8_t(sample), std::uint8_t(std::uint16_t(sample) >> 8)}) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    return ~crc;
}

// The title screen's unlock-all cue on a disc: the four waves the game picks
// from (SP entries 99, 100, 97 and 98, a quarter each).
void RunDisc(const std::string& disc) {
    const auto cue = ReadBankCue(mscharged::ReadDiscFile(disc, "audio/FE_GEN_Sfx.resbun", 1u << 20),
                                 mscharged::ReadDiscFile(disc, "audio/FE_GEN_Sfx.nlxwb", 16u << 20), kCue);
    const struct { std::size_t samples; std::uint32_t crc; } expected[] = {
        {89148, 0x57d27ec6u}, {43468, 0x51d8ba70u}, {15905, 0x9eddf43au}, {25058, 0xca4e9c45u}};
    Check(cue.waves.size() == 4 && cue.volume_db == 0.0f, "The unlock-all cue does not choose from four waves");
    for (std::size_t n = 0; n < 4; ++n)
        Check(cue.waves[n].rate == 32000 && cue.waves[n].weight == 255 && cue.waves[n].samples.size() == expected[n].samples
                  && Crc32(cue.waves[n].samples) == expected[n].crc,
              "An unlock-all wave differs from the game's");
}
} // namespace

int main(int argc, char** argv) {
    try {
        RunDecoder();
        RunBank();
        if (argc > 1) RunDisc(argv[1]);
        std::cout << "bank_cue: " << checks << " checks passed" << (argc > 1 ? " (with the disc)" : "") << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bank_cue: " << e.what() << '\n';
        return 1;
    }
}
