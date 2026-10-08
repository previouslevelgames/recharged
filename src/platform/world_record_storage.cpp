#include "platform/world_record_storage.h"
#include "platform/game_allocation_ownership.h"

#include "Game/Render/StadiumWorldObjects.h"
#include "Game/Render/StadiumPhysicsObject.h"
#include "Game/Render/SolarFlareEffect.h"
#include "Game/Render/CrowdImpostorManager.h"
#include "Game/Render/WorldNPC.h"
#include "Game/World/WorldEffect.h"
#include "Game/World/worldanim.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace mscharged::platform {
namespace {
constexpr std::uint64_t Magic = 0x574f524c44564945ULL;
struct alignas(8) Header {
    std::uint64_t magic;
    WorldRecordStorageLayout layout;
    std::uint32_t native_bytes;
    std::uint32_t wire_bytes;
};
static_assert(sizeof(Header) % 8 == 0);
static_assert(sizeof(float) == 4 && sizeof(int) == 4);
static_assert(sizeof(void*) == 8 && sizeof(unsigned long) == 8);

struct Layout {
    std::size_t wire_bytes;
    std::size_t native_bytes;
    std::size_t alignment;
};
Layout Describe(WorldRecordStorageLayout kind) {
    switch (kind) {
    case WorldRecordStorageLayout::StadiumDrawable:
        return {0x90, sizeof(StadiumWorldDrawable), alignof(StadiumWorldDrawable)};
    case WorldRecordStorageLayout::StadiumHighRange:
        return {0x90, sizeof(StadiumHighRangeDrawable), alignof(StadiumHighRangeDrawable)};
    case WorldRecordStorageLayout::StadiumMarker:
        return {0x70, sizeof(StadiumFEModelMarker), alignof(StadiumFEModelMarker)};
    case WorldRecordStorageLayout::Effect:
        return {0xA0, sizeof(WorldEffect), alignof(WorldEffect)};
    case WorldRecordStorageLayout::CupTrophy:
        return {0x80, sizeof(StadiumCupTrophyDrawable), alignof(StadiumCupTrophyDrawable)};
    case WorldRecordStorageLayout::Light:
        return {0x90, sizeof(StadiumLight), alignof(StadiumLight)};
    case WorldRecordStorageLayout::Animation:
        return {0x90, sizeof(WorldAnimObject), alignof(WorldAnimObject)};
    case WorldRecordStorageLayout::Drawable:
        return {0x70, sizeof(WorldDrawable), alignof(WorldDrawable)};
    case WorldRecordStorageLayout::Visibility:
        return {0x30, sizeof(WorldVisibilityDrawable), alignof(WorldVisibilityDrawable)};
    case WorldRecordStorageLayout::Physics:
        return {0x90, sizeof(WorldPhysicsDrawable), alignof(WorldPhysicsDrawable)};
    case WorldRecordStorageLayout::CommonObject:
        return {0x60, sizeof(WorldObject_80129EE0), alignof(WorldObject_80129EE0)};
    case WorldRecordStorageLayout::Crowd:
        return {0x80, sizeof(CrowdLayoutObject), alignof(CrowdLayoutObject)};
    case WorldRecordStorageLayout::NPC:
        return {0x70, sizeof(WorldNPC), alignof(WorldNPC)};
    case WorldRecordStorageLayout::StadiumPhysics:
        return {0x90, sizeof(StadiumPhysicsObject), alignof(StadiumPhysicsObject)};
    case WorldRecordStorageLayout::AttackSide:
        return {0x80, sizeof(StadiumAttackSideIndicator), alignof(StadiumAttackSideIndicator)};
    case WorldRecordStorageLayout::ConditionalDrawable:
        return {0x80, sizeof(SolarFlareDrawable), alignof(SolarFlareDrawable)};
    case WorldRecordStorageLayout::ShadowHeight:
        return {0x70, sizeof(StadiumShadowHeightMarker), alignof(StadiumShadowHeightMarker)};
    case WorldRecordStorageLayout::Toggle:
        return {0x80, sizeof(StadiumToggleDrawable), alignof(StadiumToggleDrawable)};
    case WorldRecordStorageLayout::ShadowVolume:
        return {0x80, sizeof(StadiumShadowVolumeDrawable), alignof(StadiumShadowVolumeDrawable)};
    }
    throw std::invalid_argument("World native record layout is unsupported");
}

std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
        | std::uint32_t(p[2]) << 8 | std::uint32_t(p[3]);
}
void Word4(unsigned char* dst, std::size_t native, const unsigned char* src,
           std::size_t wire, std::size_t count = 1) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto word = Word(src + wire + 4 * i);
        std::memcpy(dst + native + 4 * i, &word, 4);
    }
}
void Word8(unsigned char* dst, std::size_t native, const unsigned char* src,
           std::size_t wire) {
    const std::uintptr_t word = Word(src + wire);
    std::memcpy(dst + native, &word, sizeof(word));
}
void Bytes(unsigned char* dst, std::size_t native, const unsigned char* src,
           std::size_t wire, std::size_t count) {
    std::memcpy(dst + native, src + wire, count);
}

