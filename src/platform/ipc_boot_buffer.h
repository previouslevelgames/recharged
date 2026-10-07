#pragma once

#include <cstddef>

namespace mscharged::platform {
// Bootloader-owned IPC memory, distinct from the game arenas. Install before
// original __OSInitIPCBuffer/IPCInit. The original SDK captures its pointers
// permanently; the caller must retain this storage until the source image and
// all its IOS requests retire. Replacing a live source arena is unsupported.
void InstallNativeIPCBootBuffer(void* start, std::size_t size);
}
