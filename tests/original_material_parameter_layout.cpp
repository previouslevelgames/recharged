// Original Wii parameter descriptors retain fixed scalar words while the matrix
// address uses the host pointer width. These are compile-time ABI assertions;
// no material initialization, rendering or resource readiness executes here.
#include "NL/gl/glMaterialParameters.h"
#include <stddef.h>
#include "NL/glx/GXBlackTextureAlphaMaterialProgram.h"
#include "NL/glx/GXCharacterDamageMaterialProgram.h"
#include "NL/glx/GXColourFresnelMaterialProgram.h"
#include "NL/glx/GXMegaDiffuseMaterialProgram.h"
#include "NL/glx/GXMegaSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXMegaSpecularMaterialProgram.h"
#include "NL/glx/GXSkinnedMultiLightMaterialProgram.h"
#include "NL/glx/GXSkinnedUnlitTextureMaterialProgram.h"
#include "NL/glx/GXSpecularFresnelMaterialProgram.h"

namespace {
constexpr size_t matrix_pointer_delta = sizeof(void*) - 4;
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(glMaterialBufferAddress) == sizeof(void*));
template <class> struct MaterialBufferSignature;
template <class Result, class Packet, class Hash, class Address, class Count>
struct MaterialBufferSignature<Result (*)(Packet, Hash, Address, Count)>
{
    typedef Address AddressType;
};
static_assert(sizeof(MaterialBufferSignature<decltype(&glSetMaterialBufferParameter)>::AddressType)
    == sizeof(void*));
static_assert(sizeof(glTextureBinding) == 8);
static_assert(offsetof(glTextureBinding, textureIndex) == 4);

// GXBlackTextureAlphaMaterialProgram: original Wii size 16.
static_assert(sizeof(GXBlackTextureAlphaParameters) ==
    ((16 + matrix_pointer_delta + alignof(GXBlackTextureAlphaParameters) - 1)
        / alignof(GXBlackTextureAlphaParameters)) * alignof(GXBlackTextureAlphaParameters));
static_assert(sizeof(((GXBlackTextureAlphaParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXBlackTextureAlphaParameters, skinMatrixBytes) == 12 + matrix_pointer_delta);
static_assert(offsetof(GXBlackTextureAlphaParameters, diffuseTexture) == 0);
static_assert(offsetof(GXBlackTextureAlphaParameters, skinMatrices) == 8);

// GXCharacterDamageMaterialProgram: original Wii size 104.
static_assert(sizeof(GXCharacterDamageParameters) ==
    ((104 + matrix_pointer_delta + alignof(GXCharacterDamageParameters) - 1)
        / alignof(GXCharacterDamageParameters)) * alignof(GXCharacterDamageParameters));
static_assert(sizeof(((GXCharacterDamageParameters*)0)->skinMatricesSize) == 4);
static_assert(offsetof(GXCharacterDamageParameters, skinMatricesSize) == 52 + matrix_pointer_delta);
static_assert(sizeof(((GXCharacterDamageParameters*)0)->shadowLevel) == 4);
static_assert(offsetof(GXCharacterDamageParameters, shadowLevel) == 84 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, diffuseTexture) == 0);
static_assert(offsetof(GXCharacterDamageParameters, specularTexture) == 8);
static_assert(offsetof(GXCharacterDamageParameters, specularMaskTexture) == 16);
static_assert(offsetof(GXCharacterDamageParameters, megaTexture) == 24);
static_assert(offsetof(GXCharacterDamageParameters, damage1Texture) == 32);
static_assert(offsetof(GXCharacterDamageParameters, damage2Texture) == 40);
static_assert(offsetof(GXCharacterDamageParameters, skinMatrices) == 48);
static_assert(offsetof(GXCharacterDamageParameters, alphaValue) == 56 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, specularAmount) == 60 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, specularScaleX) == 64 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, specularScaleY) == 68 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, fresnelRamp) == 72 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, megaBlend) == 76 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, unidentified50) == 80 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, shadowLevel) == 84 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, lightingEnabled) == 88 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, blackOnly) == 92 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, damage1Enabled) == 96 + matrix_pointer_delta);
static_assert(offsetof(GXCharacterDamageParameters, damage2Enabled) == 100 + matrix_pointer_delta);

// GXColourFresnelMaterialProgram: original Wii size 48.
static_assert(sizeof(GXColourFresnelParameters) ==
    ((48 + matrix_pointer_delta + alignof(GXColourFresnelParameters) - 1)
        / alignof(GXColourFresnelParameters)) * alignof(GXColourFresnelParameters));
