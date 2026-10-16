#include "platform/sha1.h"

#include <algorithm>
#include <cstring>

namespace mscharged::platform
{
namespace
{
inline std::uint32_t RotateLeft(std::uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }
} // namespace

Sha1::Sha1() : state_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0} {}

void Sha1::Block(const std::uint8_t* block)
{
    std::uint32_t w[80];
    for (int n = 0; n < 16; ++n)
        w[n] = std::uint32_t(block[n * 4]) << 24 | std::uint32_t(block[n * 4 + 1]) << 16
            | std::uint32_t(block[n * 4 + 2]) << 8 | std::uint32_t(block[n * 4 + 3]);
    for (int n = 16; n < 80; ++n) w[n] = RotateLeft(w[n - 3] ^ w[n - 8] ^ w[n - 14] ^ w[n - 16], 1);
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
    for (int n = 0; n < 80; ++n)
    {
        std::uint32_t f, k;
        if (n < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        }
        else if (n < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        }
        else if (n < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        const std::uint32_t t = RotateLeft(a, 5) + f + e + k + w[n];
        e = d;
        d = c;
        c = RotateLeft(b, 30);
        b = a;
        a = t;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::Update(const void* data, std::size_t size)
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

std::string Sha1::FinishHex()
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
    hex.reserve(40);
    for (const std::uint32_t word : state_)
        for (int shift = 28; shift >= 0; shift -= 4) hex += digits[(word >> shift) & 0xF];
    return hex;
}

std::string Sha1Hex(const void* data, std::size_t size)
{
    Sha1 hash;
    hash.Update(data, size);
    return hash.FinishHex();
}

std::string Sha1Hex(std::string_view data) { return Sha1Hex(data.data(), data.size()); }
} // namespace mscharged::platform
