#pragma once

#include <cstdint>

// Native representation of the original 32-bit, pointer-shaped NL handle.
// Calls remain on the original file servicing thread. Metadata is host owned;
// the original source owns every file, destination, callback and decision.
namespace mscharged::platform {
struct FileLoadLease;
using FileLoadCleanup = void (*)(void*);
FileLoadLease* ReserveFileLoad(void* context, void* file, FileLoadCleanup cleanup);
std::uint32_t BindFileLoad(FileLoadLease* lease, void* actualReadEntry);
void* ResolveFileReadHandle(std::uint32_t handle);
void FinishFileLoad(void* context);
void AbortFileLoad(void* context);
void AbortFileLoadsForFile(void* file);
void ShutdownFileLoads();
}
