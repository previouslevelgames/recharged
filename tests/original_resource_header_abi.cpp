#include "NL/glx/glxTexture.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <stdexcept>

#if !defined(MSCHARGED_NATIVE) || !defined(TARGET_PC)
#error This fixture measures actual native source and canonical SDK declarations
#endif
// Independent Wii layout from the original member offsets, not the native SDK
// descriptor storage. Its pointer fields remain four-byte serialized words.
struct WiiTextureProjection
{
    std::uint16_t width,height;
    std::uint8_t levels,maxLevel,pad06[2];
    std::uint32_t format;
    std::int16_t paletteEntries;
    std::uint16_t textureIndex;
    std::uint8_t missing,pad11[3];
    std::uint32_t swizzled,linear,palette;
    std::uint8_t bits[4];
    std::uint32_t texobj[8],tlut[3];
};
static_assert(sizeof(WiiTextureProjection)==0x50);
static_assert(offsetof(WiiTextureProjection,swizzled)==0x14);
static_assert(offsetof(WiiTextureProjection,texobj)==0x24);
static_assert(offsetof(WiiTextureProjection,tlut)==0x44);
static_assert(sizeof(PlatTexture)==176);
static_assert(sizeof(PlatTexture::m_TexObj)==sizeof(GXTexObj));
static_assert(sizeof(PlatTexture::m_TlutObj)==sizeof(GXTlutObj));
static_assert(alignof(PlatTexture)>=alignof(void*));

int main()
{
    alignas(PlatTexture) std::array<unsigned char,sizeof(PlatTexture)> backing;
    auto* texture=::new (backing.data()) PlatTexture;
    // Observe only actual source-initialized members; no padding oracle.
    if (texture->m_Width || texture->m_Height || texture->m_Levels || texture->m_MaxLevel)
        throw std::runtime_error("Original texture dimensions initializer changed");
    if (texture->m_Format!=GXTex_Num || texture->m_nPaletteEntries || texture->GetTextureIndex()!=0xffff)
        throw std::runtime_error("Original texture format/index initializer changed");
    if (texture->m_SwizzledData || texture->m_LinearData || texture->m_PaletteData)
        throw std::runtime_error("Original texture pointer initializer changed");
    if (texture->m_NativeDataBytes || texture->m_NativePaletteBytes)
        throw std::runtime_error("Native storage facts initializer changed");
    auto* pointer=reinterpret_cast<void*>(std::uintptr_t(0x123456789ULL));
    texture->m_SwizzledData=pointer;
    if (texture->m_SwizzledData!=pointer)
        throw std::runtime_error("Native texture pointer carrier truncated");
    // The placement-created object has no source-owned payload; backing ends
    // its lifetime without importing the unqualified full texture destructor.
    std::puts("Original resource header ABI:5 declared-source checks; Wii80/native176 and canonical SDK capacities; no pool/render acceptance");
}
