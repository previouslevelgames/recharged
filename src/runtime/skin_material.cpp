#include "runtime/skin_material.h"
#include "runtime/material_environment.h"
#include "NL/glx/GXCharacterSkinCustomMaterialProgram.h"
#include "NL/glx/glxSkinMatrix.h"
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mscharged
{
void ValidateNativeSkinPacket(const glModelPacket& packet)
{
    if(!packet.streams||packet.numStreams!=6||!packet.indexBuffer||!packet.numUniqueVertices
        ||!packet.numVertices||packet.numVertices>65535||packet.primType<0||packet.primType>3
        ||!packet.materialParameters||packet.skinnedVertices||packet.skinnedNormals)
        throw std::invalid_argument("Invalid or nonrigid native skin packet");
    if((packet.primType==0&&packet.numVertices%3)||(packet.primType==3&&packet.numVertices%4)
        ||((packet.primType==1||packet.primType==2)&&packet.numVertices<3))
        throw std::invalid_argument("Invalid native skin primitive count");
    const auto& material=*static_cast<const GXCharacterSkinCustomParameters*>(packet.materialParameters);
    const auto bones=material.skinMatrixBytes/48;
    if(!material.skinMatrices||material.skinMatrixBytes%48||!bones||bones>9
        ||material.lightingEnabled<0||material.lightingEnabled>1
        ||!std::isfinite(material.blendAmount)||material.blendAmount<0||material.blendAmount>1
        ||!std::isfinite(material.alphaValue)||material.alphaValue<0||material.alphaValue>1)
        throw std::invalid_argument("Invalid native skin material/matrix storage");
    constexpr unsigned ids[]{1,2,4,4,7,5},strides[]{12,12,4,4,4,16};
    for(unsigned s=0;s<6;++s)
        if(!packet.streams[s].address||packet.streams[s].id!=ids[s]||packet.streams[s].stride!=strides[s])
            throw std::invalid_argument("Invalid native skin vertex streams");
    const auto* slots=static_cast<const unsigned char*>(packet.streams[4].address);
    const auto* weights=static_cast<const float*>(packet.streams[5].address);
    for(unsigned v=0;v<packet.numUniqueVertices;++v)
        if(slots[v*4]>=bones||weights[v*4]!=1||weights[v*4+1]!=0||weights[v*4+2]!=0||weights[v*4+3]!=0)
            throw std::invalid_argument("Unqualified native skin bone influence");
    for(unsigned i=0;i<packet.numVertices;++i)
        if(packet.indexBuffer[i]>=packet.numUniqueVertices)throw std::out_of_range("Skin index exceeds retained vertex arrays");
    for(unsigned b=0;b<bones;++b)for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
        if(!std::isfinite(material.skinMatrices[b][r][c])||std::abs(material.skinMatrices[b][r][c])>1e12f)
            throw std::invalid_argument("Invalid native skin matrix component");
}
void DrawNativeSkinPacket(const glModelPacket& packet)
{
    // Same command order as dlMakeDisplayList: direct stitch/matrix slot,
    // then four indexed streams (position, normal, UV0, UV1) for every index.
    ValidateNativeSkinPacket(packet);
    const GXPrimitive primitives[]{GX_TRIANGLES,GX_TRIANGLESTRIP,GX_TRIANGLEFAN,GX_QUADS};
    const auto* bones=static_cast<const unsigned char*>(packet.streams[4].address);
    GXBegin(primitives[unsigned(packet.primType)],GX_VTXFMT0,packet.numVertices);
    for(unsigned i=0;i<packet.numVertices;++i)
    {
        const auto vertex=packet.indexBuffer[i];
        // PNMTXIDX is the first direct byte in the configured vertex format.
        // Aurora exposes the real FIFO parameter writer for that byte.
        GXParam1u8(static_cast<u8>(glx_SkinMatrixSlots[bones[vertex*4]]));
        GXPosition1x16(vertex);GXNormal1x16(vertex);GXTexCoord1x16(vertex);GXTexCoord1x16(vertex);
    }
    GXEnd();
}
void SkinNormalMatrix(const float (&input)[3][4],float (&output)[3][4])
{
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
    {
        if(!std::isfinite(input[r][c])||std::abs(input[r][c])>1e15f)
            throw std::invalid_argument("Invalid composed skin matrix");
    }
    // Bound the six determinant terms before the original float routine. An
    // overflowing determinant otherwise returns success with an all-zero
    // inverse. The original arithmetic still produces every output component.
    double sum=0;
    constexpr unsigned columns[6][3]{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    for(const auto& c:columns)sum+=std::abs(double(input[0][c[0]])*input[1][c[1]]*input[2][c[2]]);
    if(sum>std::numeric_limits<float>::max())
        throw std::invalid_argument("Composed skin normal determinant would overflow");
    if(!PSMTXInvXpose(input,output))
        throw std::invalid_argument("Singular composed skin normal matrix");
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
        if(!std::isfinite(output[r][c]))throw std::invalid_argument("Invalid inverse-transpose skin normal matrix");
}
}
