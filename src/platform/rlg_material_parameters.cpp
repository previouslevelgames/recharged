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
#include "NL/glx/GXCharacterSkinCustomMaterialProgram.h"
#include "NL/glx/GXSpecularMaterialProgram.h"
#include "NL/glx/GXSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXMegaDiffuseMaterialProgram.h"
#include "NL/glx/GXCharacterDamageMaterialProgram.h"
#include "NL/glx/GXBlackTextureAlphaMaterialProgram.h"
#include "NL/glx/GXColourFresnelMaterialProgram.h"
#include "NL/glx/GXMegaSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXMegaSpecularMaterialProgram.h"
#include "NL/glx/GXSkinnedUnlitTextureMaterialProgram.h"
#include "NL/glx/GXMaskedDetailBlendMaterialProgram.h"
#include "NL/glx/GXSpecularDetailBlendMaterialProgram.h"
#include "NL/glx/GXShadowedDetailBlendMaterialProgram.h"
#include "NL/glx/GXCameraScrolledOverlayMaterialProgram.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include "NL/glx/GXScrollingShadowedDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingCameraOverlayMaterialProgram.h"
#include "NL/glx/GXDetailModulateMaterialProgram.h"
#include "NL/glx/GXCrystalMaterialProgram.h"
#include "NL/glx/GXScrollingMaskedDetailBlendMaterialProgram.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>

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

// This tag is native ownership metadata, outside the game parameter object.
// Shared authored records must reuse their live view, including later writes
// made by the original material setters, rather than decode the file again.
struct alignas(std::max_align_t) SkinnedParameterTag
{
    std::uint32_t program;
    std::uint32_t wireBytes;
    std::uint32_t nativeBytes;
    std::uint32_t parameters;
};

