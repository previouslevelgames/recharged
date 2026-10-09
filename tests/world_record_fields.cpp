// Pure ABI projection: actual declarations and transport, no game construction.
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
#include <revolution/os/OSTime_fwd.h>

#include <array>
#include <cstdio>
#include <ctime>
#include <type_traits>
#include <vector>

// Contain the actual platform translation unit in the fixture, so its unused
// ownership API is internal and needs no substituted registry/provider. The
// original field-decoding bodies and global game declarations stay unchanged.
namespace {
namespace codec {
namespace mscharged::platform { using namespace ::mscharged::platform; }
#include "../src/platform/world_record_storage.cpp"
}
}
namespace Transport = codec::mscharged::platform;

using AllocationSignature = void* (*)(std::size_t);
// Clang encodes implicit noexcept for these deallocators; GCC may omit it.
// Both must accept the exact native size type and return void.
using DeallocationSignature = void (*)(void*, std::size_t);
static_assert(std::is_same_v<decltype(&Function0<void>::FunctorBase::operator new), AllocationSignature>);
static_assert(std::is_same_v<decltype(&Function1<void, int>::FunctorBase::operator new), AllocationSignature>);
static_assert(std::is_same_v<decltype(&Function2<void, int, int>::FunctorBase::operator new), AllocationSignature>);
static_assert(std::is_same_v<decltype(&Function3<void, int, int, int>::FunctorBase::operator new), AllocationSignature>);
static_assert(std::is_convertible_v<decltype(&Function0<void>::FunctorBase::operator delete), DeallocationSignature>);
static_assert(std::is_convertible_v<decltype(&Function1<void, int>::FunctorBase::operator delete), DeallocationSignature>);
static_assert(std::is_convertible_v<decltype(&Function2<void, int, int>::FunctorBase::operator delete), DeallocationSignature>);
static_assert(std::is_convertible_v<decltype(&Function3<void, int, int, int>::FunctorBase::operator delete), DeallocationSignature>);
static_assert(std::is_same_v<decltype(&cPN_SAnimController::operator new), AllocationSignature>);
static_assert(sizeof(void*) == 8 && sizeof(std::size_t) == 8);
static_assert(sizeof(OSTime) == 8 && sizeof(OSTick) == 4 && sizeof(u32) == 4);
#if defined(_WIN32)
static_assert(sizeof(long) == 4 && sizeof(std::time_t) == 8);
#else
static_assert(sizeof(long) == 8);
#endif

