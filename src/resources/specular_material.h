#pragma once
#include "resources/skin_model.h"
namespace mscharged::resources
{
// Authored Wii Specular0x22cadb20 fields, decoded independently of host pointers.
// Serialized SkinMatrices bytes24..31 are runtime scratch, never file offsets.
struct SpecularSkinMaterial
{
    std::array<SkinTextureBinding,3> textures;
    float blend=0,alpha=1,specular_level=0,specular_exponent=0;
    std::array<float,4> specular_colour{};
    std::uint32_t shadow_level=0,lighting_enabled=0;
};
SpecularSkinMaterial ReadSpecularSkinMaterial(Bytes exact_wii_record);
void ValidateSpecularSkinMaterial(const SpecularSkinMaterial&);
}
