#include "platform/host_metadata.h"

#include <new>

// Compile and link this provider outside the original-game module. Its standard
// operators belong to the host; importing these functions never imports the
// host's C++ allocator or container objects into the game's source graph.
extern "C" void* ChargedNativeMetadataAllocate(std::size_t bytes)
{
    return ::operator new(bytes);
}

extern "C" void ChargedNativeMetadataRelease(void* pointer) noexcept
{
    ::operator delete(pointer);
}
