#include "platform/rlg_material_parameters.h"

#include "platform/game_allocation_ownership.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/glx/GXConstantColourMaterialProgram.h"
#include "NL/glx/GXFloatTexturedColourMaterialProgram.h"
#include "NL/glx/GXMaskedSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXScrollingDiffuseMaterialProgram.h"
#include "NL/glx/GXMovieMaterialProgram.h"
#include "NL/glx/GXScrollingSpecularMaterialProgram.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
std::uint32_t Word(const unsigned char* input, std::size_t bytes)
{
    const auto domain = FindGameByteDomain(input, bytes);
    if (domain == GameByteDomain::NativeHeader)
    {
        if (bytes == 2)
        {
            std::uint16_t value;
            std::memcpy(&value, input, 2);
            return value;
        }
        std::uint32_t value;
        std::memcpy(&value, input, 4);
        return value;
    }
    if (domain != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RLG material scalar is not owned Wii or converted header data");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i)
        value = (value << 8) | input[i];
    return value;
}
}

void CopyRLGMaterialParameterBytes(void* output, const void* input, std::size_t bytes)
{
    if (!bytes)
        return;
    if (FindGameByteDomain(input, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RLG parameter copy requires the real completed Wii input");
    GameByteWriteReservation write(output, bytes);
    if (!write.Tracked())
        throw std::invalid_argument("RLG parameter output has no actual original owner");
    std::memcpy(output, input, bytes);
    write.Complete(GameByteDomain::WiiSerialized);
}

void DecodeRLGMaterialParameters(glModelPacket* packet)
{
    static_assert(sizeof(glTextureBinding) == 8);
    static_assert(offsetof(glTextureBinding, texture) == 0);
    static_assert(offsetof(glTextureBinding, textureIndex) == 4);
    static_assert(offsetof(glTextureBinding, flags) == 6);
    static_assert(offsetof(glTextureBinding, unknown07) == 7);
    static_assert(sizeof(GXFloatTexturedColourParameters) == 8);
    static_assert(sizeof(GXConstantColourParameters) == 24);
    static_assert(offsetof(GXConstantColourParameters, constantColour) == 8);
    static_assert(sizeof(GXMaskedSpecularFresnelParameters) == 48);
    static_assert(offsetof(GXMaskedSpecularFresnelParameters, specularTexture) == 8);
    static_assert(offsetof(GXMaskedSpecularFresnelParameters, specularMaskTexture) == 16);
    static_assert(offsetof(GXMaskedSpecularFresnelParameters, specularAmount) == 24);
    static_assert(offsetof(GXMaskedSpecularFresnelParameters, lightingEnabled) == 40);
    static_assert(sizeof(GXScrollingDiffuseParameters) == 36);
    static_assert(offsetof(GXScrollingDiffuseParameters, scrollSpeedX) == 8);
    static_assert(offsetof(GXScrollingDiffuseParameters, shadowEnabled) == 16);
    static_assert(sizeof(GXMovieParameters) == 24);
    static_assert(offsetof(GXMovieParameters, tint) == 8);
    static_assert(sizeof(GXScrollingSpecularParameters) == 60);
    static_assert(offsetof(GXScrollingSpecularParameters, specularTexture) == 8);
    static_assert(offsetof(GXScrollingSpecularParameters, specularLevel) == 16);
    static_assert(offsetof(GXScrollingSpecularParameters, specularColour) == 24);
    static_assert(offsetof(GXScrollingSpecularParameters, scrollSpeedX) == 40);
    static_assert(offsetof(GXScrollingSpecularParameters, scrollSpecularTexture) == 48);
    auto* program = static_cast<GLMaterialProgram*>(packet->materialProgram);
    if (!program)
        throw std::invalid_argument("Original RLG material lookup has no genuine source provider");
    std::size_t bytes;
    std::size_t bindings = 1;
    switch (program->programHash)
    {
    case 0x19065BF6:
        bytes = sizeof(GXFloatTexturedColourParameters);
        if (program->parameterDataSize != bytes || program->parameterCount != 1)
            throw std::invalid_argument("FloatTexturedColour source layout differs from authored ABI");
        break;
    case 0xEE9D919D:
        bytes = sizeof(GXConstantColourParameters);
        if (program->parameterDataSize != bytes || program->parameterCount != 2)
            throw std::invalid_argument("ConstantColour source layout differs from authored ABI");
        break;
    case 0x32475C7D:
        bytes = sizeof(GXMaskedSpecularFresnelParameters);
        bindings = 3;
        if (program->parameterDataSize != bytes || program->parameterCount != 9)
            throw std::invalid_argument("MaskedSpecularFresnel source layout differs from authored ABI");
        break;
    case 0x2169DB5C:
        bytes = sizeof(GXScrollingDiffuseParameters);
        if (program->parameterDataSize != bytes || program->parameterCount != 8)
            throw std::invalid_argument("ScrollingDiffuse source layout differs from authored ABI");
        break;
    case 0xEC35CAAB:
        bytes = sizeof(GXMovieParameters);
        if (program->parameterDataSize != bytes || program->parameterCount != 2)
            throw std::invalid_argument("Movie source layout differs from authored ABI");
        break;
    case 0x3ECCD955:
        bytes = sizeof(GXScrollingSpecularParameters);
        bindings = 2;
        if (program->parameterDataSize != bytes || program->parameterCount != 10)
            throw std::invalid_argument("ScrollingSpecular source layout differs from authored ABI");
        break;
    default:
        throw std::invalid_argument("Authored RLG material parameter layout is not yet qualified natively");
    }
    auto* data = static_cast<unsigned char*>(packet->materialParameters);
    GameCompletedSpan span{};
    if (!FindGameCompletedSpan(data, bytes, span))
        throw std::invalid_argument("RLG material parameters leave the genuine completed logical span");
    std::array<unsigned char, sizeof(GXScrollingSpecularParameters)> native{};
    std::memcpy(native.data(), data, bytes);
    for (std::size_t binding = 0; binding < bindings; ++binding)
    {
        const auto at = binding * sizeof(glTextureBinding);
        const std::uint32_t texture = Word(data + at, 4);
        const std::uint16_t index = static_cast<std::uint16_t>(Word(data + at + 4, 2));
        std::memcpy(native.data() + at, &texture, 4);
        std::memcpy(native.data() + at + 4, &index, 2);
        // The two flag bytes are already scalar bytes, with no endian conversion.
        const auto flagDomain = FindGameByteDomain(data + at + 6, 2);
        if (flagDomain != GameByteDomain::WiiSerialized && flagDomain != GameByteDomain::NativeHeader)
            throw std::invalid_argument("RLG texture flags have no authored or converted header domain");
    }
    // Copy the bit patterns, rather than arithmetic on the float values. This
    // also preserves signed zero and all authored NaN payloads.
    for (std::size_t at = bindings * sizeof(glTextureBinding); at < bytes; at += 4)
    {
        const std::uint32_t bits = Word(data + at, 4);
        std::memcpy(native.data() + at, &bits, 4);
    }
    GameByteWriteReservation write(data, bytes);
    if (!write.Tracked())
        throw std::invalid_argument("RLG material conversion has no original owning allocation");
    std::memcpy(data, native.data(), bytes);
    write.Complete(GameByteDomain::NativeHeader);
}
}
