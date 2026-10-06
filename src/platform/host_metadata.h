#pragma once

#include <cstddef>

// The host defines these C interfaces before loading original game code.
// Metadata stays in the host CRT even when game operators use the NL arenas.
extern "C" void* ChargedNativeMetadataAllocate(std::size_t bytes);
extern "C" void ChargedNativeMetadataRelease(void* pointer) noexcept;