namespace {
unsigned checks;
void Check(bool passed) {
    if (!passed) throw std::runtime_error("World native field projection failed");
    ++checks;
}
void PutWord(unsigned char* p, std::uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
template<class T> void Field(mscharged::platform::WorldRecordStorageLayout layout,
                            std::size_t offset, std::size_t wire, std::uint32_t value) {
    const auto description = Transport::Describe(layout);
    std::vector<unsigned char> raw(description.wire_bytes, 0);
    std::vector<unsigned char> native(description.native_bytes + 16, 0xAC);
    PutWord(raw.data() + wire, value);
    const auto before = raw;
    Transport::Decode(layout, native.data(), raw.data());
    T result{};
    std::memcpy(&result, native.data() + offset, sizeof(result));
    if constexpr (std::is_pointer_v<T>)
        Check(reinterpret_cast<std::uintptr_t>(result) == value);
    else
        Check(result == value);
    Check(raw == before);
    for (std::size_t i = description.native_bytes; i < native.size(); ++i)
        Check(native[i] == 0xAC);
}
template<class T> void PrefixFields(mscharged::platform::WorldRecordStorageLayout layout,
                                    std::size_t context, std::size_t matrix, std::uint32_t value) {
    // Native declarations preserve the Wii32 numeric prefix and full native
    // pointers. Source placement still constructs the object after this decode.
    static_assert(std::is_same_v<decltype(T::m_uHashID), u32>);
    static_assert(std::is_same_v<decltype(T::m_uObjectType), u32>);
    static_assert(std::is_same_v<decltype(T::m_uObjectCreationFlags), u32>);
    static_assert(offsetof(T, m_uHashID) == offsetof(WorldDrawable, m_uHashID));
    static_assert(offsetof(T, m_uObjectType) == offsetof(WorldDrawable, m_uObjectType));
    static_assert(offsetof(T, m_uObjectCreationFlags) == offsetof(WorldDrawable, m_uObjectCreationFlags));
    static_assert(offsetof(T, m_nAnimNode) == offsetof(WorldDrawable, m_nAnimNode));
    static_assert(offsetof(T, m_pAnimController) == offsetof(WorldDrawable, m_pAnimController));
    static_assert(offsetof(T, m_pad1C) == offsetof(WorldDrawable, m_pad1C));
    Check(context == offsetof(WorldDrawable, m_pWorldContext));
    Check(matrix == offsetof(WorldDrawable, mWorldMatrix));
    Field<u32>(layout, offsetof(T, m_uHashID), 4, value);
    Field<u32>(layout, offsetof(T, m_uObjectType), 8, value);
    Field<u32>(layout, offsetof(T, m_uObjectCreationFlags), 12, value);
    Field<World*>(layout, context, 0x10, value);
    Field<int>(layout, offsetof(T, m_nAnimNode), 0x14, value);
    Field<WorldAnimController*>(layout, offsetof(T, m_pAnimController), 0x18, value);
    // Compare every matrix scalar's representation without interpreting float
    // values or dereferencing the still-serialized pointer words.
    for (unsigned i = 0; i < 16; ++i)
        Field<std::uint32_t>(layout, matrix + 4 * i, 0x20 + 4 * i, value);
}
}

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
extern "C" EXPORT unsigned CheckWorldRecordNativeAbi() {
    using namespace Transport;
    checks = 0;
    for (auto value : {0u, 0x80000000u, 0xFFFFFFFFu}) {
        PrefixFields<WorldHelperObject>(WorldRecordStorageLayout::CommonObject,
            offsetof(WorldHelperObject, m_pWorld), offsetof(WorldHelperObject, mWorldMatrix), value);
        PrefixFields<CrowdLayoutObject>(WorldRecordStorageLayout::Crowd,
            offsetof(CrowdLayoutObject, m_pWorldContext), offsetof(CrowdLayoutObject, mTransform), value);
        PrefixFields<WorldNPC>(WorldRecordStorageLayout::NPC,
            offsetof(WorldNPC, m_pWorldContext), offsetof(WorldNPC, mTransform), value);
        Field<decltype(WorldPhysicsDrawable::m_uObjectCreationFlags)>(WorldRecordStorageLayout::Physics,
            offsetof(WorldPhysicsDrawable, m_uObjectCreationFlags), 0x0C, value);
        Field<decltype(WorldPhysicsDescription::uPrimitiveType)>(WorldRecordStorageLayout::Physics,
            offsetof(WorldPhysicsDrawable, m_Description) + offsetof(WorldPhysicsDescription, uPrimitiveType), 0x60, value);
        Field<decltype(WorldNPC::mTemplateHash)>(WorldRecordStorageLayout::NPC,
            offsetof(WorldNPC, mTemplateHash), 0x60, value);
        Field<decltype(StadiumWorldDrawable::m_uFlags)>(WorldRecordStorageLayout::StadiumDrawable,
            offsetof(StadiumWorldDrawable, m_uFlags), 0x88, value);
        Field<decltype(SolarFlareDrawable::m_uDrawEnabled)>(WorldRecordStorageLayout::ConditionalDrawable,
            offsetof(SolarFlareDrawable, m_uDrawEnabled), 0x70, value);
        Field<decltype(StadiumCupTrophyDrawable::m_uCupTrophyKey)>(WorldRecordStorageLayout::CupTrophy,
            offsetof(StadiumCupTrophyDrawable, m_uCupTrophyKey), 0x70, value);
        Field<decltype(WorldDrawable::m_pModel)>(WorldRecordStorageLayout::Drawable,
            offsetof(WorldDrawable, m_pModel), 0x64, value);
        Field<decltype(WorldAnimObject::m_pAnimationHashes)>(WorldRecordStorageLayout::Animation,
            offsetof(WorldAnimObject, m_pAnimationHashes), 0x74, value);

        // Verify the exact typed write footprint, including the adjacent bytes
        // which an unconditional eight-byte scalar write would overwrite.
        std::array<unsigned char, 24> destination; destination.fill(0xAC);
        std::array<unsigned char, 4> raw{}; PutWord(raw.data(), value);
        WordLong(destination.data(), 4, raw.data(), 0);
        unsigned long result{}; std::memcpy(&result, destination.data() + 4, sizeof(result));
        Check(result == value);
        for (std::size_t i = 0; i < destination.size(); ++i)
            if (i < 4 || i >= 4 + sizeof(result)) Check(destination[i] == 0xAC);

        // LP64 retains precisely the previous helper's byte representation.
        if constexpr (sizeof(unsigned long) == sizeof(std::uintptr_t)) {
            auto expected = destination;
            Word8(expected.data(), 4, raw.data(), 0);
            Check(expected == destination);
        }
    }
    std::printf("World record ABI: %u checks, pointer=%zu long=%zu size_t=%zu time_t=%zu OSTime=%zu OSTick=%zu\n",
                checks, sizeof(void*), sizeof(long), sizeof(std::size_t), sizeof(std::time_t), sizeof(OSTime), sizeof(OSTick));
    return checks;
}

#if !defined(MSCHARGED_WORLD_RECORD_FIELDS_MODULE)
int main() {
    try {
        return CheckWorldRecordNativeAbi() >= 400 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "World record ABI: %s\n", error.what());
        return 1;
    }
}
#endif
