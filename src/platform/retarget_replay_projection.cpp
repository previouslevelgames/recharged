#include "platform/retarget_replay_projection.h"
#include "platform/anim_retarget_transport.h"
#include <dolphin/os.h>
#include <stdexcept>

namespace mscharged::platform {
std::uint32_t EncodeRetargetReplayAddress(const AnimRetarget* record)
{
    if (!record) return 0;
    const auto* raw = NativeAnimRetargetRawRecord(record);
    const auto physical = OSCachedToPhysical(const_cast<void*>(raw));
    if ((physical & 0xe0000003u) || OSPhysicalToCached(physical) != raw)
        throw std::out_of_range("Retarget record has no exact aligned original cached word");
    return physical | 0x80000000u;
}
const AnimRetarget* DecodeRetargetReplayAddress(std::uint32_t address)
{
    if (!address) return nullptr;
    if ((address & 0xe0000003u) != 0x80000000u)
        throw std::out_of_range("Retarget word is not an aligned Wii cached record address");
    return NativeAnimRetargetRecordFromRaw(OSPhysicalToCached(address & 0x1fffffffu));
}
}
