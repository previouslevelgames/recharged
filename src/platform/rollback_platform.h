#pragma once

#include <cstddef>
#include <cstdint>

// What the rollback (rollback_engine.cpp and its files) needs from the system, on Linux,
// macOS and Windows. Nothing here allocates with the game's operator new.
namespace mscharged::platform
{
// Return addresses of the calls under way, the caller's first.
int RollbackCallStack(void** frames, int capacity);
// The base address of the module (executable or library) that holds address;
// 0 when none does.
std::uintptr_t RollbackModuleBase(const void* address);
// The writable segments (ELF) or sections (PE) of a loaded module, as
// [start, end) pairs; returns how many.
int RollbackWritableSegments(std::uintptr_t moduleBase, std::uintptr_t (*segments)[2], int capacity);
// Pages of zeros, outside every allocator; null when there is no room.
void* RollbackMapPages(std::size_t bytes);
void RollbackUnmapPages(void* pages, std::size_t bytes);
// Runs body on a thread of its own, never joined.
bool RollbackStartThread(void (*body)());
void RollbackSleepMs(unsigned milliseconds);
} // namespace mscharged::platform
