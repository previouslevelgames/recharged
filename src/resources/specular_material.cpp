#include "resources/specular_material.h"
namespace mscharged::resources
{
void ValidateSpecularSkinMaterial(const SpecularSkinMaterial& value)
{
    for(const auto& binding:value.textures)
        Require(!(binding.flags&~3u),"Invalid authored Specular texture flags");
    const auto unit=[](float f){return std::isfinite(f)&&f>=0&&f<=1;};
    Require(unit(value.blend)&&unit(value.alpha)&&unit(value.specular_level),"Invalid Specular blend/alpha/level");
    Require(std::isfinite(value.specular_exponent)&&value.specular_exponent>=0&&value.specular_exponent<=1e4f,
        "Specular exponent exceeds the qualified original light input domain");
    for(float component:value.specular_colour)Require(unit(component),"Invalid Specular colour component");
    Require(value.lighting_enabled<=1,"Invalid authored Specular lighting flag");
}
SpecularSkinMaterial ReadSpecularSkinMaterial(Bytes record)
{
    Require(record.size()==72,"Specular material must contain its exact72 authored bytes");
    SpecularSkinMaterial result;
    for(unsigned i=0;i<3;++i)
    {
        // Saved textureIndex is a runtime cache. Its serialized value does not
        // identify a current host slot; native installation resets it toFFFF.
        Require(!(record[i*8+6]&~3u)&&record[i*8+7]==0,"Invalid authored Specular binding bytes");
        result.textures[i]={U32(record,i*8),record[i*8+6]};
    }
    result.blend=F32(record,32);result.alpha=F32(record,36);
    result.specular_level=F32(record,40);result.specular_exponent=F32(record,44);
    for(unsigned i=0;i<4;++i)result.specular_colour[i]=F32(record,48+i*4);
    result.shadow_level=U32(record,64);result.lighting_enabled=U32(record,68);
    ValidateSpecularSkinMaterial(result);return result;
}
}
