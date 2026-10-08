#pragma once

#include <cstddef>

class WorldDrawable;
class WorldVisibilityDrawable;
class WorldPhysicsDrawable;
class WorldObject_80129EE0;
class WorldAnimObject;
class CrowdLayoutObject;
class WorldNPC;
class WorldEffect;
class StadiumPhysicsObject;
class StadiumCupTrophyDrawable;
class StadiumWorldDrawable;
class StadiumLight;
class StadiumAttackSideIndicator;
class SolarFlareDrawable;
class StadiumFEModelMarker;
class StadiumShadowHeightMarker;
class StadiumToggleDrawable;
class StadiumShadowVolumeDrawable;
class StadiumHighRangeDrawable;

namespace mscharged::platform {

// The original factory must choose this layout at its existing placement site.
// No raw type lookup, constructor, initialization or game outcome happens here.
enum class WorldRecordStorageLayout {
    StadiumDrawable,
    StadiumHighRange,
    StadiumMarker,
    Effect,
    CupTrophy,
    Light,
    Animation,
    Drawable,
    Visibility,
    Physics,
    CommonObject,
    Crowd,
    NPC,
    StadiumPhysics,
    AttackSide,
    ConditionalDrawable,
    ShadowHeight,
    Toggle,
    ShadowVolume
};

struct WorldRecordStorage {
    void* data;
    std::size_t bytes;
    std::size_t wire_bytes;
};

// Prepare unconstructed native bytes attached to this exact completed raw
// record incarnation. Original placement constructors still own vptr writes.
// Four-byte address/hash carriers are widened numerically; this does not make
// them live native addresses. Actual source requests must replace them before
// dereferencing. The source raw cursor, parent and flags remain untouched.
WorldRecordStorage PrepareWorldRecordStorage(WorldRecordStorageLayout layout,
                                            const void* raw_record);

// Only the source-selected C++ type chooses a storage description. Unknown
// classes have no positive fallback. The original raw type switch, constructor,
// Initialize call, cursor increment and count increment remain in the game TU.
template <class T> struct WorldRecordStorageTraits;
#define CHARGED_WORLD_STORAGE_TYPE(Type, Layout) \
    template <> struct WorldRecordStorageTraits<Type> { \
        static constexpr auto layout = WorldRecordStorageLayout::Layout; \
    }
CHARGED_WORLD_STORAGE_TYPE(WorldDrawable, Drawable);
CHARGED_WORLD_STORAGE_TYPE(WorldVisibilityDrawable, Visibility);
CHARGED_WORLD_STORAGE_TYPE(WorldPhysicsDrawable, Physics);
CHARGED_WORLD_STORAGE_TYPE(WorldObject_80129EE0, CommonObject);
CHARGED_WORLD_STORAGE_TYPE(WorldAnimObject, Animation);
CHARGED_WORLD_STORAGE_TYPE(CrowdLayoutObject, Crowd);
CHARGED_WORLD_STORAGE_TYPE(WorldNPC, NPC);
CHARGED_WORLD_STORAGE_TYPE(WorldEffect, Effect);
CHARGED_WORLD_STORAGE_TYPE(StadiumPhysicsObject, StadiumPhysics);
CHARGED_WORLD_STORAGE_TYPE(StadiumCupTrophyDrawable, CupTrophy);
CHARGED_WORLD_STORAGE_TYPE(StadiumWorldDrawable, StadiumDrawable);
CHARGED_WORLD_STORAGE_TYPE(StadiumLight, Light);
CHARGED_WORLD_STORAGE_TYPE(StadiumAttackSideIndicator, AttackSide);
CHARGED_WORLD_STORAGE_TYPE(SolarFlareDrawable, ConditionalDrawable);
CHARGED_WORLD_STORAGE_TYPE(StadiumFEModelMarker, StadiumMarker);
CHARGED_WORLD_STORAGE_TYPE(StadiumShadowHeightMarker, ShadowHeight);
CHARGED_WORLD_STORAGE_TYPE(StadiumToggleDrawable, Toggle);
CHARGED_WORLD_STORAGE_TYPE(StadiumShadowVolumeDrawable, ShadowVolume);
CHARGED_WORLD_STORAGE_TYPE(StadiumHighRangeDrawable, StadiumHighRange);
#undef CHARGED_WORLD_STORAGE_TYPE

template <class T> T* PrepareWorldRecord(const void* raw_record) {
    return static_cast<T*>(PrepareWorldRecordStorage(
        WorldRecordStorageTraits<T>::layout, raw_record).data);
}
}
