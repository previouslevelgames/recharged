#include "platform/tweak_storage.h"
#include "Game/TweakRegistry.h"
#include "NL/nlMemory.h"

namespace mscharged
{
void* AllocateNativeTweakPending()
{
    return nlMalloc(sizeof(TweakPendingValue), 8, true);
}
}
