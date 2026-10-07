#include "runtime/specular_material.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/glx/GXSpecularMaterialProgram.h"
#include "NL/glx/glxSkinMatrix.h"
#include <dolphin/gx.h>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace mscharged
{
namespace
{
constexpr u32 hash=0x22cadb20;
struct SoftwareView { const glModelPacket* packet;SpecularSoftwareSkin arrays; };
thread_local std::optional<SoftwareView> software;
void Require(bool ok,const char* message){if(!ok)throw std::invalid_argument(message);}
void Component(float value){Require(std::isfinite(value)&&std::abs(value)<=1e7f,"Invalid Specular vertex/matrix component");}
}
void InstallSpecularMaterial(glModelPacket& packet,const resources::SpecularSkinMaterial& source,GXSpecularParameters& storage)
{
    resources::ValidateSpecularSkinMaterial(source);
    auto* program=static_cast<GLMaterialProgram*>(glGetMaterialProgram(hash));
    if(!program||program!=GXSpecularMaterialProgram::Instance||program->programHash!=hash
        ||program->parameterDataSize!=sizeof(GXSpecularParameters)||program->parameterCount!=11)
        throw std::logic_error("Genuine original Specular material program is unavailable");
    GXSpecularParameters replacement{};
    glTextureBinding* bindings[]{&replacement.diffuseTexture,&replacement.detailTexture,&replacement.glossTexture};
    for(unsigned i=0;i<3;++i)
        *bindings[i]={source.textures[i].hash,u8(source.textures[i].flags&1),u8((source.textures[i].flags>>1)&1)};
    replacement.skinMatrices=nullptr;replacement.skinMatrixBytes=0;
    replacement.blendAmount=source.blend;replacement.alphaValue=source.alpha;
    replacement.specularLevel=source.specular_level;replacement.specularExponent=source.specular_exponent;
    for(unsigned i=0;i<4;++i)replacement.specularColour.c[i]=source.specular_colour[i];
    replacement.shadowLevel=source.shadow_level;replacement.lightingEnabled=source.lighting_enabled;
    storage=replacement;packet.materialProgram=program;packet.materialParameters=&storage;
    program->Configure(&packet);
}
bool SpecularUsesSoftwareSkin(const glModelPacket& packet)
{
    if(software&&software->packet!=&packet)throw std::logic_error("Specular software pose belongs to another packet");
    return software.has_value();
}
const void* SpecularVertexArray(const glModelPacket& packet,unsigned stream)
{
    Require(stream<5&&packet.streams,"Invalid Specular GX array ordinal");
    if(SpecularUsesSoftwareSkin(packet)&&stream<2)
        return stream?static_cast<const void*>(software->arrays.normals.data()):software->arrays.positions.data();
    return packet.streams[stream].address;
}
void ValidateNativeSpecularPacket(const glModelPacket& packet)
{
    Require(packet.streams&&packet.numStreams==7&&packet.indexBuffer&&packet.numUniqueVertices
        &&packet.numVertices&&packet.numVertices<=65535&&packet.primType>=0&&packet.primType<=3
        &&packet.materialParameters&&!packet.displayList&&!packet.skinnedVertices&&!packet.skinnedNormals,
        "Invalid native Specular packet or truncated console skin pointer");
    auto* program=static_cast<GLMaterialProgram*>(packet.materialProgram);
    Require(program&&program==glGetMaterialProgram(hash)&&program==GXSpecularMaterialProgram::Instance
        &&program->programHash==hash&&program->parameterDataSize==sizeof(GXSpecularParameters),"Specular packet program is not its genuine registration");
    Require((packet.primType!=0||packet.numVertices%3==0)&&(packet.primType!=3||packet.numVertices%4==0)
        &&((packet.primType!=1&&packet.primType!=2)||packet.numVertices>=3),"Invalid Specular primitive count");
    const auto& p=*static_cast<const GXSpecularParameters*>(packet.materialParameters);
    resources::SpecularSkinMaterial material;
    material.textures={resources::SkinTextureBinding{p.diffuseTexture.texture,p.diffuseTexture.flags},
        {p.detailTexture.texture,p.detailTexture.flags},{p.glossTexture.texture,p.glossTexture.flags}};
    material.blend=p.blendAmount;material.alpha=p.alphaValue;material.specular_level=p.specularLevel;
    material.specular_exponent=p.specularExponent;material.shadow_level=p.shadowLevel;material.lighting_enabled=p.lightingEnabled;
    for(unsigned i=0;i<4;++i)material.specular_colour[i]=p.specularColour.c[i];resources::ValidateSpecularSkinMaterial(material);
    const unsigned bones=p.skinMatrixBytes/sizeof(*p.skinMatrices);
    Require(p.skinMatrices&&p.skinMatrixBytes%sizeof(*p.skinMatrices)==0&&bones&&bones<=9,
        "Specular draw needs genuine retained native skin matrices");
    for(unsigned b=0;b<bones;++b)for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)Component(p.skinMatrices[b][r][c]);
    constexpr unsigned ids[]{1,2,4,4,4,7,5},strides[]{12,12,4,4,4,4,16};
    for(unsigned s=0;s<7;++s)Require(packet.streams[s].address&&packet.streams[s].id==ids[s]
        &&packet.streams[s].stride==strides[s]&&packet.streams[s].unknown07==0,"Invalid Specular authored vertex stream");
    const bool posed=SpecularUsesSoftwareSkin(packet);
    const auto* slots=static_cast<const u8*>(packet.streams[5].address);
    const auto* weights=static_cast<const float*>(packet.streams[6].address);
    for(unsigned v=0;v<packet.numUniqueVertices;++v)
    {
        float sum=0;unsigned active=0;bool zero=false;unsigned seen=0;
        for(unsigned lane=0;lane<4;++lane)
        {
            const float w=weights[v*4+lane];Require(std::isfinite(w)&&w>=0&&w<=1,"Invalid Specular bone weight");
            Require(lane==0||w<=weights[v*4],"Specular weights lack original largest-first preparation");
            if(w)
            {
                Require(!zero,"Specular active weight follows the source gather terminator");
                Require(slots[v*4+lane]<bones,"Specular active bone exceeds its real palette");
                const auto bit=1u<<slots[v*4+lane];Require(!(seen&bit),"Specular vertex repeats an active bone");seen|=bit;++active;
            }
            else zero=true;
            sum+=w;
        }
        Require(active&&std::abs(sum-1.f)<=1e-5f,"Invalid Specular authored weight sum");
        if(!posed)Require(weights[v*4]==1.f&&active==1,"Fractional Specular draw needs genuine software-posed arrays");
        for(unsigned s=0;s<2;++s)
        {
            const auto* data=static_cast<const float*>(SpecularVertexArray(packet,s));
            for(unsigned c=0;c<3;++c)Component(data[v*3+c]);
        }
    }
    for(unsigned i=0;i<packet.numVertices;++i)Require(packet.indexBuffer[i]<packet.numUniqueVertices,"Specular index exceeds retained streams");
}
SpecularSoftwareSkinScope::SpecularSoftwareSkinScope(const glModelPacket& packet,SpecularSoftwareSkin arrays)
{
    if(software)throw std::logic_error("Nested Specular software pose scope");
    Require(arrays.positions.size()==packet.numUniqueVertices&&arrays.normals.size()==packet.numUniqueVertices
        &&!arrays.positions.empty(),"Software Specular pose arrays do not match actual vertices");
    software=SoftwareView{&packet,arrays};
    try{ValidateNativeSpecularPacket(packet);packet_=&packet;}catch(...){software.reset();throw;}
}
SpecularSoftwareSkinScope::~SpecularSoftwareSkinScope()
{
    if(packet_)software.reset();
}
void DrawNativeSpecularPacket(const glModelPacket& packet)
{
    ValidateNativeSpecularPacket(packet);
    const GXPrimitive primitives[]{GX_TRIANGLES,GX_TRIANGLESTRIP,GX_TRIANGLEFAN,GX_QUADS};
    const auto* bones=static_cast<const u8*>(packet.streams[5].address);
    GXBegin(primitives[unsigned(packet.primType)],GX_VTXFMT0,packet.numVertices);
    for(unsigned i=0;i<packet.numVertices;++i)
    {
        const auto vertex=packet.indexBuffer[i];
        // Exact dlMakeDisplayList order: direct stitch slot, then position,
        // normal and three authored signed16 UV stream indices.
        GXParam1u8(static_cast<u8>(glx_SkinMatrixSlots[bones[vertex*4]]));
        GXPosition1x16(vertex);GXNormal1x16(vertex);
        GXTexCoord1x16(vertex);GXTexCoord1x16(vertex);GXTexCoord1x16(vertex);
    }
    GXEnd();
}
}