// Source World::LoadObjects subsequently aliases these seven prefix members
// through DrawableObject, even for the common non-drawable world base. All
// accepted class headers have these measured compatible native offsets.
void Prefix(unsigned char* dst, const unsigned char* src) {
    Word4(dst, offsetof(WorldDrawable, m_uHashID), src, 4, 3);
    Word8(dst, offsetof(WorldDrawable, m_pWorldContext), src, 0x10);
    Word4(dst, offsetof(WorldDrawable, m_nAnimNode), src, 0x14);
    Word8(dst, offsetof(WorldDrawable, m_pAnimController), src, 0x18);
    Bytes(dst, offsetof(WorldDrawable, mUnidentified1C), src, 0x1C, 4);
    Word4(dst, offsetof(WorldDrawable, mWorldMatrix), src, 0x20, 16);
}
void Drawable(unsigned char* dst, const unsigned char* src) {
    Prefix(dst, src);
    Word4(dst, offsetof(WorldDrawable, m_fBoundingRadius), src, 0x60);
    Word8(dst, offsetof(WorldDrawable, m_pModel), src, 0x64);
    Bytes(dst, offsetof(WorldDrawable, mUnidentified68), src, 0x68, 8);
}

// These original classes expose their leading numeric words through the
// source's DrawableObject casts, but retain explicitly opaque prefix storage.
// Copy that declared storage, then lower only the three consumed Wii words.
void OpaquePrefix(unsigned char* dst, std::size_t offset,
                  const unsigned char* src, std::size_t bytes) {
    Bytes(dst, offset, src, 4, bytes);
    Word4(dst, offsetof(WorldDrawable, m_uHashID), src, 4, 3);
}

void Physics(unsigned char* dst, const unsigned char* src) {
    OpaquePrefix(dst, offsetof(WorldPhysicsDrawable, m_pad04), src, 8);
    Word8(dst, offsetof(WorldPhysicsDrawable, m_uObjectCreationFlags), src, 0x0C);
    Bytes(dst, offsetof(WorldPhysicsDrawable, m_pad10), src, 0x10, 0x10);
    const auto description = offsetof(WorldPhysicsDrawable, m_Description);
    Word4(dst, description + offsetof(WorldPhysicsDescription, matLocalToParent), src, 0x20, 16);
    Word8(dst, description + offsetof(WorldPhysicsDescription, uPrimitiveType), src, 0x60);
    Word4(dst, description + offsetof(WorldPhysicsDescription, fWidth), src, 0x64, 4);
    Bytes(dst, offsetof(WorldPhysicsDrawable, m_pad74), src, 0x74, 12);
    Word8(dst, offsetof(WorldPhysicsDrawable, m_pPhysicsObject), src, 0x80);
    Bytes(dst, offsetof(WorldPhysicsDrawable, m_pad84), src, 0x84, 12);
}

#define OFFSET_EQ(A, F, B, G) static_assert(offsetof(A, F) == offsetof(B, G))
OFFSET_EQ(WorldDrawable, m_uHashID, WorldObject_80129EE0, mUnidentified004);
OFFSET_EQ(WorldDrawable, m_pWorldContext, WorldObject_80129EE0, m_pWorld);
OFFSET_EQ(WorldDrawable, m_nAnimNode, WorldObject_80129EE0, m_nAnimNode);
OFFSET_EQ(WorldDrawable, m_pAnimController, WorldObject_80129EE0, m_pAnimController);
OFFSET_EQ(WorldDrawable, mUnidentified1C, WorldObject_80129EE0, mUnidentified01C);
OFFSET_EQ(WorldDrawable, mWorldMatrix, WorldObject_80129EE0, mWorldMatrix);
OFFSET_EQ(WorldDrawable, m_uHashID, WorldAnimObject, m_uHashID);
OFFSET_EQ(WorldDrawable, m_uRenderLayer, WorldAnimObject, m_pad08);
OFFSET_EQ(WorldDrawable, m_pWorldContext, WorldAnimObject, m_pWorld);
OFFSET_EQ(WorldDrawable, m_nAnimNode, WorldAnimObject, m_nAnimNode);
OFFSET_EQ(WorldDrawable, m_pAnimController, WorldAnimObject, m_pAnimController);
OFFSET_EQ(WorldDrawable, mUnidentified1C, WorldAnimObject, m_pad1C);
OFFSET_EQ(WorldDrawable, mWorldMatrix, WorldAnimObject, mWorldMatrix);
static_assert(alignof(WorldObject_80129EE0) == 8);
#undef OFFSET_EQ