// SizeAt is the native offset of the u32 skin-matrix byte count that follows
// the matrix pointer (named skinMatrixBytes or skinMatricesSize by source).
template<class Parameters, std::size_t SizeAt>
void DecodeSkinnedParameters(glModelPacket* packet, std::size_t wireBytes,
    std::size_t bindings, std::uint32_t parameterCount)
{
    static_assert(std::is_trivially_copyable_v<Parameters>);
    static_assert(SizeAt == offsetof(Parameters, skinMatrices) + sizeof(Parameters::skinMatrices));
    static_assert(SizeAt + 4 <= sizeof(Parameters));
    auto* program = static_cast<GLMaterialProgram*>(packet->materialProgram);
    if (program->parameterDataSize != sizeof(Parameters)
        || program->parameterCount != parameterCount
        || offsetof(Parameters, skinMatrices) != bindings * sizeof(glTextureBinding))
        throw std::invalid_argument("Original skinned material layout differs from the qualified native ABI");
    auto* raw = static_cast<unsigned char*>(packet->materialParameters);
    GameCompletedSpan source{};
    if (!FindGameCompletedSpan(raw, wireBytes, source)
        || FindGameByteDomain(raw, wireBytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Skinned material parameters require completed owned Wii records");

    const SkinnedParameterTag tag{program->programHash, static_cast<std::uint32_t>(wireBytes),
        static_cast<std::uint32_t>(sizeof(Parameters)), parameterCount};
    constexpr std::size_t nativeBytes = sizeof(SkinnedParameterTag) + sizeof(Parameters);
    GameNativeBackingSpan prior{};
    if (FindGameNativeBacking(raw, wireBytes, prior))
    {
        SkinnedParameterTag found{};
        if (prior.bytes != nativeBytes)
            throw std::invalid_argument("Skinned material view has a different native footprint");
        std::memcpy(&found, prior.data, sizeof(found));
        if (found.program != tag.program || found.wireBytes != tag.wireBytes
            || found.nativeBytes != tag.nativeBytes || found.parameters != tag.parameters)
            throw std::invalid_argument("Shared skinned material record has a different layout");
        packet->materialParameters = static_cast<unsigned char*>(prior.data) + sizeof(found);
        return;
    }

    Parameters native{};
    auto* output = reinterpret_cast<unsigned char*>(&native);
    for (std::size_t binding = 0; binding < bindings; ++binding)
    {
        const auto at = binding * sizeof(glTextureBinding);
        const std::uint32_t texture = Word(raw + at, 4);
        const std::uint16_t index = static_cast<std::uint16_t>(Word(raw + at + 4, 2));
        std::memcpy(output + at, &texture, 4);
        std::memcpy(output + at + 4, &index, 2);
        std::memcpy(output + at + 6, raw + at + 6, 2);
    }
    const auto pointerAt = bindings * sizeof(glTextureBinding);
    // Authored NPC records contain a null matrix pointer. A nonzero Wii address
    // needs a real address-domain provider; it must never be cast to a host pointer.
    if (Word(raw + pointerAt, 4) != 0)
        throw std::invalid_argument("Nonzero authored skin matrix addresses remain unqualified");
    native.skinMatrices = nullptr;
    const auto tailAt = SizeAt;
    for (std::size_t at = pointerAt + 4; at < wireBytes; at += 4)
    {
        const auto bits = Word(raw + at, 4);
        std::memcpy(output + tailAt + at - (pointerAt + 4), &bits, 4);
    }

    GameNativeBackingReservation backing(raw, wireBytes, nativeBytes);
    auto* data = static_cast<unsigned char*>(backing.Data());
    std::memcpy(data, &tag, sizeof(tag));
    new (data + sizeof(tag)) Parameters(native);
    backing.Commit();
    packet->materialParameters = data + sizeof(tag);
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
    switch (program->programHash)
    {
    case 0x041C3281:
        static_assert(sizeof(GXCharacterSkinCustomParameters) == 48);
        static_assert(offsetof(GXCharacterSkinCustomParameters, lightingEnabled) == 40);
        static_assert(sizeof(GXCharacterSkinCustomParameters::skinMatrixBytes) == 4);
        DecodeSkinnedParameters<GXCharacterSkinCustomParameters,
            offsetof(GXCharacterSkinCustomParameters, skinMatrixBytes)>(packet, 40, 2, 7);
        return;
    case 0x22CADB20:
        static_assert(sizeof(GXSpecularParameters) == 80);
        static_assert(offsetof(GXSpecularParameters, lightingEnabled) == 72);
        static_assert(sizeof(GXSpecularParameters::skinMatrixBytes) == 4);
        DecodeSkinnedParameters<GXSpecularParameters,
            offsetof(GXSpecularParameters, skinMatrixBytes)>(packet, 72, 3, 11);
        return;
    case 0x46B46F88:
        static_assert(sizeof(GXSpecularFresnelParameters) == 80);
        static_assert(offsetof(GXSpecularFresnelParameters, lightingEnabled) == 72);
        static_assert(sizeof(GXSpecularFresnelParameters::skinMatrixBytes) == 4);
        DecodeSkinnedParameters<GXSpecularFresnelParameters,
            offsetof(GXSpecularFresnelParameters, skinMatrixBytes)>(packet, 72, 4, 13);
        return;
    case 0x44410B9B:
        static_assert(sizeof(GXMegaDiffuseParameters) == 56);
        static_assert(offsetof(GXMegaDiffuseParameters, lightingEnabled) == 52);
        static_assert(sizeof(GXMegaDiffuseParameters::skinMatrixBytes) == 4);
        DecodeSkinnedParameters<GXMegaDiffuseParameters,
            offsetof(GXMegaDiffuseParameters, skinMatrixBytes)>(packet, 52, 3, 9);
        return;
    case 0x1ACE1D01:
        // Wii 104-byte record: six bindings, null authored matrix pointer at
        // 0x30, then u32/float words; the native pointer widens it to 112.
        static_assert(sizeof(GXCharacterDamageParameters) == 112);
        static_assert(sizeof(GXCharacterDamageParameters::skinMatricesSize) == 4);
        static_assert(offsetof(GXCharacterDamageParameters, alphaValue) == 60);
        static_assert(offsetof(GXCharacterDamageParameters, damage2Enabled) == 104);
        DecodeSkinnedParameters<GXCharacterDamageParameters,
            offsetof(GXCharacterDamageParameters, skinMatricesSize)>(packet, 104, 6, 19);
        return;
    // The remaining skinned programs share the same verified shape: bindings,
    // a null authored matrix pointer, its u32 byte count, then 32-bit words.
    case 0xBACEA013:
        static_assert(sizeof(GXBlackTextureAlphaParameters) == 24);
        static_assert(sizeof(GXBlackTextureAlphaParameters::skinMatrixBytes) == 4);
        DecodeSkinnedParameters<GXBlackTextureAlphaParameters,
            offsetof(GXBlackTextureAlphaParameters, skinMatrixBytes)>(packet, 16, 1, 2);
        return;
    case 0x5D6C62BA:
        static_assert(sizeof(GXSkinnedUnlitTextureParameters) == 24);
        static_assert(sizeof(GXSkinnedUnlitTextureParameters::skinMatrixBytes) == 4);
        DecodeSkinnedParameters<GXSkinnedUnlitTextureParameters,
            offsetof(GXSkinnedUnlitTextureParameters, skinMatrixBytes)>(packet, 16, 1, 2);
        return;
    case 0x52A9BB54:
        static_assert(sizeof(GXColourFresnelParameters) == 56);
        static_assert(sizeof(GXColourFresnelParameters::skinMatrixBytes) == 4);
        static_assert(offsetof(GXColourFresnelParameters, lightingEnabled) == 48);
        DecodeSkinnedParameters<GXColourFresnelParameters,
            offsetof(GXColourFresnelParameters, skinMatrixBytes)>(packet, 48, 3, 8);
        return;
    case 0x632E7422:
        static_assert(sizeof(GXMegaSpecularFresnelParameters) == 96);
        static_assert(sizeof(GXMegaSpecularFresnelParameters::skinMatrixBytes) == 4);
        static_assert(offsetof(GXMegaSpecularFresnelParameters, blackOnly) == 88);
        DecodeSkinnedParameters<GXMegaSpecularFresnelParameters,
            offsetof(GXMegaSpecularFresnelParameters, skinMatrixBytes)>(packet, 88, 5, 16);
        return;
    case 0xF540EBBA:
        static_assert(sizeof(GXMegaSpecularParameters) == 88);
        static_assert(sizeof(GXMegaSpecularParameters::skinMatrixBytes) == 4);
        static_assert(offsetof(GXMegaSpecularParameters, specularColour) == 60);
        static_assert(offsetof(GXMegaSpecularParameters, lightingEnabled) == 84);
        DecodeSkinnedParameters<GXMegaSpecularParameters,
            offsetof(GXMegaSpecularParameters, skinMatrixBytes)>(packet, 84, 4, 13);
        return;
    }
    // Stadium world programs: leading texture bindings, then only 32-bit
    // float/int words (no pointers or sub-word fields), so the native layout
    // equals the authored Wii record and the generic conversion below applies.
    static_assert(sizeof(GXMaskedDetailBlendParameters) == 36);
    static_assert(offsetof(GXMaskedDetailBlendParameters, blendAmount) == 24);
    static_assert(sizeof(GXSpecularDetailBlendParameters) == 68);
    static_assert(offsetof(GXSpecularDetailBlendParameters, blendAmount) == 32);
    static_assert(offsetof(GXSpecularDetailBlendParameters, specularColour) == 44);
    static_assert(offsetof(GXSpecularDetailBlendParameters, shadowEnabled) == 64);
    static_assert(sizeof(GXShadowedDetailBlendParameters) == 52);
    static_assert(offsetof(GXShadowedDetailBlendParameters, blendAmount) == 32);
    static_assert(offsetof(GXShadowedDetailBlendParameters, unusedParameter) == 48);
    static_assert(sizeof(GXCameraScrolledOverlayParameters) == 48);
    static_assert(offsetof(GXCameraScrolledOverlayParameters, overlayScale) == 24);
    static_assert(offsetof(GXCameraScrolledOverlayParameters, shadowEnabled) == 44);
    static_assert(sizeof(GXShadowVolumeParameters) == 12);
    static_assert(offsetof(GXShadowVolumeParameters, useFixedColour) == 8);
    static_assert(sizeof(GXScrollingShadowedDetailBlendParameters) == 72);
    static_assert(offsetof(GXScrollingShadowedDetailBlendParameters, diffuseScrollSpeedX) == 32);
    static_assert(offsetof(GXScrollingShadowedDetailBlendParameters, shadowEnabled) == 68);
    static_assert(sizeof(GXScrollingCameraOverlayParameters) == 60);
    static_assert(offsetof(GXScrollingCameraOverlayParameters, overlayScale) == 24);
    static_assert(offsetof(GXScrollingCameraOverlayParameters, shadowEnabled) == 56);
    static_assert(sizeof(GXDetailModulateParameters) == 28);
    static_assert(offsetof(GXDetailModulateParameters, detailStrength) == 16);
    static_assert(sizeof(GXCrystalMaterialParameters) == 24);
    static_assert(offsetof(GXCrystalMaterialParameters, rampTexture) == 16);
    static_assert(sizeof(GXScrollingMaskedDetailBlendParameters) == 60);
    static_assert(offsetof(GXScrollingMaskedDetailBlendParameters, diffuseScrollSpeedX) == 24);
    static_assert(offsetof(GXScrollingMaskedDetailBlendParameters, shadowEnabled) == 56);
    std::size_t bytes;
    std::size_t bindings = 1;
    std::uint32_t count;
    switch (program->programHash)
    {
    case 0x09609A35:
        bytes = sizeof(GXMaskedDetailBlendParameters); bindings = 3; count = 6;
        break;
    case 0x112AB470:
        bytes = sizeof(GXSpecularDetailBlendParameters); bindings = 4; count = 10;
        break;
    case 0x1D8CCB9B:
        bytes = sizeof(GXShadowedDetailBlendParameters); bindings = 4; count = 9;
        break;
    case 0x32BC21E8:
        bytes = sizeof(GXCameraScrolledOverlayParameters); bindings = 3; count = 9;
        break;
    case 0x386ECBDD:
        bytes = sizeof(GXShadowVolumeParameters); bindings = 1; count = 2;
        break;
    case 0x552041EC:
        bytes = sizeof(GXScrollingShadowedDetailBlendParameters); bindings = 4; count = 14;
        break;
    case 0x845CAD59:
        bytes = sizeof(GXScrollingCameraOverlayParameters); bindings = 3; count = 12;
        break;
    case 0x9BE20E02:
        bytes = sizeof(GXDetailModulateParameters); bindings = 2; count = 5;
        break;
    case 0xEB3E6061:
        bytes = sizeof(GXCrystalMaterialParameters); bindings = 3; count = 3;
        break;
    case 0xF2D57AC6:
        bytes = sizeof(GXScrollingMaskedDetailBlendParameters); bindings = 3; count = 12;
        break;
    default:
        count = 0;
        break;
    }
    if (count)
    {
        if (program->parameterDataSize != bytes || program->parameterCount != count)
            throw std::invalid_argument("Stadium material source layout differs from authored ABI");
    }
    else switch (program->programHash)
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
    std::array<unsigned char, sizeof(GXScrollingShadowedDetailBlendParameters)> native{};
    if (bytes > native.size())
        throw std::invalid_argument("RLG material parameters exceed the qualified native record size");
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