static_assert(sizeof(((GXColourFresnelParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXColourFresnelParameters, skinMatrixBytes) == 28 + matrix_pointer_delta);
static_assert(offsetof(GXColourFresnelParameters, diffuseTexture) == 0);
static_assert(offsetof(GXColourFresnelParameters, detailTexture) == 8);
static_assert(offsetof(GXColourFresnelParameters, shadowTexture) == 16);
static_assert(offsetof(GXColourFresnelParameters, skinMatrices) == 24);
static_assert(offsetof(GXColourFresnelParameters, blendAmount) == 32 + matrix_pointer_delta);
static_assert(offsetof(GXColourFresnelParameters, alphaValue) == 36 + matrix_pointer_delta);
static_assert(offsetof(GXColourFresnelParameters, colourFresnelRamp) == 40 + matrix_pointer_delta);
static_assert(offsetof(GXColourFresnelParameters, lightingEnabled) == 44 + matrix_pointer_delta);

// GXMegaDiffuseMaterialProgram: original Wii size 52.
static_assert(sizeof(GXMegaDiffuseParameters) ==
    ((52 + matrix_pointer_delta + alignof(GXMegaDiffuseParameters) - 1)
        / alignof(GXMegaDiffuseParameters)) * alignof(GXMegaDiffuseParameters));
static_assert(sizeof(((GXMegaDiffuseParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXMegaDiffuseParameters, skinMatrixBytes) == 28 + matrix_pointer_delta);
static_assert(sizeof(((GXMegaDiffuseParameters*)0)->shadowLevel) == 4);
static_assert(offsetof(GXMegaDiffuseParameters, shadowLevel) == 44 + matrix_pointer_delta);
static_assert(offsetof(GXMegaDiffuseParameters, diffuseTexture) == 0);
static_assert(offsetof(GXMegaDiffuseParameters, detailTexture) == 8);
static_assert(offsetof(GXMegaDiffuseParameters, megaTexture) == 16);
static_assert(offsetof(GXMegaDiffuseParameters, skinMatrices) == 24);
static_assert(offsetof(GXMegaDiffuseParameters, blendAmount) == 32 + matrix_pointer_delta);
static_assert(offsetof(GXMegaDiffuseParameters, alphaValue) == 36 + matrix_pointer_delta);
static_assert(offsetof(GXMegaDiffuseParameters, megaBlend) == 40 + matrix_pointer_delta);
static_assert(offsetof(GXMegaDiffuseParameters, shadowLevel) == 44 + matrix_pointer_delta);
static_assert(offsetof(GXMegaDiffuseParameters, lightingEnabled) == 48 + matrix_pointer_delta);

// GXMegaSpecularFresnelMaterialProgram: original Wii size 88.
static_assert(sizeof(GXMegaSpecularFresnelParameters) ==
    ((88 + matrix_pointer_delta + alignof(GXMegaSpecularFresnelParameters) - 1)
        / alignof(GXMegaSpecularFresnelParameters)) * alignof(GXMegaSpecularFresnelParameters));
static_assert(sizeof(((GXMegaSpecularFresnelParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXMegaSpecularFresnelParameters, skinMatrixBytes) == 44 + matrix_pointer_delta);
static_assert(sizeof(((GXMegaSpecularFresnelParameters*)0)->shadowLevel) == 4);
static_assert(offsetof(GXMegaSpecularFresnelParameters, shadowLevel) == 76 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, diffuseTexture) == 0);
static_assert(offsetof(GXMegaSpecularFresnelParameters, detailTexture) == 8);
static_assert(offsetof(GXMegaSpecularFresnelParameters, specularTexture) == 16);
static_assert(offsetof(GXMegaSpecularFresnelParameters, specularMaskTexture) == 24);
static_assert(offsetof(GXMegaSpecularFresnelParameters, megaTexture) == 32);
static_assert(offsetof(GXMegaSpecularFresnelParameters, skinMatrices) == 40);
static_assert(offsetof(GXMegaSpecularFresnelParameters, blendAmount) == 48 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, alphaValue) == 52 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, specularAmount) == 56 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, specularScaleX) == 60 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, specularScaleY) == 64 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, fresnelRamp) == 68 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, megaBlend) == 72 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, shadowLevel) == 76 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, lightingEnabled) == 80 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularFresnelParameters, blackOnly) == 84 + matrix_pointer_delta);

// GXMegaSpecularMaterialProgram: original Wii size 84.
static_assert(sizeof(GXMegaSpecularParameters) ==
    ((84 + matrix_pointer_delta + alignof(GXMegaSpecularParameters) - 1)
        / alignof(GXMegaSpecularParameters)) * alignof(GXMegaSpecularParameters));
static_assert(sizeof(((GXMegaSpecularParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXMegaSpecularParameters, skinMatrixBytes) == 36 + matrix_pointer_delta);
static_assert(sizeof(((GXMegaSpecularParameters*)0)->shadowLevel) == 4);
static_assert(offsetof(GXMegaSpecularParameters, shadowLevel) == 76 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, diffuseTexture) == 0);
static_assert(offsetof(GXMegaSpecularParameters, detailTexture) == 8);
static_assert(offsetof(GXMegaSpecularParameters, glossTexture) == 16);
static_assert(offsetof(GXMegaSpecularParameters, megaTexture) == 24);
static_assert(offsetof(GXMegaSpecularParameters, skinMatrices) == 32);
static_assert(offsetof(GXMegaSpecularParameters, blendAmount) == 40 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, alphaValue) == 44 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, specularLevel) == 48 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, specularExponent) == 52 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, specularColour) == 56 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, megaBlend) == 72 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, shadowLevel) == 76 + matrix_pointer_delta);
static_assert(offsetof(GXMegaSpecularParameters, lightingEnabled) == 80 + matrix_pointer_delta);

