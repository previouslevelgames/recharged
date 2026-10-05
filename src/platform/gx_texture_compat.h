#pragma once

#include <dolphin/gx/GXTexture.h>

// Reconstructed platform calls use Wii SDK numeric/opaque signatures. Forward
// those calls to the canonical native SDK types without changing game choices.
inline void GXInitTlutObj(void* object, void* palette, int format, u16 entries)
{
    GXInitTlutObj(static_cast<GXTlutObj*>(object), static_cast<const void*>(palette),
        static_cast<GXTlutFmt>(format), entries);
}
inline void GXInitTexObjCI(void* object, void* image, u16 width, u16 height,
    int format, int wrap_s, int wrap_t, u8 mipmap, unsigned long tlut)
{
    GXInitTexObjCI(static_cast<GXTexObj*>(object), static_cast<const void*>(image), width, height,
        static_cast<GXCITexFmt>(format), static_cast<GXTexWrapMode>(wrap_s),
        static_cast<GXTexWrapMode>(wrap_t), static_cast<GXBool>(mipmap),
        static_cast<u32>(tlut));
}
inline void GXInitTexObj(void* object, void* image, u16 width, u16 height,
    int format, int wrap_s, int wrap_t, u8 mipmap)
{
    GXInitTexObj(static_cast<GXTexObj*>(object), static_cast<const void*>(image), width, height,
        static_cast<GXTexFmt>(format), static_cast<GXTexWrapMode>(wrap_s),
        static_cast<GXTexWrapMode>(wrap_t), static_cast<GXBool>(mipmap));
}
inline void GXInitTexObjLOD(void* object, int min_filter, int mag_filter,
    float min_lod, float max_lod, float bias, u8 clamp, u8 edge, int anisotropy)
{
    GXInitTexObjLOD(static_cast<GXTexObj*>(object),
        static_cast<GXTexFilter>(min_filter), static_cast<GXTexFilter>(mag_filter),
        min_lod, max_lod, bias, static_cast<GXBool>(clamp),
        static_cast<GXBool>(edge), static_cast<GXAnisotropy>(anisotropy));
}
inline void GXInitTexObjWrapMode(void* object, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t)
{ GXInitTexObjWrapMode(static_cast<GXTexObj*>(object), wrap_s, wrap_t); }
inline void GXInitTexObjTlut(void* object, unsigned long tlut)
{ GXInitTexObjTlut(static_cast<GXTexObj*>(object), static_cast<u32>(tlut)); }
inline void GXLoadTlut(void* object, unsigned long tlut)
{ GXLoadTlut(static_cast<GXTlutObj*>(object), static_cast<u32>(tlut)); }
inline void GXLoadTexObj(void* object, unsigned long texture_map)
{ GXLoadTexObj(static_cast<GXTexObj*>(object), static_cast<GXTexMapID>(texture_map)); }
