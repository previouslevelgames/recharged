#pragma once
#include "resources/specular_material.h"
#include "NL/gl/glModel.h"
#include <span>
struct GXSpecularParameters;
namespace mscharged
{
// Storage belongs to the caller's real pool/frame owner. Installation checks
// the genuine registered program, preserves lazy authored texture bindings and
// leaves pose pointer/size unavailable. Prepare/draw admission remains separate.
void InstallSpecularMaterial(glModelPacket&,const resources::SpecularSkinMaterial&,GXSpecularParameters&);
void ValidateNativeSpecularPacket(const glModelPacket&);
void DrawNativeSpecularPacket(const glModelPacket&);

// Explicit native software-skin storage. These are genuine already-posed host
// arrays retained by the calling pose/frame owner, never Wii u32 pointer fields.
// The scoped admission selects the source default-matrix branch and overrides
// only position/normal GX arrays. UV/bone/weight streams stay authored. Scope
// lifetime covers actual dispatch, after which the owner must drain its frame.
struct SpecularSoftwareSkin
{
    std::span<const std::array<float,3>> positions,normals;
};
class SpecularSoftwareSkinScope
{
    const glModelPacket* packet_=nullptr;
public:
    SpecularSoftwareSkinScope(const glModelPacket&,SpecularSoftwareSkin);
    ~SpecularSoftwareSkinScope();
    SpecularSoftwareSkinScope(const SpecularSoftwareSkinScope&)=delete;
    SpecularSoftwareSkinScope& operator=(const SpecularSoftwareSkinScope&)=delete;
};
bool SpecularUsesSoftwareSkin(const glModelPacket&);
const void* SpecularVertexArray(const glModelPacket&,unsigned stream);
}
