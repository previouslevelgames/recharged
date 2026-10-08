#include "platform/character_physics_wire.h"
#include "platform/game_allocation_ownership.h"
#include "Game/Physics/CharacterPhysicsElement.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace mscharged::platform {
namespace {
constexpr std::size_t WiiElementSize = 0xA0;
static_assert(sizeof(CharacterPhysicsElement) == WiiElementSize);
static_assert(offsetof(CharacterPhysicsElement, szName) == 0x40);
static_assert(offsetof(CharacterPhysicsElement, uHashID) == 0x60);
static_assert(offsetof(CharacterPhysicsElement, szParentName) == 0x64);
static_assert(offsetof(CharacterPhysicsElement, uParentHashID) == 0x84);
static_assert(offsetof(CharacterPhysicsElement, uPrimitiveType) == 0x88);
static_assert(offsetof(CharacterPhysicsElement, fWidth) == 0x8C);
static_assert(offsetof(CharacterPhysicsElement, uReserved) == 0x9C);
static_assert(sizeof(float) == sizeof(std::uint32_t));

void RequireWiiBytes(const void* source, std::size_t bytes)
{
    GameCompletedSpan completed{};
    if (!source || !FindGameCompletedSpan(source, bytes, completed)
        || FindGameByteDomain(source, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Character physics data requires completed Wii source bytes");
}

std::uint32_t Word(const unsigned char* p) noexcept
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
        | std::uint32_t(p[2]) << 8 | std::uint32_t(p[3]);
}

float Float(const unsigned char* p) noexcept
{
    // No float arithmetic: retain the authored bits, including NaN payloads.
    const std::uint32_t bits = Word(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
}

std::uint32_t ReadCharacterPhysicsWord(const void* source)
{
    RequireWiiBytes(source, sizeof(std::uint32_t));
    return Word(static_cast<const unsigned char*>(source));
}

void ReadCharacterPhysicsElement(CharacterPhysicsElement& target, const void* source)
{
    RequireWiiBytes(source, WiiElementSize);
    const auto* raw = static_cast<const unsigned char*>(source);
    for (std::size_t i = 0; i < 16; ++i)
        target.matLocalToParent.e[i] = Float(raw + i * 4);
    std::memcpy(target.szName, raw + 0x40, sizeof(target.szName));
    target.uHashID = Word(raw + 0x60);
    std::memcpy(target.szParentName, raw + 0x64, sizeof(target.szParentName));
    target.uParentHashID = Word(raw + 0x84);
    target.uPrimitiveType = Word(raw + 0x88);
    target.fWidth = Float(raw + 0x8C);
    target.fLength = Float(raw + 0x90);
    target.fHeight = Float(raw + 0x94);
    target.fRadius = Float(raw + 0x98);
    target.uReserved = Word(raw + 0x9C);
}
}
