#pragma once
#include <dolphin/os/OSThread.h>

#ifdef __cplusplus
extern "C" {
#endif
// The same live registry used by native SDK create/join/detach/cancel. Borrowed
// intrusive list access requires the caller's source interrupt mask throughout.
// This is not a copied list, low-memory overlay or separate reset registry.
OSThreadQueue* ChargedNativeActiveThreadQueue(void);
#ifdef __cplusplus
}
#endif
