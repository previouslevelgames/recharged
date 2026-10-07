#pragma once

#include <cstdint>

class cSAnim;

namespace mscharged::platform {
// Typed original animation-header address word, never a native CRT pointer ID.
// The original raw source owner must stay live throughout Replay use. Neither
// call owns it or distinguishes an earlier pointer after address reuse.
std::uint32_t EncodeSAnimReplayAddress(const cSAnim* animation);
cSAnim* DecodeSAnimReplayAddress(std::uint32_t addressWord);
}
