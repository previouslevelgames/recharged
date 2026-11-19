#include "lfg.h"

#include <algorithm>
#include <cstring>

namespace mscharged::platform::wii_disc
{
void LaggedFibonacci::GenerateSeed(const std::uint8_t disc_id[4], std::uint8_t disc_num, std::uint32_t sector,
                                   std::uint32_t seed[kSeedWords])
{
    const std::uint32_t base = (std::uint32_t(disc_id[2]) << 24 | std::uint32_t(disc_id[1]) << 16
                                | std::uint32_t(std::uint8_t(disc_id[3] + disc_id[2])) << 8
                                | std::uint8_t(disc_id[0] + disc_id[1]))
        ^ disc_num;
    std::uint32_t n = base * 0x260BCD5u ^ sector * 0x1EF29123u;
    for (std::size_t word = 0; word < kSeedWords; ++word)
    {
        std::uint32_t value = 0;
        for (int bit = 0; bit < 32; ++bit)
        {
            n = n * 0x5D588B65u + 1;
            value = value >> 1 | (n & 0x80000000u);
        }
        seed[word] = value;
    }
    seed[16] ^= seed[0] >> 9 ^ seed[16] << 23;
}

void LaggedFibonacci::Init(const std::uint32_t seed[kSeedWords])
{
    std::uint32_t state[kWords];
    std::copy(seed, seed + kSeedWords, state);
    for (std::size_t i = kSeedWords; i < kWords; ++i)
        state[i] = state[i - kSeedWords] << 23 ^ state[i - kSeedWords + 1] >> 9 ^ state[i - 1];
    // The output's second byte is bits 18-25, not 16-23, of each word. As nod
    // does, fold that into the state (the steps below are XORs, so it commutes)
    // and keep the words as big-endian bytes: output is then a plain copy.
    for (std::size_t i = 0; i < kWords; ++i)
    {
        const std::uint32_t value = (state[i] & 0xFF00FFFFu) | ((state[i] >> 2) & 0x00FF0000u);
        const std::uint8_t bytes[4] = {std::uint8_t(value >> 24), std::uint8_t(value >> 16), std::uint8_t(value >> 8),
                                       std::uint8_t(value)};
        std::memcpy(&words_[i], bytes, 4);
    }
    position_ = 0;
    for (int step = 0; step < 4; ++step) Forward();
}

void LaggedFibonacci::Forward()
{
    for (std::size_t i = 0; i < kLag; ++i) words_[i] ^= words_[i + kWords - kLag];
    for (std::size_t i = kLag; i < kWords; ++i) words_[i] ^= words_[i - kLag];
}

void LaggedFibonacci::Skip(std::size_t bytes)
{
    position_ += bytes;
    while (position_ >= kBytes)
    {
        Forward();
        position_ -= kBytes;
    }
}

void LaggedFibonacci::Fill(std::uint8_t* out, std::size_t size)
{
    while (size)
    {
        if (position_ >= kBytes)
        {
            Forward();
            position_ -= kBytes;
        }
        const std::size_t count = std::min(size, kBytes - position_);
        std::memcpy(out, reinterpret_cast<const std::uint8_t*>(words_) + position_, count);
        position_ += count;
        out += count;
        size -= count;
    }
}
} // namespace mscharged::platform::wii_disc