void Decode(WorldRecordStorageLayout kind, unsigned char* dst, const unsigned char* src) {
    switch (kind) {
    case WorldRecordStorageLayout::Drawable:
        Drawable(dst, src);
        return;
    case WorldRecordStorageLayout::CommonObject:
        Prefix(dst, src);
        return;
    case WorldRecordStorageLayout::Visibility:
        OpaquePrefix(dst, offsetof(WorldVisibilityDrawable, m_pad04), src, 12);
        Word8(dst, offsetof(WorldVisibilityDrawable, m_pWorld), src, 0x10);
        Bytes(dst, offsetof(WorldVisibilityDrawable, m_pad14), src, 0x14, 12);
        Word8(dst, offsetof(WorldVisibilityDrawable, m_pModel), src, 0x20);
        Word8(dst, offsetof(WorldVisibilityDrawable, m_pVisibilityNode), src, 0x24);
        Bytes(dst, offsetof(WorldVisibilityDrawable, m_pad28), src, 0x28, 8);
        return;
    case WorldRecordStorageLayout::Physics:
    case WorldRecordStorageLayout::StadiumPhysics:
        Physics(dst, src);
        return;
    case WorldRecordStorageLayout::Crowd:
        OpaquePrefix(dst, offsetof(CrowdLayoutObject, mUnidentified004), src, 28);
        Word4(dst, offsetof(CrowdLayoutObject, mTransform), src, 0x20, 16);
        Word4(dst, offsetof(CrowdLayoutObject, mStartWidth), src, 0x60, 5);
        Bytes(dst, offsetof(CrowdLayoutObject, mUnidentified074), src, 0x74, 12);
        return;
    case WorldRecordStorageLayout::NPC:
        OpaquePrefix(dst, offsetof(WorldNPC, mUnidentified004), src, 28);
        Word4(dst, offsetof(WorldNPC, mTransform), src, 0x20, 16);
        Word8(dst, offsetof(WorldNPC, mTemplateHash), src, 0x60);
        Bytes(dst, offsetof(WorldNPC, mUnidentified064), src, 0x64, 12);
        return;
    case WorldRecordStorageLayout::StadiumDrawable:
    case WorldRecordStorageLayout::StadiumHighRange:
        Drawable(dst, src);
        Word4(dst, offsetof(StadiumWorldDrawable, m_boundsMin), src, 0x70, 3);
        Word4(dst, offsetof(StadiumWorldDrawable, m_boundsMax), src, 0x7C, 3);
        Word8(dst, offsetof(StadiumWorldDrawable, m_uFlags), src, 0x88);
        Word4(dst, offsetof(StadiumWorldDrawable, m_fBlend), src, 0x8C);
        return;
    case WorldRecordStorageLayout::StadiumMarker:
        Prefix(dst, src);
        Word4(dst, offsetof(StadiumFEModelMarker, mMarkerID), src, 0x60);
        Bytes(dst, offsetof(StadiumFEModelMarker, mUnidentified064), src, 0x64, 12);
        return;
    case WorldRecordStorageLayout::ShadowHeight:
        Prefix(dst, src);
        Bytes(dst, offsetof(StadiumShadowHeightMarker, mUnidentified060), src, 0x60, 16);
        return;
    case WorldRecordStorageLayout::AttackSide:
        Drawable(dst, src);
        Word4(dst, offsetof(StadiumAttackSideIndicator, m_nIndex), src, 0x70, 2);
        Bytes(dst, offsetof(StadiumAttackSideIndicator, mUnidentified78), src, 0x78, 8);
        return;
    case WorldRecordStorageLayout::ConditionalDrawable:
        Drawable(dst, src);
        Word8(dst, offsetof(SolarFlareDrawable, m_uDrawEnabled), src, 0x70);
        return;
    case WorldRecordStorageLayout::Toggle:
        Drawable(dst, src);
        Word4(dst, offsetof(StadiumToggleDrawable, m_nVisible), src, 0x70);
        Bytes(dst, offsetof(StadiumToggleDrawable, mUnidentified74), src, 0x74, 12);
        return;
    case WorldRecordStorageLayout::ShadowVolume:
        Drawable(dst, src);
        Word8(dst, offsetof(StadiumShadowVolumeDrawable, m_pShadowModels), src, 0x70);
        Word8(dst, offsetof(StadiumShadowVolumeDrawable, m_pShadowModels) + sizeof(glModel*), src, 0x74);
        Bytes(dst, offsetof(StadiumShadowVolumeDrawable, mUnidentified78), src, 0x78, 8);
        return;
    case WorldRecordStorageLayout::Effect:
        Prefix(dst, src);
        Word4(dst, offsetof(WorldEffect, m_fEmissionInterval), src, 0x60, 3);
        Bytes(dst, offsetof(WorldEffect, m_pad6C), src, 0x6C, 4);
        Word4(dst, offsetof(WorldEffect, m_uEffectHash), src, 0x70, 6);
        Bytes(dst, offsetof(WorldEffect, m_pad88), src, 0x88, 8);
        Word4(dst, offsetof(WorldEffect, m_nRemainingEmissions), src, 0x90, 3);
        Bytes(dst, offsetof(WorldEffect, m_bAlwaysVisible), src, 0x9C, 1);
        return;
    case WorldRecordStorageLayout::CupTrophy:
        Drawable(dst, src);
        Word8(dst, offsetof(StadiumCupTrophyDrawable, m_uCupTrophyKey), src, 0x70);
        Word4(dst, offsetof(StadiumCupTrophyDrawable, m_fCupTrophyOpacity), src, 0x74);
        Bytes(dst, offsetof(StadiumCupTrophyDrawable, mUnidentified78), src, 0x78, 8);
        return;
    case WorldRecordStorageLayout::Light:
        Prefix(dst, src);
        Bytes(dst, offsetof(StadiumLight, mUnidentified60), src, 0x60, 4);
        Word4(dst, offsetof(StadiumLight, m_fIntensity), src, 0x64);
        Bytes(dst, offsetof(StadiumLight, mUnidentified68), src, 0x68, 8);
        Word4(dst, offsetof(StadiumLight, m_colour), src, 0x70, 4);
        Bytes(dst, offsetof(StadiumLight, mUnidentified80), src, 0x80, 16);
        return;
    case WorldRecordStorageLayout::Animation:
        Prefix(dst, src);
        Word4(dst, offsetof(WorldAnimObject, m_nBindings), src, 0x60, 2);
        Word8(dst, offsetof(WorldAnimObject, m_pBindings), src, 0x68);
        Bytes(dst, offsetof(WorldAnimObject, m_pad6C), src, 0x6C, 4);
        Word4(dst, offsetof(WorldAnimObject, m_nAnimations), src, 0x70);
        Word8(dst, offsetof(WorldAnimObject, m_pAnimationHashes), src, 0x74);
        Bytes(dst, offsetof(WorldAnimObject, m_pad78), src, 0x78, 8);
        Word4(dst, offsetof(WorldAnimObject, m_ePlayMode), src, 0x80, 3);
        return;
    }
    throw std::invalid_argument("World native record layout is unsupported");
}
}

