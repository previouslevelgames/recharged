#include "Game/SAnimDecode.h"
#include "Game/SAnim.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

static_assert(sizeof(PackedScale) == 6 && offsetof(PackedScale, z) == 4);
static_assert(std::is_same_v<decltype(PackedScale::x), unsigned short>);
static_assert(std::numeric_limits<unsigned short>::digits == 16);

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

std::uint32_t Bits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

template<class T> struct Guarded
{
    std::uint32_t before = 0xabcdef01;
    T value{};
    std::uint32_t after = 0x23456789;
    void Check() const
    { Require(before == 0xabcdef01 && after == 0x23456789, "Decoder overwrote output guards"); }
};

using RotationDecoder = void (*)(nlQuaternion*, const void*);

void GoldenRotations()
{
    const unsigned char rot16[] = {0x80, 0x00, 0x7f, 0xff, 0xff, 0xff, 0x12, 0x34};
    const unsigned char rot12[] = {0x7f, 0xf0, 0x80, 0x00, 0x1f, 0xff};
    const unsigned char rot8[] = {0x80, 0x7f, 0xff, 0x12};
    const std::array<float, 4> expected16{-1, 0.999969482421875f, -0.000030517578125f, 0.1422119140625f};
    const std::array<float, 4> expected12{0.99951171875f, -1, 0.00048828125f, -0.00048828125f};
    const std::array<float, 4> expected8{-1, 0.9921875f, -0.0078125f, 0.140625f};
    Guarded<nlQuaternion> output;
    auto check = [&](RotationDecoder decode, const void* key, const auto& expected) {
        decode(&output.value, key);
        output.Check();
        for (unsigned i = 0; i < 4; ++i)
            Require(Bits(output.value.e[i]) == Bits(expected[i]), "Golden rotation changed");
    };
    check(SAnimDecodeRot16, rot16, expected16);
    check(SAnimDecodeRot12, rot12, expected12);
    check(SAnimDecodeRot8, rot8, expected8);
}

void AllRotations(unsigned width, RotationDecoder decode)
{
    const unsigned count = 1u << width, mask = count - 1;
    const unsigned bytes = 4 * width / 8;
    // The packed key is unaligned and ends exactly at its allocation boundary.
    // ASan can therefore detect a load beyond the original 8/6/4-byte contract.
    const auto storage = std::make_unique<unsigned char[]>(bytes + 1);
    unsigned char* key = storage.get() + 1;
    for (unsigned value = 0; value < count; ++value)
    {
        const std::array<unsigned, 4> components{
            value, value ^ mask, (value + count / 2) & mask, (13 * value + 47) & mask};
        std::memset(key, 0, bytes);
        storage[0] = 0xa5;
        // Rot12 stores both high bytes followed by their nibbles in one shared
        // byte: A-high, A-low/B-low, B-high. It is not a contiguous bit stream.
        // Each lane visits its entire signed input range.
        for (unsigned lane = 0; lane < 4; ++lane)
            for (unsigned bit = 0; bit < width; ++bit)
            {
                const unsigned stream = width == 12
                    ? (lane / 2) * 24 + (bit < 8 ? (lane % 2) * 16 + bit : 8 + (lane % 2) * 4 + bit - 8)
                    : lane * width + bit;
                if (components[lane] & (1u << (width - bit - 1)))
                    key[stream / 8] |= 1u << (7 - stream % 8);
            }
        std::array<unsigned char, 8> original{};
        std::memcpy(original.data(), key, bytes);
        Guarded<nlQuaternion> output;
        for (float& lane : output.value.e) lane = std::numeric_limits<float>::quiet_NaN();
        decode(&output.value, key);
        output.Check();
        Require(storage[0] == 0xa5 && std::memcmp(original.data(), key, bytes) == 0,
                "Rotation decoder mutated its packed input");
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            const int integer = components[lane] < count / 2 ? static_cast<int>(components[lane])
                               : static_cast<int>(components[lane]) - static_cast<int>(count);
            const float expected = static_cast<float>(std::ldexp(static_cast<double>(integer),
                                                                 -static_cast<int>(width - 1)));
            Require(Bits(output.value.e[lane]) == Bits(expected),
                    "Rotation differs from signed GQR dequantization");
        }
    }
}

