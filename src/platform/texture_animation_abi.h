#pragma once

#include "Game/GL/GLTextureAnim.h"
#include <cstddef>
#include <cstring>
#include <cstdint>

namespace mscharged::platform {
inline constexpr std::size_t WiiTextureAnimHeaderSize = 36;
inline constexpr std::size_t WiiTextureAnimFrameSize = 8;

inline u32 ReadTextureAnimWord(const void* source)
{
    const auto* bytes = static_cast<const u8*>(source);
    return (u32(bytes[0]) << 24) | (u32(bytes[1]) << 16)
        | (u32(bytes[2]) << 8) | u32(bytes[3]);
}

inline f32 ReadTextureAnimFloat(const void* source)
{
    const u32 bits = ReadTextureAnimWord(source);
    f32 result;
    static_assert(sizeof(result) == sizeof(bits));
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

inline s32 ReadTextureAnimSignedWord(const void* source)
{
    const u32 bits = ReadTextureAnimWord(source);
    s32 result;
    static_assert(sizeof(result) == sizeof(bits));
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// Expand the Wii record into pointer-width native object storage. Selection,
// allocations, frame resolution, registration and updates remain in the TU.
inline void ExpandTextureAnimHeader(GLTextureAnim& target, const void* source)
{
    const auto* bytes = static_cast<const u8*>(source);
    target.m_nFrame = ReadTextureAnimSignedWord(bytes);
    target.m_uHashID = ReadTextureAnimWord(bytes + 4);
    target.m_nNumTextures = ReadTextureAnimSignedWord(bytes + 8);
#if defined(MSCHARGED_DIAGNOSTIC_TEXTURES)
    target.m_NativeFrameCount = static_cast<u32>(target.m_nNumTextures);
#endif
    target.m_ePlayMode = static_cast<eGLTexAnimMode>(ReadTextureAnimWord(bytes + 12));
    target.m_nPlayDir = ReadTextureAnimSignedWord(bytes + 16);
    target.m_bPaused = bytes[20];
    std::memcpy(target.m_pad15, bytes + 21, sizeof(target.m_pad15));
    target.m_textureIndex = ReadTextureAnimWord(bytes + 24);
    target.m_fTime = ReadTextureAnimFloat(bytes + 28);
    target.m_pAnimTex = reinterpret_cast<GLAnimTex*>(
        static_cast<std::uintptr_t>(ReadTextureAnimWord(bytes + 32)));
}

inline void ExpandTextureAnimFrame(GLAnimTex& target, const void* source)
{
    const auto* bytes = static_cast<const u8*>(source);
    target.m_TexHandle = ReadTextureAnimWord(bytes);
    target.m_fTime = ReadTextureAnimFloat(bytes + 4);
}
} // namespace mscharged::platform
