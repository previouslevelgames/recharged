#pragma once

namespace mscharged
{
// The reconstructed constructor requests one Wii pending record. Allocate its
// native layout through the same original game allocator and fromEnd argument.
void* AllocateNativeTweakPending();
}
