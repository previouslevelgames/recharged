#pragma once

#include <cstddef>
#include <cstdint>

namespace mscharged::platform::wii_disc
{
// The lagged Fibonacci generator of GameCube and Wii junk data, which RVZ
// stores as seeds. After nod's util/lfg.rs (MIT, Luke Street), which follows
// Dolphin's CC0 description of WIA and RVZ.
class LaggedFibonacci
{
public:
    static constexpr std::size_t kSeedWords = 17;

    // The seed of the junk in the 0x8000 bytes at `sector` of a disc or partition.
    static void GenerateSeed(const std::uint8_t disc_id[4], std::uint8_t disc_num, std::uint32_t sector,
                             std::uint32_t seed[kSeedWords]);

    // Starts the stream from a seed's words (RVZ stores them big-endian).
    void Init(const std::uint32_t seed[kSeedWords]);
    void Skip(std::size_t bytes);
    void Fill(std::uint8_t* out, std::size_t size);

private:
    static constexpr std::size_t kWords = 521, kLag = 32, kBytes = kWords * 4;

    void Forward();

    // The output words, each stored as its big-endian bytes: Fill copies them.
    std::uint32_t words_[kWords];
    std::size_t position_ = 0;
};
} // namespace mscharged::platform::wii_disc