WorldRecordStorage PrepareWorldRecordStorage(WorldRecordStorageLayout layout,
                                            const void* raw_record) {
    const Layout descriptor = Describe(layout);
    if (descriptor.alignment > alignof(Header))
        throw std::invalid_argument("World native record alignment exceeds its backing contract");
    GameCompletedSpan completed{};
    if (!raw_record || !FindGameCompletedSpan(raw_record, descriptor.wire_bytes, completed)
        || FindGameByteDomain(raw_record, descriptor.wire_bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("World native record requires unchanged completed Wii source bytes");

    GameNativeBackingSpan existing{};
    const std::size_t backing_bytes = sizeof(Header) + descriptor.native_bytes;
    if (FindGameNativeBacking(raw_record, descriptor.wire_bytes, existing)) {
        Header header{};
        if (existing.bytes != backing_bytes)
            throw std::invalid_argument("World native record backing layout conflicts with an existing source view");
        std::memcpy(&header, existing.data, sizeof(header));
        if (header.magic != Magic || header.layout != layout
            || header.native_bytes != descriptor.native_bytes || header.wire_bytes != descriptor.wire_bytes)
            throw std::invalid_argument("World native record backing layout conflicts with an existing source view");
        return {static_cast<unsigned char*>(existing.data) + sizeof(Header),
                descriptor.native_bytes, descriptor.wire_bytes};
    }

    GameNativeBackingReservation reservation(raw_record, descriptor.wire_bytes, backing_bytes);
    auto* bytes = static_cast<unsigned char*>(reservation.Data());
    Header header{Magic, layout, static_cast<std::uint32_t>(descriptor.native_bytes),
                  static_cast<std::uint32_t>(descriptor.wire_bytes)};
    std::memcpy(bytes, &header, sizeof(header));
    auto* native = bytes + sizeof(Header);
    // Padding and unconstructed vptr bytes have no game meaning. No fake vtable
    // or initialized object is published; a real placement constructor is next.
    std::memset(native, 0, descriptor.native_bytes);
    Decode(layout, native, static_cast<const unsigned char*>(raw_record));
    reservation.Commit();
    return {native, descriptor.native_bytes, descriptor.wire_bytes};
}
}
