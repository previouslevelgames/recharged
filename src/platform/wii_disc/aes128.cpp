#include "aes128.h"

#include <cstring>

#if defined(__aarch64__) && (defined(__ARM_FEATURE_AES) || defined(__ARM_FEATURE_CRYPTO))
#include <arm_neon.h>
#define WII_DISC_ARM_AES 1
#endif

namespace mscharged::platform::wii_disc
{
namespace
{
// GF(2^8) with the AES polynomial x^8 + x^4 + x^3 + x + 1.
std::uint8_t Times2(std::uint8_t value) { return std::uint8_t(value << 1) ^ ((value & 0x80) ? 0x1b : 0); }

std::uint8_t Multiply(std::uint8_t a, std::uint8_t b)
{
    std::uint8_t product = 0;
    for (; b; b >>= 1, a = Times2(a))
        if (b & 1) product ^= a;
    return product;
}

std::uint8_t RotateLeft8(std::uint8_t value, int bits) { return std::uint8_t(value << bits | value >> (8 - bits)); }
std::uint32_t RotateRight32(std::uint32_t value, int bits)
{
    return bits ? value >> bits | value << (32 - bits) : value;
}

struct Tables
{
    std::uint8_t sbox[256];
    std::uint8_t inv_sbox[256];
    // InvSubBytes followed by InvMixColumns for one byte of each row.
    std::uint32_t td[4][256];
};

Tables BuildTables()
{
    Tables tables{};
    // The S-box is the multiplicative inverse (generator 3 walks the field)
    // followed by the FIPS-197 affine transform.
    std::uint8_t power[255], log[256]{};
    std::uint8_t x = 1;
    for (int i = 0; i < 255; ++i)
    {
        power[i] = x;
        log[x] = std::uint8_t(i);
        x ^= Times2(x);
    }
    for (int value = 0; value < 256; ++value)
    {
        const std::uint8_t inverse = value ? power[(255 - log[value]) % 255] : 0;
        const std::uint8_t s = inverse ^ RotateLeft8(inverse, 1) ^ RotateLeft8(inverse, 2) ^ RotateLeft8(inverse, 3)
            ^ RotateLeft8(inverse, 4) ^ 0x63;
        tables.sbox[value] = s;
        tables.inv_sbox[s] = std::uint8_t(value);
    }
    for (int value = 0; value < 256; ++value)
    {
        const std::uint8_t y = tables.inv_sbox[value];
        const std::uint32_t word = std::uint32_t(Multiply(0x0e, y)) << 24 | std::uint32_t(Multiply(0x09, y)) << 16
            | std::uint32_t(Multiply(0x0d, y)) << 8 | Multiply(0x0b, y);
        for (int row = 0; row < 4; ++row) tables.td[row][value] = RotateRight32(word, 8 * row);
    }
    return tables;
}

const Tables& GetTables()
{
    static const Tables tables = BuildTables();
    return tables;
}

std::uint32_t Load32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

void Store32(std::uint8_t* p, std::uint32_t value)
{
    p[0] = std::uint8_t(value >> 24);
    p[1] = std::uint8_t(value >> 16);
    p[2] = std::uint8_t(value >> 8);
    p[3] = std::uint8_t(value);
}

std::uint32_t InvMixColumn(std::uint32_t column)
{
    const std::uint8_t a0 = std::uint8_t(column >> 24), a1 = std::uint8_t(column >> 16), a2 = std::uint8_t(column >> 8),
                       a3 = std::uint8_t(column);
    const auto row = [](std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, std::uint8_t b3) {
        return std::uint32_t(Multiply(0x0e, b0) ^ Multiply(0x0b, b1) ^ Multiply(0x0d, b2) ^ Multiply(0x09, b3));
    };
    return row(a0, a1, a2, a3) << 24 | row(a1, a2, a3, a0) << 16 | row(a2, a3, a0, a1) << 8 | row(a3, a0, a1, a2);
}

#ifndef WII_DISC_ARM_AES
void DecryptBlock(const Tables& t, const std::uint32_t* rk, const std::uint8_t* in, std::uint8_t* out)
{
    std::uint32_t s0 = Load32(in) ^ rk[0], s1 = Load32(in + 4) ^ rk[1], s2 = Load32(in + 8) ^ rk[2],
                  s3 = Load32(in + 12) ^ rk[3];
    // InvShiftRows moves row r of column c to column c + r: each output column
    // takes row 1 from the column to its left, and so on.
    for (int round = 1; round < 10; ++round)
    {
        rk += 4;
        const std::uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][(s3 >> 16) & 0xff] ^ t.td[2][(s2 >> 8) & 0xff]
            ^ t.td[3][s1 & 0xff] ^ rk[0];
        const std::uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][(s0 >> 16) & 0xff] ^ t.td[2][(s3 >> 8) & 0xff]
            ^ t.td[3][s2 & 0xff] ^ rk[1];
        const std::uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][(s1 >> 16) & 0xff] ^ t.td[2][(s0 >> 8) & 0xff]
            ^ t.td[3][s3 & 0xff] ^ rk[2];
        const std::uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][(s2 >> 16) & 0xff] ^ t.td[2][(s1 >> 8) & 0xff]
            ^ t.td[3][s0 & 0xff] ^ rk[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }
    rk += 4;
    const std::uint8_t* is = t.inv_sbox;
    const auto last = [is](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        return std::uint32_t(is[a >> 24]) << 24 | std::uint32_t(is[(b >> 16) & 0xff]) << 16
            | std::uint32_t(is[(c >> 8) & 0xff]) << 8 | is[d & 0xff];
    };
    Store32(out, last(s0, s3, s2, s1) ^ rk[0]);
    Store32(out + 4, last(s1, s0, s3, s2) ^ rk[1]);
    Store32(out + 8, last(s2, s1, s0, s3) ^ rk[2]);
    Store32(out + 12, last(s3, s2, s1, s0) ^ rk[3]);
}
#endif
} // namespace