void OriginalRot12Unpack()
{
    // Independent literal transcription of the original byte assignments,
    // followed by signed16 GQR6 dequantization. This oracle starts from raw
    // bytes, not from the encoder used by AllRotations.
    for (unsigned lane = 0; lane < 6; ++lane)
        for (unsigned value = 0; value < 256; ++value)
        {
            std::array<unsigned char, 6> key{0x91,0xe5,0x27,0x63,0x1a,0xfe};
            key[lane] = value;
            const std::array<unsigned char, 8> expanded{
                key[0], static_cast<unsigned char>(key[1] & 0xf0),
                key[2], static_cast<unsigned char>(key[1] << 4),
                key[3], static_cast<unsigned char>(key[4] & 0xf0),
                key[5], static_cast<unsigned char>(key[4] << 4)};
            Guarded<nlQuaternion> out;
            SAnimDecodeRot12(&out.value,key.data()); out.Check();
            for (unsigned component = 0; component < 4; ++component)
            {
                const unsigned word = expanded[2*component]*256u + expanded[2*component+1];
                const int signed_word = word >= 32768 ? int(word)-65536 : int(word);
                const float expected = float(std::ldexp(double(signed_word),-15));
                Require(Bits(out.value.e[component]) == Bits(expected), "Rot12 differs from original byte unpack/GQR loads");
            }
        }
    const unsigned char identity[] = {0,0,0,0,15,127};
    nlQuaternion out; SAnimDecodeRot12(&out,identity);
    Require(out.x == 0 && out.y == 0 && out.z == 0 && out.w == 2047.f/2048,
            "Rot12 high/low nibble identity vector changed");
}

void AllScales()
{
    for (unsigned value = 0; value <= 65535; ++value)
    {
        const PackedScale packed{static_cast<unsigned short>(value),
            static_cast<unsigned short>(value ^ 65535), static_cast<unsigned short>((value + 1024) & 65535)};
        std::array<unsigned char, sizeof(packed)> original{};
        std::memcpy(original.data(), &packed, sizeof(packed));
        Guarded<nlVector3> output;
        SAnimDecodeScale(&output.value, &packed);
        output.Check();
        const std::array<unsigned, 3> fields{packed.x, packed.y, packed.z};
        for (unsigned lane = 0; lane < 3; ++lane)
        {
            const float expected = static_cast<float>(std::ldexp(static_cast<double>(fields[lane]), -11));
            Require(Bits(output.value.e[lane]) == Bits(expected),
                    "Charged unsigned host-order scale changed");
        }
        Require(std::memcmp(original.data(), &packed, sizeof(packed)) == 0, "Scale decoder mutated input");
    }
}

void AllWeights()
{
    auto packed = std::make_unique<unsigned char[]>(1);
    for (unsigned value = 0; value <= 255; ++value)
    {
        packed[0] = static_cast<unsigned char>(value);
        Guarded<float> weight, morph;
        SAnimDecodeWeight(&weight.value, packed.get());
        SAnimDecodeMorphWeight(&morph.value, packed.get());
        weight.Check(); morph.Check();
        const float expected = static_cast<float>(static_cast<double>(value) / 255.0);
        Require(Bits(weight.value) == Bits(expected) && Bits(morph.value) == Bits(expected),
                "Byte weight or morph weight changed");
        Require(packed[0] == value, "Weight decoder mutated input");
    }
}
}

int main()
{
    try
    {
        // Native decoding has no mutable GQR state and repeated init is safe.
        GoldenRotations();
        SAnimInitGQR();
        AllRotations(16, SAnimDecodeRot16);
        AllRotations(12, SAnimDecodeRot12);
        OriginalRot12Unpack();
        AllRotations(8, SAnimDecodeRot8);
        AllScales();
        AllWeights();
        SAnimInitGQR();
        GoldenRotations();
        std::cout << "SAnim decoders passed: 65,536 Rot16, 4,096 Rot12, 256 Rot8 keys (all lanes), "
                     "1,536 original Rot12 byte-unpack cases, 65,536 unsigned scale triples, "
                     "256 weight/morph pairs; guards and repeated init.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
