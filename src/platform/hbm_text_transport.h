#pragma once

namespace mscharged::platform {
// Original Game/HBMManager owns the buffer, zero padding and six file callbacks.
// Convert only its completed BOM-bearing BE16 file bytes in place. The original
// set_text loop mutates those same cells and retains pointers into this owner.
// A repeat preserves NativePayload and earlier quote mutations. This grants no
// lease: the original owner must remain live/quiescent throughout parsing/use.
void* PrepareNativeHBMMessage(void* message);
}
