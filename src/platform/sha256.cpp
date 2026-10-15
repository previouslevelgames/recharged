#include "platform/sha256.h"

#include "platform/path.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace mscharged::platform
{
namespace
{
constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t Rotate(std::uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }
} // namespace

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
{
}

void Sha256::Block(const std::uint8_t* block)
{
    std::uint32_t w[64];
    for (int n = 0; n < 16; ++n)
        w[n] = std::uint32_t(block[n * 4]) << 24 | std::uint32_t(block[n * 4 + 1]) << 16
            | std::uint32_t(block[n * 4 + 2]) << 8 | std::uint32_t(block[n * 4 + 3]);
    for (int n = 16; n < 64; ++n)
    {
        const std::uint32_t s0 = Rotate(w[n - 15], 7) ^ Rotate(w[n - 15], 18) ^ (w[n - 15] >> 3);
        const std::uint32_t s1 = Rotate(w[n - 2], 17) ^ Rotate(w[n - 2], 19) ^ (w[n - 2] >> 10);
        w[n] = w[n - 16] + s0 + w[n - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int n = 0; n < 64; ++n)
    {
        const std::uint32_t t1 = h + (Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25)) + ((e & f) ^ (~e & g)) + kRound[n] + w[n];
        const std::uint32_t t2 = (Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    length_ += size;
    while (size)
    {
        if (buffered_ == 0 && size >= 64)
        {
            Block(bytes);
            bytes += 64;
            size -= 64;
            continue;
        }
        const std::size_t take = std::min(size, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        size -= take;
        if (buffered_ == buffer_.size())
        {
            Block(buffer_.data());
            buffered_ = 0;
        }
    }
}

std::string Sha256::FinishHex()
{
    const std::uint64_t bits = length_ * 8;
    const std::uint8_t pad = 0x80;
    Update(&pad, 1);
    const std::uint8_t zero = 0;
    while (buffered_ != 56) Update(&zero, 1);
    std::uint8_t length[8];
    for (int n = 0; n < 8; ++n) length[n] = std::uint8_t(bits >> (56 - n * 8));
    Update(length, 8);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const std::uint32_t word : state_)
        for (int shift = 28; shift >= 0; shift -= 4) hex += digits[(word >> shift) & 0xF];
    return hex;
}

std::string Sha256Hex(std::string_view data)
{
    Sha256 hash;
    hash.Update(data.data(), data.size());
    return hash.FinishHex();
}

std::string Sha256File(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + PathUtf8(file));
    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (stream)
    {
        stream.read(buffer.data(), std::streamsize(buffer.size()));
        if (stream.gcount() > 0) hash.Update(buffer.data(), std::size_t(stream.gcount()));
    }
    if (stream.bad()) throw std::runtime_error("Cannot read " + PathUtf8(file));
    return hash.FinishHex();
}
} // namespace mscharged::platform
