#pragma once

#include <cstddef>

namespace mscharged::platform {
// Native transport of the serialized Wii image. The returned backing is tied
// to the exact original raw allocation, not owned by a new game manager.
void* PrepareNativePackedRegistryImage(const void* raw, std::size_t bytes);
}
