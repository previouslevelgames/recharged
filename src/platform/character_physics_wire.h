#pragma once

#include <cstdint>

struct CharacterPhysicsElement;

namespace mscharged::platform {
// Original character physics chunks keep their Wii big-endian words. The
// original loader keeps its chunk walk, allocation and copy loop; these read
// its element count and expand one 0xA0 record into native word order. Names
// stay byte strings. Neither call selects, allocates or validates game data.
std::uint32_t ReadCharacterPhysicsWord(const void* source);
void ReadCharacterPhysicsElement(CharacterPhysicsElement& target, const void* source);
}
