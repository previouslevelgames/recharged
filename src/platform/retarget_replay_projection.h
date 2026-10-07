#pragma once
#include <cstdint>
struct AnimRetarget;
namespace mscharged::platform {
// Same-process original four-byte record address, requiring a current live
// raw owner and its already-initialized typed native record. No owner lease.
std::uint32_t EncodeRetargetReplayAddress(const AnimRetarget* record);
const AnimRetarget* DecodeRetargetReplayAddress(std::uint32_t address);
}