// GXSkinnedMultiLightMaterialProgram: original Wii size 368.
static_assert(sizeof(GXSkinnedMultiLightParameters) ==
    ((368 + matrix_pointer_delta + alignof(GXSkinnedMultiLightParameters) - 1)
        / alignof(GXSkinnedMultiLightParameters)) * alignof(GXSkinnedMultiLightParameters));
static_assert(sizeof(((GXSkinnedMultiLightParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXSkinnedMultiLightParameters, skinMatrixBytes) == 28 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, diffuseTexture) == 0);
static_assert(offsetof(GXSkinnedMultiLightParameters, detailTexture) == 8);
static_assert(offsetof(GXSkinnedMultiLightParameters, rampTexture) == 16);
static_assert(offsetof(GXSkinnedMultiLightParameters, skinMatrices) == 24);
static_assert(offsetof(GXSkinnedMultiLightParameters, blendAmount) == 32 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, animateNormalTexCoords) == 36 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightDirections[0]) == 40 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightDirections[1]) == 52 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightDirections[2]) == 64 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightDirections[3]) == 76 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[0].position) == 88 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[0].distance) == 100 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[1].position) == 104 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[1].distance) == 116 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[2].position) == 120 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[2].distance) == 132 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[3].position) == 136 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, pointLights[3].distance) == 148 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[0].direction) == 152 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[0].exponent) == 164 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[1].direction) == 168 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[1].exponent) == 180 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[2].direction) == 184 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[2].exponent) == 196 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[3].direction) == 200 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLights[3].exponent) == 212 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[0]) == 216 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[1]) == 232 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[2]) == 248 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[3]) == 264 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[4]) == 280 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[5]) == 296 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[6]) == 312 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, lightColours[7]) == 328 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, useDirectionalLights) == 344 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, diffuseLightCount) == 360 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedMultiLightParameters, specularLightCount) == 364 + matrix_pointer_delta);

// GXSkinnedUnlitTextureMaterialProgram: original Wii size 16.
static_assert(sizeof(GXSkinnedUnlitTextureParameters) ==
    ((16 + matrix_pointer_delta + alignof(GXSkinnedUnlitTextureParameters) - 1)
        / alignof(GXSkinnedUnlitTextureParameters)) * alignof(GXSkinnedUnlitTextureParameters));
static_assert(sizeof(((GXSkinnedUnlitTextureParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXSkinnedUnlitTextureParameters, skinMatrixBytes) == 12 + matrix_pointer_delta);
static_assert(offsetof(GXSkinnedUnlitTextureParameters, diffuseTexture) == 0);
static_assert(offsetof(GXSkinnedUnlitTextureParameters, skinMatrices) == 8);

// GXSpecularFresnelMaterialProgram: original Wii size 72.
static_assert(sizeof(GXSpecularFresnelParameters) ==
    ((72 + matrix_pointer_delta + alignof(GXSpecularFresnelParameters) - 1)
        / alignof(GXSpecularFresnelParameters)) * alignof(GXSpecularFresnelParameters));
static_assert(sizeof(((GXSpecularFresnelParameters*)0)->skinMatrixBytes) == 4);
static_assert(offsetof(GXSpecularFresnelParameters, skinMatrixBytes) == 36 + matrix_pointer_delta);
static_assert(sizeof(((GXSpecularFresnelParameters*)0)->shadowLevel) == 4);
static_assert(offsetof(GXSpecularFresnelParameters, shadowLevel) == 64 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, diffuseTexture) == 0);
static_assert(offsetof(GXSpecularFresnelParameters, detailTexture) == 8);
static_assert(offsetof(GXSpecularFresnelParameters, specularTexture) == 16);
static_assert(offsetof(GXSpecularFresnelParameters, specularMaskTexture) == 24);
static_assert(offsetof(GXSpecularFresnelParameters, skinMatrices) == 32);
static_assert(offsetof(GXSpecularFresnelParameters, blendAmount) == 40 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, alphaValue) == 44 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, specularAmount) == 48 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, specularScaleX) == 52 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, specularScaleY) == 56 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, fresnelRamp) == 60 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, shadowLevel) == 64 + matrix_pointer_delta);
static_assert(offsetof(GXSpecularFresnelParameters, lightingEnabled) == 68 + matrix_pointer_delta);

} // namespace

// Compile the genuine address carrier used by ShaderSkinMesh. The two whole
// original ShaderSkinMesh calls are compiled in the accompanying inventory.
void OriginalMaterialBufferAddressABI(glModelPacket* packet, const float (*matrix)[3][4])
{
    glSetMaterialBufferParameter(packet, 0xfb3b01ec, (glMaterialBufferAddress)matrix, 48);
}