Aes128Decryptor::Aes128Decryptor(const std::uint8_t key[16])
{
    const Tables& t = GetTables();
    std::uint32_t expanded[44];
    for (int i = 0; i < 4; ++i) expanded[i] = Load32(key + 4 * i);
    std::uint8_t rcon = 1;
    for (int i = 4; i < 44; ++i)
    {
        std::uint32_t word = expanded[i - 1];
        if (i % 4 == 0)
        {
            word = word << 8 | word >> 24;
            word = std::uint32_t(t.sbox[word >> 24]) << 24 | std::uint32_t(t.sbox[(word >> 16) & 0xff]) << 16
                | std::uint32_t(t.sbox[(word >> 8) & 0xff]) << 8 | t.sbox[word & 0xff];
            word ^= std::uint32_t(rcon) << 24;
            rcon = Times2(rcon);
        }
        expanded[i] = expanded[i - 4] ^ word;
    }
    // Equivalent inverse cipher: the encryption keys in reverse, the middle
    // ones through InvMixColumns, so every round is InvSub/InvShift/InvMix.
    for (int round = 0; round <= 10; ++round)
        for (int column = 0; column < 4; ++column)
        {
            const std::uint32_t word = expanded[(10 - round) * 4 + column];
            words_[round * 4 + column] = round == 0 || round == 10 ? word : InvMixColumn(word);
            Store32(bytes_[round] + 4 * column, words_[round * 4 + column]);
        }
}

void Aes128Decryptor::DecryptCbc(const std::uint8_t iv[16], const std::uint8_t* in, std::uint8_t* out,
                                 std::size_t size) const
{
#ifdef WII_DISC_ARM_AES
    uint8x16_t k[11];
    for (int round = 0; round <= 10; ++round) k[round] = vld1q_u8(bytes_[round]);
    // AESD is AddRoundKey + InvShiftRows + InvSubBytes, AESIMC InvMixColumns.
    const auto decrypt = [&k](uint8x16_t s) {
        for (int round = 0; round < 9; ++round) s = vaesimcq_u8(vaesdq_u8(s, k[round]));
        return veorq_u8(vaesdq_u8(s, k[9]), k[10]);
    };
    uint8x16_t previous = vld1q_u8(iv);
    std::size_t offset = 0;
    // Four blocks at a time hide the AES instructions' latency; all are read
    // before any is written, so decrypting in place stays correct.
    for (; offset + 64 <= size; offset += 64)
    {
        const uint8x16_t c0 = vld1q_u8(in + offset), c1 = vld1q_u8(in + offset + 16), c2 = vld1q_u8(in + offset + 32),
                         c3 = vld1q_u8(in + offset + 48);
        uint8x16_t s0 = c0, s1 = c1, s2 = c2, s3 = c3;
        for (int round = 0; round < 9; ++round)
        {
            s0 = vaesimcq_u8(vaesdq_u8(s0, k[round]));
            s1 = vaesimcq_u8(vaesdq_u8(s1, k[round]));
            s2 = vaesimcq_u8(vaesdq_u8(s2, k[round]));
            s3 = vaesimcq_u8(vaesdq_u8(s3, k[round]));
        }
        s0 = veorq_u8(veorq_u8(vaesdq_u8(s0, k[9]), k[10]), previous);
        s1 = veorq_u8(veorq_u8(vaesdq_u8(s1, k[9]), k[10]), c0);
        s2 = veorq_u8(veorq_u8(vaesdq_u8(s2, k[9]), k[10]), c1);
        s3 = veorq_u8(veorq_u8(vaesdq_u8(s3, k[9]), k[10]), c2);
        vst1q_u8(out + offset, s0);
        vst1q_u8(out + offset + 16, s1);
        vst1q_u8(out + offset + 32, s2);
        vst1q_u8(out + offset + 48, s3);
        previous = c3;
    }
    for (; offset < size; offset += 16)
    {
        const uint8x16_t block = vld1q_u8(in + offset);
        vst1q_u8(out + offset, veorq_u8(decrypt(block), previous));
        previous = block;
    }
#else
    const Tables& t = GetTables();
    std::uint8_t previous[16], block[16], plain[16];
    std::memcpy(previous, iv, 16);
    for (std::size_t offset = 0; offset < size; offset += 16)
    {
        std::memcpy(block, in + offset, 16);
        DecryptBlock(t, words_, block, plain);
        for (int i = 0; i < 16; ++i) out[offset + i] = plain[i] ^ previous[i];
        std::memcpy(previous, block, 16);
    }
#endif
}
} // namespace mscharged::platform::wii_disc
